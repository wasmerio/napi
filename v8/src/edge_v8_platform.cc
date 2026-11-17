#include "edge_v8_platform.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include <libplatform/libplatform.h>
#include <v8.h>

namespace {

// EdgeV8Platform::SetForegroundTaskWaker.
std::atomic<void (*)()> g_foreground_task_waker{nullptr};

void WakeForForegroundTask() {
  if (void (*waker)() = g_foreground_task_waker.load(std::memory_order_acquire)) waker();
}

// The memory protection key of V8's JIT code (EdgeV8AllowJitKeyReads), or -1.
std::atomic<int> g_jit_pkey{-1};

// The V8 background work of one instance (snapi_v8_lane_new). Only the
// embedder's dedicated thread runs it, and the bounded queue keeps a guest
// from piling up host-side Task objects while that thread is busy.
class BackgroundLane {
 public:
  BackgroundLane(size_t max_queued_tasks,
                 void* scope_context,
                 void* (*enter_scope)(void*),
                 bool (*leave_scope)(void*, void*),
                 void (*on_overload)(void*))
      : max_queued_tasks_(max_queued_tasks),
        scope_context_(scope_context),
        enter_scope_(enter_scope),
        leave_scope_(leave_scope),
        on_overload_(on_overload) {}

  bool Post(std::unique_ptr<v8::Task> task, double delay_seconds) {
    if (!task) return true;
    const double bounded_delay =
        std::isfinite(delay_seconds) ? std::clamp(delay_seconds, 0.0, 86400.0) : 0.0;
    const auto delay = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(bounded_delay));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_) return false;
      if (queue_.size() < max_queued_tasks_) {
        Item item{Clock::now() + delay, std::move(task)};
        auto it = queue_.begin();
        while (it != queue_.end() && it->due <= item.due) ++it;
        queue_.insert(it, std::move(item));
        cv_.notify_one();
        return true;
      }
      // Stop admission before telling the embedder: stopping the instance may
      // take other locks, so it never runs under ours.
      overloaded_ = true;
      stopped_ = true;
      cv_.notify_all();
    }
    if (on_overload_ != nullptr) on_overload_(scope_context_);
    return false;
  }

  void Run() {
    EdgeV8AllowJitKeyReads();
    std::unique_lock<std::mutex> lock(mutex_);
    if (running_) return;
    running_ = true;
    cv_.notify_all();
    while (!stopped_) {
      if (queue_.empty()) {
        cv_.wait(lock, [&] { return stopped_ || !queue_.empty(); });
        continue;
      }
      const Clock::time_point due = queue_.front().due;
      if (Clock::now() < due) {
        cv_.wait_until(lock, due);
        continue;
      }
      std::unique_ptr<v8::Task> task = std::move(queue_.front().task);
      queue_.pop_front();
      lock.unlock();
      void* scope = enter_scope_ != nullptr ? enter_scope_(scope_context_) : nullptr;
      if (enter_scope_ != nullptr && scope == nullptr) {
        SignalOverload();
        lock.lock();
        break;
      }
      // The lane can start before the platform allocates V8's JIT key.
      EdgeV8AllowJitKeyReads();
      task->Run();
      task.reset();
      if (leave_scope_ != nullptr && !leave_scope_(scope_context_, scope)) {
        SignalOverload();
        lock.lock();
        break;
      }
      lock.lock();
    }
    queue_.clear();
    running_ = false;
  }

  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    cv_.notify_all();
  }

  bool IsRunning() {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
  }

  bool IsOverloaded() {
    std::lock_guard<std::mutex> lock(mutex_);
    return overloaded_;
  }

 private:
  using Clock = std::chrono::steady_clock;
  struct Item {
    Clock::time_point due;
    std::unique_ptr<v8::Task> task;
  };

  void SignalOverload() {
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!overloaded_) {
        overloaded_ = true;
        notify = true;
      }
      stopped_ = true;
      cv_.notify_all();
    }
    if (notify && on_overload_ != nullptr) on_overload_(scope_context_);
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Item> queue_;
  bool running_ = false;
  bool stopped_ = false;
  bool overloaded_ = false;
  const size_t max_queued_tasks_;
  void* const scope_context_;
  void* (*const enter_scope_)(void*);
  bool (*const leave_scope_)(void*, void*);
  void (*const on_overload_)(void*);
};

thread_local BackgroundLane* current_background_lane = nullptr;
// Worker tasks run by the standalone pool, and those dropped because no lane
// was bound in a process without one.
std::atomic<uint64_t> standalone_worker_posts{0};
std::atomic<uint64_t> unattributed_worker_posts{0};

struct WorkerWarmupState {
  std::mutex mutex;
  std::condition_variable cv;
  size_t pending = 0;
};

class WorkerWarmupTask final : public v8::Task {
 public:
  explicit WorkerWarmupTask(WorkerWarmupState* state) : state_(state) {}

