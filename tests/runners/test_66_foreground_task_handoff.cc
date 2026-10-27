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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "test_env.h"

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

  std::mutex mutex;
  std::condition_variable cv;
  bool closed = false;
  std::deque<HookedTask> queue;

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
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->closed) {
          state->queue.push_back(task);
          state->cv.notify_all();
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

}  // namespace
