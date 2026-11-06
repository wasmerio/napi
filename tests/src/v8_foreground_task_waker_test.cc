#include "../../v8/src/edge_v8_platform.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <v8.h>

namespace {

// Every wait is bounded so a regression fails the test instead of hanging it.
constexpr auto kWaitTimeout = std::chrono::seconds(10);
// More than V8 posts while an idle isolate settles.
constexpr int kMaxPumps = 64;

std::atomic<bool> g_passed{true};

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void Fail(const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fprintf(stderr, "\n");
  g_passed = false;
}

// What the waker saw. It runs on whichever thread posted the task.
struct Wakes {
  std::mutex mutex;
  std::condition_variable cv;
  // Calls made on a thread that set t_poster.
  size_t from_poster = 0;
  // Set by the main thread once it has pumped after a poster's wake.
  bool pumped = false;
};
Wakes g_wakes;

// The test's posting threads mark themselves, so that a task V8 posts on its
// own, from another thread, doesn't count.
thread_local bool t_poster = false;
// A poster that sets this waits inside the waker until the main thread has
// pumped, so the pump sees exactly what was queued before the waker ran.
thread_local bool t_hold = false;
thread_local size_t t_calls = 0;

void RecordingWaker() {
  ++t_calls;
  if (!t_poster) return;
  std::unique_lock<std::mutex> lock(g_wakes.mutex);
  ++g_wakes.from_poster;
  g_wakes.cv.notify_all();
  if (t_hold &&
      !g_wakes.cv.wait_for(lock, kWaitTimeout, [] { return g_wakes.pumped; })) {
    Fail("the main thread did not pump after the wake");
  }
}

void ResetWakes() {
  std::lock_guard<std::mutex> lock(g_wakes.mutex);
  g_wakes.from_poster = 0;
  g_wakes.pumped = false;
}

// How many times a FlagTask ran. Shared with the task, so that a task a
// failing case leaves queued can still run safely at the final pump.
using RunCount = std::shared_ptr<std::atomic<int>>;

RunCount NewRunCount() { return std::make_shared<std::atomic<int>>(0); }

class FlagTask final : public v8::Task {
 public:
  explicit FlagTask(RunCount ran) : ran_(std::move(ran)) {}
  void Run() override { ran_->fetch_add(1, std::memory_order_acq_rel); }

