// Regression tests for handing V8 foreground tasks to the embedder's
// enqueue_foreground_task hook. V8 provider only: QuickJS does not post engine
// tasks through this hook.
//
// The hook owns a task from the moment it is called, whatever it returns (see
// unofficial_napi_enqueue_foreground_task_callback). It may refuse the task and
// release it on the spot, or accept it and have another thread run and release
// it before the call has even returned. The provider must never touch a task
// after handing it over. These tests make the hook do each of those things
// while V8 posts its own work: GC jobs under a small heap, FinalizationRegistry
// cleanup, Atomics.waitAsync timeouts and async WebAssembly compilation (whose
// foreground steps are posted from V8 worker threads).
//
// A provider that touches a released task crashes, trips AddressSanitizer, or
// leaks its pending-task count, which makes unofficial_napi_release_env wait
// forever; the watchdog below turns that into a failure.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <v8.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

#include "test_env.h"

// The V8 provider's own entry points for memory that only needs freeing
// (v8/src/internal/napi_v8_env.h). The WASIX bridge makes guest-heap buffers
// with them.
typedef void (*napi_v8_external_free_callback)(void* data, void* hint);
napi_status napi_v8_create_external_arraybuffer_with_free(
    napi_env env, void* data, size_t byte_length, napi_v8_external_free_callback free_cb,
    void* free_hint, napi_value* result);
napi_status napi_v8_create_external_buffer_with_free(
    napi_env env, size_t length, void* data, napi_v8_external_free_callback free_cb,
    void* free_hint, napi_value* result);

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;

struct HookedTask {
  unofficial_napi_foreground_task_callback callback = nullptr;
  void* data = nullptr;
  unofficial_napi_foreground_task_cleanup cleanup = nullptr;
};

enum class HookMode {
  // Refuse every task: release it on the calling thread, then return an error.
  kRefuse,
  // Accept every task, but release it on another thread before returning, as
  // an embedder whose loop thread picks the task up straight away would.
  kReleaseOnAnotherThread,
  // Queue tasks for the env's own thread to run later, as EdgeJS does.
  kQueue,
};

struct HookState {
  explicit HookState(HookMode mode_in) : mode(mode_in) {}

  const HookMode mode;
  napi_env env = nullptr;
  // Made on the test's thread, which is the env's.
  const std::thread::id env_thread = std::this_thread::get_id();
  // In kQueue mode, while set, a task posted from another thread also waits
  // until the env's thread has run or released it. A hook may not wait like
  // that in general, since it is called during GCs; tests do it while nothing
  // collects, to show the provider holds no lock the task needs while it
  // calls the hook.
  std::atomic<bool> hand_off{false};

  std::mutex mutex;
  std::condition_variable cv;
  bool closed = false;
  std::deque<HookedTask> queue;
  // Tasks queued, and tasks run or released since, in queue order.
  uint64_t queued = 0;
  uint64_t done = 0;
  // Threads that waited in the hook because of `hand_off`.
  std::vector<std::thread::id> waited;

  std::atomic<int> enqueued{0};
  std::atomic<int> released{0};
  std::atomic<int> ran{0};
};

// Hands the task back to the provider for good. Counts first: once the last
// task is released the env may finish tearing down, and the test may then
// destroy `state` while this thread is still returning.
void ReleaseTask(HookState* state, const HookedTask& task) {
  napi_env env = state->env;
  state->released.fetch_add(1);
  if (task.cleanup != nullptr) task.cleanup(env, task.data);
}

void RunTask(HookState* state, const HookedTask& task) {
  state->ran.fetch_add(1);
  task.callback(state->env, task.data);
  ReleaseTask(state, task);
}