  void Run() override {
    if (state_ == nullptr) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->pending > 0) {
      state_->pending -= 1;
    }
    if (state_->pending == 0) {
      state_->cv.notify_all();
    }
  }

 private:
  WorkerWarmupState* state_ = nullptr;
};

void WarmUpFallbackWorkerThreads(v8::Platform* fallback) {
  if (fallback == nullptr) return;

  const int worker_threads = fallback->NumberOfWorkerThreads();
  if (worker_threads <= 0) return;

  WorkerWarmupState state;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pending = static_cast<size_t>(worker_threads) + 1;
  }

  for (int i = 0; i < worker_threads; ++i) {
    fallback->PostTaskOnWorkerThread(
        v8::TaskPriority::kUserVisible,
        std::make_unique<WorkerWarmupTask>(&state));
  }
  fallback->PostDelayedTaskOnWorkerThread(
      v8::TaskPriority::kUserVisible,
      std::make_unique<WorkerWarmupTask>(&state),
      0);

  std::unique_lock<std::mutex> lock(state.mutex);
  while (state.pending != 0) {
    state.cv.wait(lock);
  }
}

struct ForegroundTaskRecord {
  std::shared_ptr<EdgeV8Platform::IsolateState> isolate_state;
  std::unique_ptr<v8::Task> task;
};

void RunForegroundTaskRecord(napi_env /*env*/, void* data);
void CleanupForegroundTaskRecord(napi_env /*env*/, void* data);

}  // namespace

void EdgeV8AllowJitKeyReads() {
#if defined(__linux__) && defined(__x86_64__)
  const int key = g_jit_pkey.load(std::memory_order_acquire);
  // Key 0 is the default key every thread may use.
  if (key <= 0 || key >= 16) return;
  // What glibc's pkey_set(key, PKEY_DISABLE_WRITE) does: RDPKRU, then WRPKRU
  // with the key's two bits set to write-disabled. A key exists only where
  // the CPU and kernel support them, so these instructions don't fault.
  uint32_t pkru;
  __asm__ volatile(".byte 0x0f, 0x01, 0xee" : "=a"(pkru) : "c"(0) : "rdx");
  constexpr uint32_t kDisableWrite = 0x2;
  const uint32_t shift = 2 * static_cast<uint32_t>(key);
  const uint32_t updated = (pkru & ~(uint32_t{3} << shift)) | (kDisableWrite << shift);
  if (updated != pkru) {
    __asm__ volatile(".byte 0x0f, 0x01, 0xef" : : "a"(updated), "c"(0), "d"(0) : "memory");
  }
#endif
}

extern "C" void* snapi_v8_lane_new(size_t max_queued_tasks,
                                   void* scope_context,
                                   void* (*enter_scope)(void*),
                                   bool (*leave_scope)(void*, void*),
                                   void (*on_overload)(void*)) {
  if (max_queued_tasks == 0) return nullptr;
  return new (std::nothrow)
      BackgroundLane(max_queued_tasks, scope_context, enter_scope, leave_scope, on_overload);
}

extern "C" void snapi_v8_lane_run(void* handle) {
  if (handle == nullptr) return;
  auto* lane = static_cast<BackgroundLane*>(handle);
  // Work a task posts from the lane, such as a job's next worker, stays on it.
  BackgroundLane* previous = current_background_lane;
  current_background_lane = lane;
  lane->Run();
  current_background_lane = previous;
}

extern "C" void snapi_v8_lane_stop(void* handle) {
  if (handle != nullptr) static_cast<BackgroundLane*>(handle)->Stop();
}

extern "C" void snapi_v8_lane_delete(void* handle) {
  delete static_cast<BackgroundLane*>(handle);
}

extern "C" void* snapi_v8_lane_swap_current(void* handle) {
  BackgroundLane* previous = current_background_lane;
  current_background_lane = static_cast<BackgroundLane*>(handle);
  return previous;
}

extern "C" void* snapi_v8_lane_current() { return current_background_lane; }

extern "C" bool snapi_v8_lane_is_running(void* handle) {
  return handle != nullptr && static_cast<BackgroundLane*>(handle)->IsRunning();
}

extern "C" bool snapi_v8_lane_overloaded(void* handle) {
  return handle != nullptr && static_cast<BackgroundLane*>(handle)->IsOverloaded();
}

// Test hooks, not reachable from guests.
extern "C" uint64_t snapi_v8_standalone_worker_posts() {
  return standalone_worker_posts.load(std::memory_order_acquire);
}

extern "C" uint64_t snapi_v8_unattributed_worker_posts() {
  return unattributed_worker_posts.load(std::memory_order_acquire);
}