 private:
  RunCount ran_;
};

// Posts from a thread of its own, as V8 does when another thread resolves an
// Atomics.waitAsync or finishes a WebAssembly.compile. Returns how many times
// the waker ran on that thread while it posted.
template <typename Post>
size_t PostFromAnotherThread(Post post) {
  size_t calls = 0;
  std::thread poster([&] {
    t_poster = true;
    post();
    calls = t_calls;
  });
  poster.join();
  return calls;
}

// Pumps like a guest's checkpoint does, one task at a time, until `ran` is
// set or nothing is left to run.
void PumpUntil(EdgeV8Platform* platform, v8::Isolate* isolate, const std::atomic<int>& ran) {
  for (int i = 0; i < kMaxPumps && ran.load(std::memory_order_acquire) == 0; ++i) {
    if (!platform->PumpPendingForegroundTasks(isolate)) break;
  }
}

void PumpAll(EdgeV8Platform* platform, v8::Isolate* isolate) {
  for (int i = 0; i < kMaxPumps && platform->PumpPendingForegroundTasks(isolate); ++i) {
  }
}

// Stands in for a guest's enqueue hook: keeps the records it is handed.
struct BoundTarget {
  struct Entry {
    unofficial_napi_foreground_task_callback callback;
    void* data;
    unofficial_napi_foreground_task_cleanup cleanup;
  };
  std::mutex mutex;
  std::vector<Entry> entries;
};

napi_status EnqueueOnTarget(void* target,
                            unofficial_napi_foreground_task_callback callback,
                            void* data,
                            unofficial_napi_foreground_task_cleanup cleanup,
                            uint64_t /*delay_millis*/) {
  auto* bound = static_cast<BoundTarget*>(target);
  std::lock_guard<std::mutex> lock(bound->mutex);
  bound->entries.push_back({callback, data, cleanup});
  return napi_ok;
}

// An immediate task posted from another thread calls the waker on that
// thread, once, after the task is queued: the main thread, woken by it, finds
// the task at its next pump.
void ImmediateTaskWakes(EdgeV8Platform* platform,
                        v8::Isolate* isolate,
                        const std::shared_ptr<v8::TaskRunner>& runner) {
  EdgeV8Platform::SetForegroundTaskWaker(RecordingWaker);
  ResetWakes();
  const RunCount ran = NewRunCount();
  size_t calls = 0;
  std::thread poster([&] {
    t_poster = true;
    t_hold = true;
    runner->PostTask(std::make_unique<FlagTask>(ran));
    calls = t_calls;
  });
  // This thread waits like a guest blocked on its event source.
  {
    std::unique_lock<std::mutex> lock(g_wakes.mutex);
    if (!g_wakes.cv.wait_for(lock, kWaitTimeout, [] { return g_wakes.from_poster > 0; })) {
      Fail("immediate task: the waker was not called");
    }
  }
  // The poster waits in the waker meanwhile, so what is queued now was
  // queued before the waker ran.
  const bool ran_before_pump = ran->load(std::memory_order_acquire) != 0;
  PumpUntil(platform, isolate, *ran);
  const bool ran_at_pump = ran->load(std::memory_order_acquire) != 0;
  {
    std::lock_guard<std::mutex> lock(g_wakes.mutex);
    g_wakes.pumped = true;
    g_wakes.cv.notify_all();
  }
  poster.join();

  if (calls != 1) {
    Fail("immediate task: the posting thread called the waker %zu times, expected 1", calls);
  }
  if (ran_before_pump) Fail("immediate task: ran before any pump");
  if (!ran_at_pump) Fail("immediate task: not queued when the waker ran");
  PumpUntil(platform, isolate, *ran);
  if (ran->load() != 1) Fail("immediate task: ran %d times, expected 1", ran->load());
}

// A delayed task doesn't call the waker. It runs at a pump after its delay.
void DelayedTaskDoesNotWake(EdgeV8Platform* platform,
                            v8::Isolate* isolate,
                            const std::shared_ptr<v8::TaskRunner>& runner) {
  EdgeV8Platform::SetForegroundTaskWaker(RecordingWaker);
  const RunCount ran = NewRunCount();
  const size_t calls = PostFromAnotherThread([&] {
    runner->PostDelayedTask(std::make_unique<FlagTask>(ran), 0.05);
  });
  if (calls != 0) {
    Fail("delayed task: the posting thread called the waker %zu times, expected 0", calls);
  }

  const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
  while (ran->load(std::memory_order_acquire) == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    PumpUntil(platform, isolate, *ran);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (ran->load() != 1) Fail("delayed task: ran %d times after its delay, expected 1", ran->load());
}

// A cleared waker isn't called. The task still runs at the next pump.
void ClearedWakerIsNotCalled(EdgeV8Platform* platform,
                             v8::Isolate* isolate,
                             const std::shared_ptr<v8::TaskRunner>& runner) {
  EdgeV8Platform::SetForegroundTaskWaker(RecordingWaker);
  EdgeV8Platform::SetForegroundTaskWaker(nullptr);
  const RunCount ran = NewRunCount();
  const size_t calls = PostFromAnotherThread([&] {
    runner->PostTask(std::make_unique<FlagTask>(ran));
  });
  if (calls != 0) Fail("cleared waker: called %zu times, expected 0", calls);
  PumpUntil(platform, isolate, *ran);
  if (ran->load() != 1) {
    Fail("cleared waker: the task ran %d times at the next pump, expected 1", ran->load());
  }
}

// A task for an isolate with a target bound goes to that target, which runs it
// on its own: the waker is only for the queue a checkpoint runs.
void BoundTargetDoesNotWake(EdgeV8Platform* platform,
                            v8::Isolate* isolate,
                            const std::shared_ptr<v8::TaskRunner>& runner) {
  EdgeV8Platform::SetForegroundTaskWaker(RecordingWaker);
  BoundTarget bound;
  int env_tag = 0;
  auto* env = reinterpret_cast<napi_env>(&env_tag);
  if (!platform->BindForegroundTaskTarget(isolate, env, EnqueueOnTarget, &bound)) {
    Fail("bound target: BindForegroundTaskTarget failed");
    return;
  }
  const RunCount ran = NewRunCount();
  const size_t calls = PostFromAnotherThread([&] {
    runner->PostTask(std::make_unique<FlagTask>(ran));
  });
  platform->ClearForegroundTaskTarget(isolate, env);
  if (calls != 0) Fail("bound target: called the waker %zu times, expected 0", calls);

  std::vector<BoundTarget::Entry> entries;
  {
    std::lock_guard<std::mutex> lock(bound.mutex);
    entries.swap(bound.entries);
  }
  if (entries.size() != 1) Fail("bound target: got %zu tasks, expected 1", entries.size());
  for (const BoundTarget::Entry& entry : entries) {
    entry.callback(env, entry.data);
    if (entry.cleanup != nullptr) entry.cleanup(env, entry.data);
  }
  if (ran->load() != 1) Fail("bound target: the task ran %d times, expected 1", ran->load());
}

}  // namespace

// Own V8 initialization and disposal in a separate process: the waker is
// process-wide.
int main() {
  EdgeV8Platform::SetWorkerThreadCount(2);
  auto platform = EdgeV8Platform::Create();
  if (!platform) return 1;
  v8::V8::InitializePlatform(platform.get());
  if (!v8::V8::Initialize()) return 1;
  auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = allocator.get();
  v8::Isolate* isolate = v8::Isolate::New(params);
  if (!isolate) return 1;
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    // No target is bound, so the isolate's foreground tasks wait in the
    // default platform's queue, which only PumpPendingForegroundTasks runs.
    auto runner = platform->GetForegroundTaskRunner(isolate, v8::TaskPriority::kUserVisible);
    PumpAll(platform.get(), isolate);

    ImmediateTaskWakes(platform.get(), isolate, runner);
    DelayedTaskDoesNotWake(platform.get(), isolate, runner);
    ClearedWakerIsNotCalled(platform.get(), isolate, runner);
    BoundTargetDoesNotWake(platform.get(), isolate, runner);
    EdgeV8Platform::SetForegroundTaskWaker(nullptr);
    PumpAll(platform.get(), isolate);
  }
  platform->DisposeIsolate(isolate);
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  if (g_passed) std::printf("foreground task waker: ok\n");
  return g_passed ? 0 : 1;
}