napi_status EnqueueHook(void* target,
                        unofficial_napi_foreground_task_callback callback,
                        void* data,
                        unofficial_napi_foreground_task_cleanup cleanup,
                        uint64_t /*delay_millis*/) {
  auto* state = static_cast<HookState*>(target);
  if (state == nullptr || callback == nullptr) return napi_invalid_arg;
  const HookedTask task{callback, data, cleanup};
  state->enqueued.fetch_add(1);

  switch (state->mode) {
    case HookMode::kRefuse:
      ReleaseTask(state, task);
      return napi_generic_failure;
    case HookMode::kReleaseOnAnotherThread:
      std::thread([state, task] { ReleaseTask(state, task); }).join();
      return napi_ok;
    case HookMode::kQueue: {
      {
        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->closed) {
          state->queue.push_back(task);
          const uint64_t ticket = ++state->queued;
          const bool wait =
              state->hand_off.load() && std::this_thread::get_id() != state->env_thread;
          if (wait) state->waited.push_back(std::this_thread::get_id());
          state->cv.notify_all();
          if (wait) {
            state->cv.wait(lock, [state, ticket] { return state->done >= ticket; });
          }
          return napi_ok;
        }
      }
      ReleaseTask(state, task);
      return napi_generic_failure;
    }
  }
  ReleaseTask(state, task);
  return napi_generic_failure;
}

// Runs the queued tasks on the calling thread, which must be the env's thread.
size_t RunQueuedTasks(HookState* state) {
  std::deque<HookedTask> batch;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    batch.swap(state->queue);
  }
  for (const HookedTask& task : batch) RunTask(state, task);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->done += batch.size();
  }
  state->cv.notify_all();
  return batch.size();
}

// Refuses further tasks and releases the queued ones without running them, as
// an embedder does when its environment shuts down.
void CloseQueue(HookState* state) {
  std::deque<HookedTask> batch;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->closed = true;
    batch.swap(state->queue);
  }
  for (const HookedTask& task : batch) ReleaseTask(state, task);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->done += batch.size();
  }
  state->cv.notify_all();
}

// Aborts the process if it is not destroyed within `limit`. A provider that
// loses track of a task never lets unofficial_napi_release_env return.
class Watchdog {
 public:
  Watchdog(const char* what, seconds limit)
      : thread_([this, what, limit] {
          std::unique_lock<std::mutex> lock(mutex_);
          if (!cv_.wait_for(lock, limit, [this] { return done_; })) {
            std::fprintf(stderr,
                         "%s did not finish within %lld s; a foreground task "
                         "was probably lost after being handed to the hook\n",
                         what, static_cast<long long>(limit.count()));
            std::abort();
          }
        }) {}

  ~Watchdog() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
  std::thread thread_;
};

class HookedEnv {
 public:
  explicit HookedEnv(HookState* hook) {
    unofficial_napi_env_create_options options{};
    InitializeTestEnvCreateOptions(&options);
    // A small heap makes V8 schedule its own GC jobs early.
    options.max_young_generation_size_in_bytes = 2u << 20;
    options.max_old_generation_size_in_bytes = 64u << 20;
    EXPECT_EQ(unofficial_napi_create_env(
                  NAPI_TEST_MODULE_API_VERSION, &options, &env_, &owner_),
              napi_ok);
    if (env_ == nullptr) return;

    hook->env = env_;
    unofficial_napi_env_hooks hooks{};
    hooks.size = sizeof(hooks);
    hooks.version = UNOFFICIAL_NAPI_ENV_HOOKS_VERSION;
    hooks.data = hook;
    hooks.enqueue_foreground_task_callback = EnqueueHook;
    uint64_t accepted = 0;
    EXPECT_EQ(unofficial_napi_attach_env(env_, &hooks, &accepted), napi_ok);
    attached_ =
        (accepted & unofficial_napi_env_hook_enqueue_foreground_task) != 0;
  }

  ~HookedEnv() { Release(); }

  HookedEnv(const HookedEnv&) = delete;
  HookedEnv& operator=(const HookedEnv&) = delete;

  void Release() {
    if (owner_ == nullptr) return;
    Watchdog watchdog("unofficial_napi_release_env", seconds(30));
    EXPECT_EQ(unofficial_napi_release_env(owner_, nullptr), napi_ok);
    owner_ = nullptr;
    env_ = nullptr;
  }