// Posts a task that calls `callback(data)` to the lane, through V8's Task type.
extern "C" bool snapi_v8_lane_post_test_task(void* handle, void (*callback)(void*), void* data) {
  if (handle == nullptr || callback == nullptr) return false;
  class CallbackTask final : public v8::Task {
   public:
    CallbackTask(void (*callback)(void*), void* data) : callback_(callback), data_(data) {}
    void Run() override { callback_(data_); }

   private:
    void (*callback_)(void*);
    void* data_;
  };
  return static_cast<BackgroundLane*>(handle)->Post(
      std::make_unique<CallbackTask>(callback, data), 0.0);
}

struct EdgeV8Platform::FinishedCallback {
  void (*callback)(void*) = nullptr;
  void* data = nullptr;
};

struct EdgeV8Platform::IsolateState {
  IsolateState(EdgeV8Platform* platform_in,
               v8::Isolate* isolate_in,
               std::shared_ptr<ForegroundTaskRunner> runner_in,
               bool standalone_workers_in)
      : platform(platform_in),
        isolate(isolate_in),
        runner(std::move(runner_in)),
        standalone_workers(standalone_workers_in) {}

  EdgeV8Platform* platform = nullptr;
  v8::Isolate* isolate = nullptr;
  std::shared_ptr<ForegroundTaskRunner> runner;
  // Created with no lane bound: the standalone pool runs its worker tasks.
  const bool standalone_workers = false;
  std::mutex mutex;
  size_t pending_foreground_tasks = 0;
  bool shutdown_started = false;
  bool finished = false;
  std::vector<FinishedCallback> finished_callbacks;
};

namespace {

class CountedForegroundTask final : public v8::Task {
 public:
  CountedForegroundTask(std::shared_ptr<EdgeV8Platform::IsolateState> isolate_state,
                        std::unique_ptr<v8::Task> task)
      : isolate_state_(std::move(isolate_state)),
        task_(std::move(task)) {}

  ~CountedForegroundTask() override { Finish(); }

  void Run() override {
    if (task_) {
      task_->Run();
      task_.reset();
    }
    Finish();
  }

 private:
  void Finish() {
    if (finished_.exchange(true, std::memory_order_acq_rel)) return;
    std::shared_ptr<EdgeV8Platform::IsolateState> isolate_state =
        std::move(isolate_state_);
    task_.reset();
    if (isolate_state && isolate_state->platform != nullptr) {
      isolate_state->platform->CompletePendingForegroundTask(isolate_state);
    }
  }

  std::shared_ptr<EdgeV8Platform::IsolateState> isolate_state_;
  std::unique_ptr<v8::Task> task_;
  std::atomic<bool> finished_ {false};
};

void RunForegroundTaskRecord(napi_env /*env*/, void* data) {
  auto* record = static_cast<ForegroundTaskRecord*>(data);
  if (record != nullptr && record->task) {
    record->task->Run();
  }
}

void CleanupForegroundTaskRecord(napi_env /*env*/, void* data) {
  auto* record = static_cast<ForegroundTaskRecord*>(data);
  if (record == nullptr) {
    delete record;
    return;
  }
  record->task.reset();
  std::shared_ptr<EdgeV8Platform::IsolateState> isolate_state =
      std::move(record->isolate_state);
  if (isolate_state && isolate_state->platform != nullptr) {
    isolate_state->platform->CompletePendingForegroundTask(isolate_state);
  }
  delete record;
}

}  // namespace

class EdgeV8Platform::ForegroundTaskRunner final : public v8::TaskRunner {
 public:
  ForegroundTaskRunner(std::shared_ptr<IsolateState> isolate_state,
                       v8::Isolate* isolate,
                       v8::Platform* fallback)
      : isolate_state_(std::move(isolate_state)),
        isolate_(isolate),
        fallback_(fallback) {}

  bool IdleTasksEnabled() override { return false; }
  bool NonNestableTasksEnabled() const override { return true; }
  bool NonNestableDelayedTasksEnabled() const override { return true; }

 protected:
  void PostTaskImpl(std::unique_ptr<v8::Task> task,
                    const v8::SourceLocation& location) override {
    PostTaskCommon(std::move(task), 0, location);
  }

  void PostNonNestableTaskImpl(std::unique_ptr<v8::Task> task,
                               const v8::SourceLocation& location) override {
    PostTaskCommon(std::move(task), 0, location);
  }

  void PostDelayedTaskImpl(std::unique_ptr<v8::Task> task,
                           double delay_in_seconds,
                           const v8::SourceLocation& location) override {
    uint64_t delay_ms = 0;
    if (delay_in_seconds > 0) {
      delay_ms = static_cast<uint64_t>(std::llround(delay_in_seconds * 1000.0));
    }
    PostTaskCommon(std::move(task), delay_ms, location);
  }

  void PostNonNestableDelayedTaskImpl(
      std::unique_ptr<v8::Task> task,
      double delay_in_seconds,
      const v8::SourceLocation& location) override {
    PostDelayedTaskImpl(std::move(task), delay_in_seconds, location);
  }

  void PostIdleTaskImpl(std::unique_ptr<v8::IdleTask> /*task*/,
                        const v8::SourceLocation& /*location*/) override {}