  napi_env env() const { return env_; }
  bool attached() const { return attached_; }

 private:
  napi_env env_ = nullptr;
  unofficial_napi_env_owner owner_ = nullptr;
  bool attached_ = false;
};

testing::AssertionResult RunJs(napi_env env, const char* source_text) {
  napi_value source = nullptr;
  if (napi_create_string_utf8(env, source_text, NAPI_AUTO_LENGTH, &source) !=
      napi_ok) {
    return testing::AssertionFailure() << "could not create the script source";
  }
  napi_value result = nullptr;
  if (napi_run_script(env, source, &result) == napi_ok) {
    return testing::AssertionSuccess();
  }
  napi_value exception = nullptr;
  napi_value message = nullptr;
  char buffer[512] = {};
  size_t length = 0;
  if (napi_get_and_clear_last_exception(env, &exception) == napi_ok &&
      napi_coerce_to_string(env, exception, &message) == napi_ok) {
    napi_get_value_string_utf8(env, message, buffer, sizeof(buffer), &length);
  }
  return testing::AssertionFailure() << "script threw: " << buffer;
}

bool GlobalIsTrue(napi_env env, const char* name) {
  napi_value global = nullptr;
  napi_value value = nullptr;
  bool result = false;
  return napi_get_global(env, &global) == napi_ok &&
         napi_get_named_property(env, global, name, &value) == napi_ok &&
         napi_get_value_bool(env, value, &result) == napi_ok && result;
}

int32_t GlobalInt(napi_env env, const char* name) {
  napi_value global = nullptr;
  napi_value value = nullptr;
  int32_t result = -1;
  if (napi_get_global(env, &global) != napi_ok ||
      napi_get_named_property(env, global, name, &value) != napi_ok ||
      napi_get_value_int32(env, value, &result) != napi_ok) {
    return -1;
  }
  return result;
}

void RunMicrotasks(napi_env env) {
  uint32_t state = 0;
  EXPECT_EQ(unofficial_napi_event_loop_checkpoint(
                env, unofficial_napi_event_loop_checkpoint_microtasks, false,
                &state),
            napi_ok);
}

// Makes V8 post foreground tasks of several kinds, from its own thread and
// from worker threads.
constexpr char kPostEngineTasks[] = R"JS(
  (() => {
    // GC finds dead FinalizationRegistry targets and posts a cleanup task.
    globalThis.finalized = 0;
    globalThis.registry =
        new FinalizationRegistry(() => { globalThis.finalized++; });
    for (let i = 0; i < 64; i++) registry.register({ i }, i);

    // Every Atomics.waitAsync with a timeout posts a delayed task.
    const cell = new Int32Array(new SharedArrayBuffer(4));
    for (let i = 0; i < 16; i++) Atomics.waitAsync(cell, 0, 0, 60000);

    // Async compilation decodes on a worker thread, which then posts the next
    // step to this isolate's foreground runner.
    globalThis.wasmSettled = false;
    WebAssembly.compile(new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]))
        .finally(() => { globalThis.wasmSettled = true; });

    // Allocation pressure on the small heap lets V8 schedule its GC jobs.
    let live = [];
    for (let i = 0; i < 200000; i++) {
      live.push({ i, payload: [i, i, i, i, i, i, i, i] });
      if (live.length === 1024) live = [];
    }
    gc();
  })();
)JS";

class Test66ForegroundTaskHandoff : public FixtureTestBase {};

TEST_F(Test66ForegroundTaskHandoff, RefusedTasksAreReleasedOnlyByTheHook) {
  HookState hook(HookMode::kRefuse);
  {
    HookedEnv env(&hook);
    ASSERT_NE(env.env(), nullptr);
    ASSERT_TRUE(env.attached());

    for (int round = 0; round < 4; ++round) {
      ASSERT_TRUE(RunJs(env.env(), kPostEngineTasks));
      ASSERT_EQ(unofficial_napi_collect_garbage(env.env()), napi_ok);
      RunMicrotasks(env.env());
    }
    EXPECT_GT(hook.enqueued.load(), 0);
    // The env keeps working after its tasks were refused.
    ASSERT_TRUE(RunJs(env.env(), "globalThis.stillAlive = true;"));
    EXPECT_TRUE(GlobalIsTrue(env.env(), "stillAlive"));
    env.Release();
  }
  EXPECT_EQ(hook.released.load(), hook.enqueued.load());
  EXPECT_EQ(hook.ran.load(), 0);
}

TEST_F(Test66ForegroundTaskHandoff,
       AcceptedTasksMayBeReleasedOnAnotherThreadBeforeEnqueueReturns) {
  HookState hook(HookMode::kReleaseOnAnotherThread);
  {
    HookedEnv env(&hook);
    ASSERT_NE(env.env(), nullptr);
    ASSERT_TRUE(env.attached());

    for (int round = 0; round < 4; ++round) {
      ASSERT_TRUE(RunJs(env.env(), kPostEngineTasks));
      ASSERT_EQ(unofficial_napi_collect_garbage(env.env()), napi_ok);
      RunMicrotasks(env.env());
    }
    EXPECT_GT(hook.enqueued.load(), 0);
    env.Release();
  }
  EXPECT_EQ(hook.released.load(), hook.enqueued.load());
}

TEST_F(Test66ForegroundTaskHandoff, QueuedTasksRunOnTheEnvThread) {
  HookState hook(HookMode::kQueue);
  {
    HookedEnv env(&hook);
    ASSERT_NE(env.env(), nullptr);
    ASSERT_TRUE(env.attached());

    ASSERT_TRUE(RunJs(env.env(), kPostEngineTasks));
    ASSERT_EQ(unofficial_napi_collect_garbage(env.env()), napi_ok);

    // Pump the hook's queue until the worker-posted compile steps have run and
    // the FinalizationRegistry callback has fired.
    const auto deadline = steady_clock::now() + seconds(20);
    while (!GlobalIsTrue(env.env(), "wasmSettled") ||
           GlobalInt(env.env(), "finalized") <= 0) {
      ASSERT_LT(steady_clock::now(), deadline)
          << "queued foreground tasks did not complete; wasmSettled="
          << GlobalIsTrue(env.env(), "wasmSettled")
          << " finalized=" << GlobalInt(env.env(), "finalized");
      if (RunQueuedTasks(&hook) == 0) {
        std::unique_lock<std::mutex> lock(hook.mutex);
        hook.cv.wait_for(lock, milliseconds(10),
                         [&hook] { return !hook.queue.empty(); });
      }
      RunMicrotasks(env.env());
    }
    EXPECT_GT(hook.ran.load(), 0);

    CloseQueue(&hook);
    env.Release();
  }
  EXPECT_EQ(hook.released.load(), hook.enqueued.load());
}

struct ExternalFinalizerCounts {
  std::thread::id env_thread = std::this_thread::get_id();
  std::atomic<int> calls{0};
  std::atomic<int> off_thread{0};
};

void CountExternalFinalizer(node_api_basic_env /*env*/, void* data, void* hint) {
  auto* counts = static_cast<ExternalFinalizerCounts*>(hint);
  if (std::this_thread::get_id() != counts->env_thread) counts->off_thread.fetch_add(1);
  delete[] static_cast<uint8_t*>(data);
  counts->calls.fetch_add(1);
}