 private:
  void PostTaskCommon(std::unique_ptr<v8::Task> task,
                      uint64_t delay_ms,
                      const v8::SourceLocation& location) {
    if (!task) return;
    if (shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    std::shared_ptr<IsolateState> isolate_state = isolate_state_.lock();

    unofficial_napi_enqueue_foreground_task_callback enqueue =
        target_enqueue_.load(std::memory_order_acquire);
    void* target = target_data_.load(std::memory_order_acquire);
    if (enqueue != nullptr && target != nullptr) {
      ForegroundTaskRecord* record = new (std::nothrow) ForegroundTaskRecord();
      if (record != nullptr) {
        // The hook owns the record from the moment it is called, whatever it
        // returns (see unofficial_napi_enqueue_foreground_task_callback): it
        // may refuse and release the record, or have another thread run and
        // release it, before the call returns. So the record is complete,
        // including the pending-task count its cleanup gives back, before it
        // is handed over, and it is never touched afterwards. A refused task
        // is dropped rather than rerouted; V8 tolerates dropped foreground
        // tasks.
        record->task = std::move(task);
        if (isolate_state != nullptr && isolate_state->platform != nullptr) {
          isolate_state->platform->AddPendingForegroundTask(isolate_state);
          record->isolate_state = isolate_state;
        }
        (void)enqueue(target,
                      RunForegroundTaskRecord,
                      record,
                      CleanupForegroundTaskRecord,
                      delay_ms);
        return;
      }
    }

    auto runner = fallback_ != nullptr ? fallback_->GetForegroundTaskRunner(isolate_) : nullptr;
    if (runner) {
      used_fallback_runner_.store(true, std::memory_order_release);
      std::unique_ptr<v8::Task> counted_task = std::move(task);
      if (isolate_state != nullptr && isolate_state->platform != nullptr) {
        isolate_state->platform->AddPendingForegroundTask(isolate_state);
        counted_task =
            std::make_unique<CountedForegroundTask>(isolate_state, std::move(counted_task));
      }
      if (delay_ms == 0) {
        runner->PostTask(std::move(counted_task), location);
        // Only a checkpoint runs it (PumpPendingForegroundTasks): wake the
        // embedder so that one comes now. A delayed task waits for whatever
        // checkpoint follows its delay, as before.
        WakeForForegroundTask();
      } else {
        runner->PostDelayedTask(std::move(counted_task), delay_ms / 1000.0, location);
      }
    }
  }

  std::weak_ptr<IsolateState> isolate_state_;
  v8::Isolate* isolate_ = nullptr;
  v8::Platform* fallback_ = nullptr;
  std::atomic<napi_env> target_env_ {nullptr};
  std::atomic<unofficial_napi_enqueue_foreground_task_callback> target_enqueue_ {nullptr};
  std::atomic<void*> target_data_ {nullptr};
  std::atomic<bool> shutting_down_ {false};
  std::atomic<bool> used_fallback_runner_ {false};

 public:
  ~ForegroundTaskRunner() override = default;

  void BindTarget(napi_env env,
                  unofficial_napi_enqueue_foreground_task_callback callback,
                  void* target) {
    target_data_.store(target, std::memory_order_release);
    target_enqueue_.store(callback, std::memory_order_release);
    target_env_.store(env, std::memory_order_release);
  }

  void ClearTarget(napi_env env) {
    if (target_env_.load(std::memory_order_acquire) != env) return;
    target_env_.store(nullptr, std::memory_order_release);
    target_enqueue_.store(nullptr, std::memory_order_release);
    target_data_.store(nullptr, std::memory_order_release);
  }

  void NotifyIsolateShutdown() {
    shutting_down_.store(true, std::memory_order_release);
    target_env_.store(nullptr, std::memory_order_release);
    target_enqueue_.store(nullptr, std::memory_order_release);
    target_data_.store(nullptr, std::memory_order_release);
  }

  bool used_fallback_runner() const {
    return used_fallback_runner_.load(std::memory_order_acquire);
  }
};

namespace {
// Worker threads for the shared platform: 0 means "let V8 decide". Read once,
// when the platform is built.
std::atomic<int> g_worker_thread_count{0};
std::atomic<bool> g_platform_created{false};
std::atomic<bool> g_standalone_pool_created{false};
}  // namespace

// Test hooks, not reachable from guests.
extern "C" bool snapi_v8_platform_created() {
  return g_platform_created.load(std::memory_order_acquire);
}

extern "C" bool snapi_v8_standalone_pool_created() {
  return g_standalone_pool_created.load(std::memory_order_acquire);
}

bool EdgeV8Platform::SetWorkerThreadCount(int count) {
  const int requested = count < 0 ? 0 : count;
  if (g_platform_created.load(std::memory_order_acquire)) {
    // The pool is already built. Asking for what is already in place is not an
    // error -- several embedder components may configure the runtime from the
    // same setting -- but asking for anything else cannot be honoured.
    return g_worker_thread_count.load(std::memory_order_acquire) == requested;
  }
  g_worker_thread_count.store(requested, std::memory_order_release);
  return true;
}

void EdgeV8Platform::SetForegroundTaskWaker(void (*waker)()) {
  g_foreground_task_waker.store(waker, std::memory_order_release);
}

std::unique_ptr<EdgeV8Platform> EdgeV8Platform::Create(bool standalone_workers) {
  g_platform_created.store(true, std::memory_order_release);
  // The delegate supplies foreground runners, clocks and allocators. V8 sees
  // EdgeV8Platform, which handles every worker and job entry point itself, so
  // the delegate's own pool would only run work no instance pays for.
  std::unique_ptr<v8::Platform> fallback = v8::platform::NewSingleThreadedDefaultPlatform();
  if (!fallback) return nullptr;
  // V8 takes its JIT key from this allocator (GetThreadIsolatedAllocator).
  if (v8::ThreadIsolatedAllocator* allocator = fallback->GetThreadIsolatedAllocator();
      allocator != nullptr && allocator->Type() == v8::ThreadIsolatedAllocator::Type::kPkey) {
    g_jit_pkey.store(allocator->Pkey(), std::memory_order_release);
  }
  auto platform = std::unique_ptr<EdgeV8Platform>(new EdgeV8Platform(std::move(fallback)));
  if (standalone_workers && !platform->EnableStandaloneWorkers()) return nullptr;
  return platform;
}

EdgeV8Platform::EdgeV8Platform(std::unique_ptr<v8::Platform> fallback)
    : fallback_(std::move(fallback)) {}

EdgeV8Platform::~EdgeV8Platform() = default;

bool EdgeV8Platform::EnableStandaloneWorkers() {
  std::lock_guard<std::mutex> lock(standalone_workers_mutex_);
  if (standalone_workers_ != nullptr) return true;
  // The pool's threads inherit this thread's key rights.
  EdgeV8AllowJitKeyReads();
  std::unique_ptr<v8::Platform> workers =
      v8::platform::NewDefaultPlatform(g_worker_thread_count.load(std::memory_order_acquire));
  if (!workers) return false;
  WarmUpFallbackWorkerThreads(workers.get());
  standalone_workers_ = std::move(workers);
  standalone_workers_ptr_.store(standalone_workers_.get(), std::memory_order_release);
  g_standalone_pool_created.store(true, std::memory_order_release);
  return true;
}

v8::Platform* EdgeV8Platform::StandaloneWorkers() {
  return standalone_workers_ptr_.load(std::memory_order_acquire);
}

std::shared_ptr<EdgeV8Platform::IsolateState> EdgeV8Platform::GetState(v8::Isolate* isolate) {
  if (isolate == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = isolates_.find(isolate);
  return it != isolates_.end() ? it->second : nullptr;
}

std::shared_ptr<EdgeV8Platform::IsolateState> EdgeV8Platform::EnsureState(v8::Isolate* isolate) {
  if (isolate == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = isolates_.find(isolate);
  if (it != isolates_.end()) return it->second;
  if (fallback_ != nullptr) {
    // libplatform's NotifyIsolateShutdown() assumes a foreground runner entry
    // exists for every isolate it tears down.
    (void)fallback_->GetForegroundTaskRunner(isolate);
  }
  const bool standalone_workers =
      current_background_lane == nullptr && StandaloneWorkers() != nullptr;
  if (standalone_workers) {
    // The same holds for the pool that runs this isolate's worker tasks.
    (void)StandaloneWorkers()->GetForegroundTaskRunner(isolate);
  }
  auto state = std::make_shared<IsolateState>(this, isolate, nullptr, standalone_workers);
  state->runner = std::make_shared<ForegroundTaskRunner>(state, isolate, fallback_.get());
  isolates_.emplace(isolate, state);
  return state;
}

std::shared_ptr<EdgeV8Platform::ForegroundTaskRunner> EdgeV8Platform::EnsureRunner(v8::Isolate* isolate) {
  std::shared_ptr<IsolateState> state = EnsureState(isolate);
  return state != nullptr ? state->runner : nullptr;
}

bool EdgeV8Platform::RegisterIsolate(v8::Isolate* isolate) { return EnsureRunner(isolate) != nullptr; }

void EdgeV8Platform::AddPendingForegroundTask(const std::shared_ptr<IsolateState>& state) {
  if (!state) return;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->finished) return;
  state->pending_foreground_tasks += 1;
}

void EdgeV8Platform::CompletePendingForegroundTask(const std::shared_ptr<IsolateState>& state) {
  if (!state) return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->pending_foreground_tasks > 0) {
      state->pending_foreground_tasks -= 1;
    }
  }
  MaybeFinishIsolate(state, false);
}