// V8 sweeps dead ArrayBuffers on its own threads too, and frees their backing
// stores there. An external ArrayBuffer's finalizer never runs there, nor
// inside the collector: the provider posts a drain through the hook, which
// runs it on the env's thread.
TEST_F(Test66ForegroundTaskHandoff, ExternalArrayBufferFinalizersRunFromAQueuedTask) {
  constexpr int kBatches = 100;
  constexpr int kBuffers = kBatches * 64;
  ExternalFinalizerCounts counts;
  HookState hook(HookMode::kQueue);
  {
    HookedEnv env(&hook);
    ASSERT_NE(env.env(), nullptr);
    ASSERT_TRUE(env.attached());

    // Allocation pressure on the small heap, rather than a forced collection,
    // makes V8 sweep concurrently.
    for (int batch = 0; batch < kBatches; ++batch) {
      napi_handle_scope handles = nullptr;
      ASSERT_EQ(napi_open_handle_scope(env.env(), &handles), napi_ok);
      for (int i = 0; i < kBuffers / kBatches; ++i) {
        auto* bytes = new uint8_t[1024]();
        napi_value buffer = nullptr;
        ASSERT_EQ(napi_create_external_arraybuffer(env.env(), bytes, 1024,
                                                   CountExternalFinalizer, &counts,
                                                   &buffer),
                  napi_ok);
      }
      ASSERT_TRUE(RunJs(env.env(),
                        "{ let a = []; for (let i = 0; i < 20000; i++) a.push({ i }); }"));
      ASSERT_EQ(napi_close_handle_scope(env.env(), handles), napi_ok);
    }
    ASSERT_EQ(unofficial_napi_collect_garbage(env.env()), napi_ok);
    EXPECT_EQ(counts.calls.load(), 0) << "finalizers ran inside the collector";

    // Only the hook's queue runs; nothing drains finalizers at a checkpoint.
    const auto deadline = steady_clock::now() + seconds(20);
    while (counts.calls.load() < kBuffers) {
      ASSERT_LT(steady_clock::now(), deadline)
          << "only " << counts.calls.load() << " of " << kBuffers
          << " external finalizers ran";
      if (RunQueuedTasks(&hook) == 0) {
        // Collects any buffer the first collection missed.
        ASSERT_EQ(unofficial_napi_collect_garbage(env.env()), napi_ok);
        std::unique_lock<std::mutex> lock(hook.mutex);
        hook.cv.wait_for(lock, milliseconds(10),
                         [&hook] { return !hook.queue.empty(); });
      }
    }
    EXPECT_EQ(counts.off_thread.load(), 0);

    CloseQueue(&hook);
    env.Release();
  }
  EXPECT_EQ(counts.calls.load(), kBuffers);
  EXPECT_EQ(hook.released.load(), hook.enqueued.load());
}

// Makes an external ArrayBuffer and detaches it. Only `store` keeps its
// backing store alive.
void MakeDetachedExternalBuffer(napi_env env,
                                ExternalFinalizerCounts* counts,
                                std::shared_ptr<v8::BackingStore>* store) {
  napi_handle_scope handles = nullptr;
  ASSERT_EQ(napi_open_handle_scope(env, &handles), napi_ok);
  auto* bytes = new uint8_t[1024]();
  napi_value buffer = nullptr;
  ASSERT_EQ(napi_create_external_arraybuffer(env, bytes, 1024, CountExternalFinalizer, counts,
                                             &buffer),
            napi_ok);
  // This provider's napi_value is a v8::Local.
  v8::Local<v8::Value> value;
  std::memcpy(static_cast<void*>(&value), &buffer, sizeof(buffer));
  *store = value.As<v8::ArrayBuffer>()->GetBackingStore();
  ASSERT_EQ(napi_detach_arraybuffer(env, buffer), napi_ok);
  ASSERT_EQ(napi_close_handle_scope(env, handles), napi_ok);
  ASSERT_EQ(counts->calls.load(), 0);
}

// The last reference to a backing store drops on another thread, so its
// deleter runs there and posts the drain. The hook hands the drain to the
// env's thread and waits until it has run, which deadlocks if the provider
// posts while holding a lock the drain takes.
TEST_F(Test66ForegroundTaskHandoff,
       ExternalArrayBufferFreedOnAnotherThreadIsFinalizedByThePostedDrain) {
  ExternalFinalizerCounts counts;
  HookState hook(HookMode::kQueue);
  {
    HookedEnv env(&hook);
    ASSERT_NE(env.env(), nullptr);
    ASSERT_TRUE(env.attached());
    std::shared_ptr<v8::BackingStore> store;
    ASSERT_NO_FATAL_FAILURE(MakeDetachedExternalBuffer(env.env(), &counts, &store));

    {
      Watchdog watchdog("the drain posted from another thread", seconds(30));
      hook.hand_off = true;
      std::thread dropper([&store] { store.reset(); });
      // Only the hook's queue runs.
      const auto deadline = steady_clock::now() + seconds(20);
      while (counts.calls.load() == 0 && steady_clock::now() < deadline) {
        if (RunQueuedTasks(&hook) == 0) {
          std::unique_lock<std::mutex> lock(hook.mutex);
          hook.cv.wait_for(lock, milliseconds(10),
                           [&hook] { return !hook.queue.empty(); });
        }
      }
      // Releases what the hook still holds, so the dropper returns.
      CloseQueue(&hook);
      dropper.join();
      hook.hand_off = false;
    }
    EXPECT_EQ(counts.calls.load(), 1);
    EXPECT_EQ(counts.off_thread.load(), 0);
    env.Release();
  }
  EXPECT_EQ(hook.released.load(), hook.enqueued.load());
}

struct InstanceDataSignal {
  ExternalFinalizerCounts* counts = nullptr;
  std::mutex mutex;
  std::condition_variable cv;
  bool finalized = false;
  int calls_before = -1;
};

void SignalInstanceDataFinalized(napi_env /*env*/, void* data, void* /*hint*/) {
  auto* signal = static_cast<InstanceDataSignal*>(data);
  std::lock_guard<std::mutex> lock(signal->mutex);
  signal->calls_before = signal->counts->calls.load();
  signal->finalized = true;
  signal->cv.notify_all();
}

// The env is released while a deleter on another thread is still in the
// hook, posting a drain. The release runs the queued finalizer itself, and
// the env outlives the post: the release waits for the hook to return, which
// here is only once the embedder lets go of the task.
TEST_F(Test66ForegroundTaskHandoff, ReleaseOutlastsADrainBeingPostedFromAnotherThread) {
  ExternalFinalizerCounts counts;
  InstanceDataSignal signal;
  signal.counts = &counts;
  HookState hook(HookMode::kQueue);
  {
    HookedEnv env(&hook);
    ASSERT_NE(env.env(), nullptr);
    ASSERT_TRUE(env.attached());
    std::shared_ptr<v8::BackingStore> store;
    ASSERT_NO_FATAL_FAILURE(MakeDetachedExternalBuffer(env.env(), &counts, &store));
    // Runs last in the release, just before it waits for posts.
    ASSERT_EQ(napi_set_instance_data(env.env(), &signal, SignalInstanceDataFinalized, nullptr),
              napi_ok);

    Watchdog watchdog("the release while a drain was being posted", seconds(30));
    hook.hand_off = true;
    std::thread dropper([&store] { store.reset(); });
    const std::thread::id dropper_id = dropper.get_id();
    bool posted = false;
    {
      // The dropper has posted its drain and waits in the hook.
      std::unique_lock<std::mutex> lock(hook.mutex);
      posted = hook.cv.wait_for(lock, seconds(10), [&] {
        return std::find(hook.waited.begin(), hook.waited.end(), dropper_id) !=
               hook.waited.end();
      });
    }
    if (!posted) {
      dropper.join();
      hook.hand_off = false;
      FAIL() << "the deleter posted no drain from the dropper's thread";
    }
    std::thread closer([&] {
      {
        std::unique_lock<std::mutex> lock(signal.mutex);
        signal.cv.wait(lock, [&] { return signal.finalized; });
      }
      // By now the release waits for the post, or is about to.
      std::this_thread::sleep_for(milliseconds(100));
      CloseQueue(&hook);
    });
    env.Release();
    closer.join();
    dropper.join();
    hook.hand_off = false;
  }
  // The release ran the queued finalizer before the instance-data finalizer.
  EXPECT_EQ(signal.calls_before, 1);
  EXPECT_EQ(counts.calls.load(), 1);
  EXPECT_EQ(counts.off_thread.load(), 0);
  EXPECT_EQ(hook.released.load(), hook.enqueued.load());
}