void EdgeV8Platform::BeginShutdown(const std::shared_ptr<IsolateState>& state) {
  if (!state) return;
  std::lock_guard<std::mutex> lock(state->mutex);
  state->shutdown_started = true;
}

void EdgeV8Platform::MaybeFinishIsolate(const std::shared_ptr<IsolateState>& state,
                                        bool begin_shutdown) {
  if (!state) return;

  std::vector<FinishedCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (begin_shutdown) {
      state->shutdown_started = true;
    }
    if (state->finished ||
        !state->shutdown_started ||
        state->pending_foreground_tasks != 0) {
      return;
    }
    state->finished = true;
    callbacks.swap(state->finished_callbacks);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolates_.find(state->isolate);
    if (it != isolates_.end() && it->second.get() == state.get()) {
      isolates_.erase(it);
    }
  }

  for (const FinishedCallback& callback : callbacks) {
    if (callback.callback != nullptr) {
      callback.callback(callback.data);
    }
  }
}

void EdgeV8Platform::AddIsolateFinishedCallback(v8::Isolate* isolate,
                                                void (*callback)(void*),
                                                void* data) {
  if (callback == nullptr) return;
  std::shared_ptr<IsolateState> state;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolates_.find(isolate);
    if (it != isolates_.end()) {
      state = it->second;
    }
  }
  if (!state) {
    callback(data);
    return;
  }

  bool run_now = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->finished) {
      run_now = true;
    } else {
      state->finished_callbacks.push_back(FinishedCallback{callback, data});
      run_now = state->shutdown_started && state->pending_foreground_tasks == 0;
    }
  }

  if (run_now) {
    MaybeFinishIsolate(state, false);
  }
}

void EdgeV8Platform::NotifyIsolateShutdown(v8::Isolate* isolate) {
  if (isolate == nullptr) return;

  std::shared_ptr<IsolateState> state = GetState(isolate);
  std::shared_ptr<ForegroundTaskRunner> runner = state != nullptr ? state->runner : nullptr;
  if (runner) {
    runner->NotifyIsolateShutdown();
  }
  // The delegate owns the foreground runner used before a target is bound. A
  // standalone isolate's worker tasks ran on the standalone pool; a managed
  // one's ran on its lane.
  if (fallback_ != nullptr) {
    v8::platform::NotifyIsolateShutdown(fallback_.get(), isolate);
  }
  if (state != nullptr && state->standalone_workers) {
    v8::platform::NotifyIsolateShutdown(StandaloneWorkers(), isolate);
  }
  BeginShutdown(state);
  MaybeFinishIsolate(state, false);
}

void EdgeV8Platform::DisposeIsolate(v8::Isolate* isolate) {
  if (isolate == nullptr) return;

  std::shared_ptr<IsolateState> state = GetState(isolate);
  NotifyIsolateShutdown(isolate);

  // Keep the isolate registered while it is being disposed because V8 may
  // still post tasks during teardown, then drop the map entry before the
  // address can be reused for a new isolate.
  isolate->Dispose();
  UnregisterIsolate(isolate);
  MaybeFinishIsolate(state, false);
}

void EdgeV8Platform::UnregisterIsolate(v8::Isolate* isolate) {
  if (isolate == nullptr) return;
  std::lock_guard<std::mutex> lock(mutex_);
  isolates_.erase(isolate);
}

bool EdgeV8Platform::BindForegroundTaskTarget(
    v8::Isolate* isolate,
    napi_env env,
    unofficial_napi_enqueue_foreground_task_callback callback,
    void* target) {
  std::shared_ptr<ForegroundTaskRunner> runner = EnsureRunner(isolate);
  if (!runner) return false;
  runner->BindTarget(env, callback, target);
  return true;
}

void EdgeV8Platform::ClearForegroundTaskTarget(v8::Isolate* isolate, napi_env env) {
  std::shared_ptr<IsolateState> state;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolates_.find(isolate);
    if (it == isolates_.end()) return;
    state = it->second;
  }
  if (!state || !state->runner) return;
  state->runner->ClearTarget(env);
}

bool EdgeV8Platform::PumpPendingForegroundTasks(v8::Isolate* isolate) {
  if (fallback_ == nullptr || isolate == nullptr) return false;
  // ForegroundTaskRunner::PostTaskCommon forwards to the guest's bound
  // enqueue callback when one is set, but falls back to fallback_'s own
  // GetForegroundTaskRunner() when no guest target is bound (e.g. the guest
  // drives everything through the provider event-loop checkpoint and never
  // calls BindForegroundTaskTarget). Nothing else pumps that fallback
  // runner's queue, so tasks routed there -- including V8-internal work like
  // Heap::PostFinalizationRegistryCleanupTaskIfNeeded's cleanup task -- would
  // otherwise be posted and never run. Admit one task per checkpoint so
  // self-rescheduling GC work cannot monopolize the guest's event-loop turn.
  // The caller reports progress to keep the loop runnable for the next task.
  return v8::platform::PumpMessageLoop(fallback_.get(), isolate);
}