// Memory that only needs freeing comes back where V8 frees its backing store,
// unlike a user finalizer, which waits for the env's thread. Otherwise a
// synchronous stretch that makes and drops buffers keeps all of them until
// the next drain; a WASIX guest's heap then grows, and wasm memory never
// shrinks.

struct FreeCounts {
  std::atomic<int> freed{0};
};

void CountFree(void* data, void* hint) {
  delete[] static_cast<uint8_t*>(data);
  static_cast<FreeCounts*>(hint)->freed.fetch_add(1);
}

TEST_F(Test66ForegroundTaskHandoff, MemoryFreedInTheDeleterComesBackWithoutADrain) {
  constexpr int kBatches = 16;
  constexpr int kBuffers = kBatches * 64;
  FreeCounts counts;
  HookState hook(HookMode::kQueue);
  {
    HookedEnv env(&hook);
    ASSERT_NE(env.env(), nullptr);
    ASSERT_TRUE(env.attached());

    for (int batch = 0; batch < kBatches; ++batch) {
      napi_handle_scope handles = nullptr;
      ASSERT_EQ(napi_open_handle_scope(env.env(), &handles), napi_ok);
      for (int i = 0; i < kBuffers / kBatches; ++i) {
        auto* bytes = new uint8_t[1024]();
        napi_value buffer = nullptr;
        // Half ArrayBuffers, half Buffers.
        if (i % 2 == 0) {
          ASSERT_EQ(napi_v8_create_external_arraybuffer_with_free(env.env(), bytes, 1024,
                                                                  CountFree, &counts, &buffer),
                    napi_ok);
        } else {
          ASSERT_EQ(napi_v8_create_external_buffer_with_free(env.env(), 1024, bytes, CountFree,
                                                             &counts, &buffer),
                    napi_ok);
        }
      }
      ASSERT_TRUE(RunJs(env.env(),
                        "{ let a = []; for (let i = 0; i < 20000; i++) a.push({ i }); }"));
      ASSERT_EQ(napi_close_handle_scope(env.env(), handles), napi_ok);
    }
    // Neither the hook's queue nor a checkpoint runs; only the collector.
    for (int round = 0; round < 20 && counts.freed.load() < kBuffers; ++round) {
      ASSERT_EQ(unofficial_napi_collect_garbage(env.env()), napi_ok);
      if (counts.freed.load() < kBuffers) std::this_thread::sleep_for(milliseconds(10));
    }
    EXPECT_EQ(counts.freed.load(), kBuffers) << "freed only after a drain";

    CloseQueue(&hook);
    env.Release();
  }
  EXPECT_EQ(counts.freed.load(), kBuffers);
  EXPECT_EQ(hook.released.load(), hook.enqueued.load());
}

// What is still alive at the release is freed then, once, as before: the
// guest may reuse the memory after that. A backing store that outlives the
// env is not freed again when V8 drops it.
TEST_F(Test66ForegroundTaskHandoff, MemoryFreedInTheDeleterIsFreedOnceAtRelease) {
  FreeCounts counts;
  HookState hook(HookMode::kRefuse);
  std::shared_ptr<v8::BackingStore> outliving;
  {
    HookedEnv env(&hook);
    ASSERT_NE(env.env(), nullptr);
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(env.env(), &handles), napi_ok);
    napi_value array_buffer = nullptr;
    ASSERT_EQ(napi_v8_create_external_arraybuffer_with_free(env.env(), new uint8_t[16](), 16,
                                                            CountFree, &counts, &array_buffer),
              napi_ok);
    napi_value buffer = nullptr;
    ASSERT_EQ(napi_v8_create_external_buffer_with_free(env.env(), 16, new uint8_t[16](),
                                                       CountFree, &counts, &buffer),
              napi_ok);
    napi_value detached = nullptr;
    ASSERT_EQ(napi_v8_create_external_arraybuffer_with_free(env.env(), new uint8_t[16](), 16,
                                                            CountFree, &counts, &detached),
              napi_ok);
    // This provider's napi_value is a v8::Local.
    v8::Local<v8::Value> value;
    std::memcpy(static_cast<void*>(&value), &detached, sizeof(detached));
    outliving = value.As<v8::ArrayBuffer>()->GetBackingStore();
    ASSERT_EQ(napi_detach_arraybuffer(env.env(), detached), napi_ok);
    // Keeps the other two alive until the release.
    napi_value global = nullptr;
    ASSERT_EQ(napi_get_global(env.env(), &global), napi_ok);
    ASSERT_EQ(napi_set_named_property(env.env(), global, "liveArrayBuffer", array_buffer),
              napi_ok);
    ASSERT_EQ(napi_set_named_property(env.env(), global, "liveBuffer", buffer), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(env.env(), handles), napi_ok);
    ASSERT_EQ(unofficial_napi_collect_garbage(env.env()), napi_ok);
    EXPECT_EQ(counts.freed.load(), 0);
    env.Release();
  }
  EXPECT_EQ(counts.freed.load(), 3);
  outliving.reset();
  EXPECT_EQ(counts.freed.load(), 3);
  EXPECT_EQ(hook.released.load(), hook.enqueued.load());
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NAPI_TEST_ADDRESS_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define NAPI_TEST_ADDRESS_SANITIZER 1
#endif

// Bytes malloc has handed out and not had back, or 0 where unknown.
size_t MallocBytesInUse() {
#if defined(__APPLE__)
  malloc_statistics_t stats{};
  malloc_zone_statistics(nullptr, &stats);
  return stats.size_in_use;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
  const struct mallinfo2 info = mallinfo2();
  return info.uordblks + info.hblkhd;
#else
  return 0;
#endif
}

// unofficial_napi_create_uninitialized_arraybuffer mallocs its memory, so a
// dropped buffer's memory comes back as V8 collects it.
TEST_F(Test66ForegroundTaskHandoff, UninitializedArrayBuffersAreFreedWithoutACheckpoint) {
#if defined(NAPI_TEST_ADDRESS_SANITIZER)
  GTEST_SKIP() << "ASan holds freed memory in quarantine";
#endif
  if (MallocBytesInUse() == 0) GTEST_SKIP() << "no malloc statistics here";
  constexpr size_t kBufferSize = size_t{1} << 20;
  constexpr int kBuffers = 256;
  HookState hook(HookMode::kRefuse);
  HookedEnv env(&hook);
  ASSERT_NE(env.env(), nullptr);
  RunMicrotasks(env.env());
  const size_t base = MallocBytesInUse();
  for (int i = 0; i < kBuffers; ++i) {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(env.env(), &handles), napi_ok);
    napi_value buffer = nullptr;
    ASSERT_EQ(unofficial_napi_create_uninitialized_arraybuffer(env.env(), kBufferSize, false,
                                                               &buffer),
              napi_ok);
    ASSERT_EQ(napi_close_handle_scope(env.env(), handles), napi_ok);
    if (i % 64 == 63) ASSERT_EQ(unofficial_napi_collect_garbage(env.env()), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(env.env()), napi_ok);
  const size_t in_use = MallocBytesInUse();
  const size_t grown = in_use > base ? in_use - base : 0;
  EXPECT_LT(grown, 32 * kBufferSize)
      << (grown >> 20) << " MiB of " << ((kBuffers * kBufferSize) >> 20)
      << " MiB of dropped buffers still held before a checkpoint";
  env.Release();
}

}  // namespace