void EdgeV8Platform::WakeForQueuedForegroundTasks(v8::Isolate* isolate) {
  std::shared_ptr<IsolateState> state = GetState(isolate);
  // Only the default queue waits for a checkpoint. Tasks posted there before
  // a target was bound stay there, so a bound target is no reason to skip.
  if (!state || !state->runner || !state->runner->used_fallback_runner()) return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->finished || state->shutdown_started || state->pending_foreground_tasks == 0) {
      return;
    }
  }
  WakeForForegroundTask();
}

int EdgeV8Platform::NumberOfWorkerThreads() {
  if (current_background_lane != nullptr) return 1;
  v8::Platform* workers = StandaloneWorkers();
  return workers != nullptr ? workers->NumberOfWorkerThreads() : 0;
}

std::shared_ptr<v8::TaskRunner> EdgeV8Platform::GetForegroundTaskRunner(
    v8::Isolate* isolate,
    v8::TaskPriority /*priority*/) {
  return EnsureRunner(isolate);
}

bool EdgeV8Platform::IdleTasksEnabled(v8::Isolate* isolate) {
  (void)isolate;
  return false;
}

double EdgeV8Platform::MonotonicallyIncreasingTime() {
  if (fallback_ != nullptr) return fallback_->MonotonicallyIncreasingTime();
  using clock = std::chrono::steady_clock;
  const auto now = clock::now().time_since_epoch();
  return std::chrono::duration<double>(now).count();
}

double EdgeV8Platform::CurrentClockTimeMillis() {
  return fallback_ != nullptr ? fallback_->CurrentClockTimeMillis()
                              : v8::Platform::SystemClockTimeMillis();
}

v8::TracingController* EdgeV8Platform::GetTracingController() {
  return fallback_ != nullptr ? fallback_->GetTracingController() : nullptr;
}

v8::PageAllocator* EdgeV8Platform::GetPageAllocator() {
  return fallback_ != nullptr ? fallback_->GetPageAllocator() : nullptr;
}

v8::ThreadIsolatedAllocator* EdgeV8Platform::GetThreadIsolatedAllocator() {
  return fallback_ != nullptr ? fallback_->GetThreadIsolatedAllocator() : nullptr;
}

void EdgeV8Platform::OnCriticalMemoryPressure() {
  if (fallback_ != nullptr) fallback_->OnCriticalMemoryPressure();
}

void EdgeV8Platform::DumpWithoutCrashing() {
  if (fallback_ != nullptr) fallback_->DumpWithoutCrashing();
}

v8::HighAllocationThroughputObserver* EdgeV8Platform::GetHighAllocationThroughputObserver() {
  if (fallback_ != nullptr) return fallback_->GetHighAllocationThroughputObserver();
  static v8::HighAllocationThroughputObserver observer;
  return &observer;
}

v8::Platform::StackTracePrinter EdgeV8Platform::GetStackTracePrinter() {
  return fallback_ != nullptr ? fallback_->GetStackTracePrinter() : nullptr;
}

std::unique_ptr<v8::ScopedBlockingCall> EdgeV8Platform::CreateBlockingScope(
    v8::BlockingType blocking_type) {
  return fallback_ != nullptr ? fallback_->CreateBlockingScope(blocking_type) : nullptr;
}

namespace {

// A job that nobody waits for gets every worker but one. libplatform runs
// worker tasks first come, first served, and a job's workers keep running
// while it has work, never yielding to tasks posted after them. A concurrent
// marker or sweeper holding every worker would hold back the worker tasks
// posted after it, such as an optimizing compile or the deletion of a
// finished one. A compile job's JSHeapBroker keeps every native context's
// Array and Object prototypes alive until the job is deleted, so a collection
// that marks its roots meanwhile keeps native contexts that are no longer used.
//
// Work someone waits for keeps the whole pool: the limit is lifted while the
// job's priority is kUserBlocking (V8 raises concurrent marking to it for the
// mark-compact pause) and once a thread joins the job. The limit is per job,
// so two background jobs running at once can still take every worker.
class JobWorkerLimit {
 public:
  JobWorkerLimit(size_t background_limit, v8::TaskPriority priority)
      : background_limit_(background_limit), limit_(LimitFor(priority)) {}

  size_t Get() const { return limit_.load(std::memory_order_relaxed); }

  // Returns true when the limit went up.
  bool FollowPriority(v8::TaskPriority priority) {
    const size_t limit = LimitFor(priority);
    return limit_.exchange(limit, std::memory_order_relaxed) < limit;
  }

  void Lift() { limit_.store(kNoLimit, std::memory_order_relaxed); }

 private:
  static constexpr size_t kNoLimit = SIZE_MAX;

  size_t LimitFor(v8::TaskPriority priority) const {
    return priority == v8::TaskPriority::kUserBlocking ? kNoLimit : background_limit_;
  }

  const size_t background_limit_;
  std::atomic<size_t> limit_;
};

class LimitedJobTask final : public v8::JobTask {
 public:
  LimitedJobTask(std::unique_ptr<v8::JobTask> task, std::shared_ptr<JobWorkerLimit> limit)
      : task_(std::move(task)), limit_(std::move(limit)) {}

  void Run(v8::JobDelegate* delegate) override { task_->Run(delegate); }

  // libplatform also caps this at the pool size (plus one for a joining thread).
  size_t GetMaxConcurrency(size_t worker_count) const override {
    return std::min(task_->GetMaxConcurrency(worker_count), limit_->Get());
  }

 private:
  std::unique_ptr<v8::JobTask> task_;
  std::shared_ptr<JobWorkerLimit> limit_;
};

class LimitedJobHandle final : public v8::JobHandle {
 public:
  LimitedJobHandle(std::unique_ptr<v8::JobHandle> handle, std::shared_ptr<JobWorkerLimit> limit)
      : handle_(std::move(handle)), limit_(std::move(limit)) {}

  void NotifyConcurrencyIncrease() override {
    started_.store(true, std::memory_order_relaxed);
    handle_->NotifyConcurrencyIncrease();
  }

  void Join() override {
    // Before joining: Join() sizes the job from GetMaxConcurrency().
    limit_->Lift();
    handle_->Join();
  }

  void Cancel() override { handle_->Cancel(); }
  void CancelAndDetach() override { handle_->CancelAndDetach(); }
  bool IsActive() override { return handle_->IsActive(); }
  bool IsValid() override { return handle_->IsValid(); }
  bool UpdatePriorityEnabled() const override { return handle_->UpdatePriorityEnabled(); }

  // A lower limit takes effect as workers return from Run(), as with any drop
  // in GetMaxConcurrency().
  void UpdatePriority(v8::TaskPriority priority) override {
    handle_->UpdatePriority(priority);
    // The caller can't know the limit went up, so post the workers it now
    // allows. A job from CreateJob() that hasn't been started stays unstarted.
    if (limit_->FollowPriority(priority) && started_.load(std::memory_order_relaxed)) {
      handle_->NotifyConcurrencyIncrease();
    }
  }

 private:
  std::unique_ptr<v8::JobHandle> handle_;
  std::shared_ptr<JobWorkerLimit> limit_;
  std::atomic<bool> started_{false};
};

}  // namespace

std::unique_ptr<v8::JobHandle> EdgeV8Platform::CreateJobImpl(
    v8::TaskPriority priority,
    std::unique_ptr<v8::JobTask> job_task,
    const v8::SourceLocation& location) {
  (void)location;
  const int workers = std::max(1, NumberOfWorkerThreads());
  // With one worker, every job keeps it. Limited to zero workers, a job would
  // only run once a thread joins it, and V8 joins some jobs, such as Maglev's
  // compile job, only when it flushes them.
  if (workers == 1) {
    return v8::platform::NewDefaultJobHandle(this, priority, std::move(job_task), 1);
  }
  auto limit = std::make_shared<JobWorkerLimit>(static_cast<size_t>(workers - 1), priority);
  std::unique_ptr<v8::JobHandle> handle = v8::platform::NewDefaultJobHandle(
      this, priority, std::make_unique<LimitedJobTask>(std::move(job_task), limit),
      static_cast<size_t>(workers));
  return std::make_unique<LimitedJobHandle>(std::move(handle), std::move(limit));
}

void EdgeV8Platform::PostTaskOnWorkerThreadImpl(v8::TaskPriority priority,
                                               std::unique_ptr<v8::Task> task,
                                               const v8::SourceLocation& location) {
  if (current_background_lane != nullptr) {
    current_background_lane->Post(std::move(task), 0.0);
    return;
  }
  if (v8::Platform* workers = StandaloneWorkers()) {
    standalone_worker_posts.fetch_add(1, std::memory_order_relaxed);
    workers->PostTaskOnWorkerThread(priority, std::move(task), location);
    return;
  }
  // No instance to run it for: drop it rather than run it unattributed.
  unattributed_worker_posts.fetch_add(1, std::memory_order_relaxed);
}

void EdgeV8Platform::PostDelayedTaskOnWorkerThreadImpl(
    v8::TaskPriority priority,
    std::unique_ptr<v8::Task> task,
    double delay_in_seconds,
    const v8::SourceLocation& location) {
  if (current_background_lane != nullptr) {
    current_background_lane->Post(std::move(task), delay_in_seconds);
    return;
  }
  if (v8::Platform* workers = StandaloneWorkers()) {
    standalone_worker_posts.fetch_add(1, std::memory_order_relaxed);
    workers->PostDelayedTaskOnWorkerThread(priority, std::move(task), delay_in_seconds, location);
    return;
  }
  unattributed_worker_posts.fetch_add(1, std::memory_order_relaxed);
}
