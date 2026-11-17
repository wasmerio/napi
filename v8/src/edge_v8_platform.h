#ifndef NAPI_V8_EDGE_V8_PLATFORM_H_
#define NAPI_V8_EDGE_V8_PLATFORM_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <v8-platform.h>

#include "unofficial_napi.h"

// An embedder-owned background execution lane for the V8 work of one
// instance: a bounded queue that only the embedder's dedicated thread runs
// (snapi_v8_lane_run). The embedder owns the handle until it has called
// snapi_v8_lane_stop, snapi_v8_lane_run has returned, and every env created
// while the lane was bound has been released.
//
// While a lane is bound to a thread (snapi_v8_lane_swap_current), V8 worker
// tasks, delayed worker tasks and job workers that thread posts go to the
// lane. With no lane bound, they go to a process-wide worker pool, which
// exists only once an env was created with no lane bound (a standalone
// env). In a process without one, a worker task posted with no lane bound is
// dropped and counted (snapi_v8_unattributed_worker_posts), never run
// unattributed.
//
// `max_queued_tasks` bounds the queue. A post past it stops the lane and calls
// `on_overload(scope_context)`, which must stop the instance's JS without
// waiting on V8. `enter_scope(scope_context)` runs before each task, and
// `leave_scope(scope_context, scope)` after it with what enter returned; a
// null scope or a false leave stops the lane the same way. Null on a zero
// capacity or allocation failure.
extern "C" void* snapi_v8_lane_new(size_t max_queued_tasks,
                                   void* scope_context,
                                   void* (*enter_scope)(void*),
                                   bool (*leave_scope)(void*, void*),
                                   void (*on_overload)(void*));
// Runs the lane's tasks on the calling thread until snapi_v8_lane_stop.
extern "C" void snapi_v8_lane_run(void* handle);
// Closes admission and wakes the lane's thread, which returns after its
// current task. Idempotent; queued tasks are dropped.
extern "C" void snapi_v8_lane_stop(void* handle);
extern "C" void snapi_v8_lane_delete(void* handle);
// Binds `handle` (or no lane, for null) to the calling thread and returns the
// previous binding.
extern "C" void* snapi_v8_lane_swap_current(void* handle);
extern "C" void* snapi_v8_lane_current();
// The id of the lane bound to the calling thread, unique for the process, or
// 0 when none is.
extern "C" uint64_t snapi_v8_lane_current_id();
extern "C" bool snapi_v8_lane_is_running(void* handle);
extern "C" bool snapi_v8_lane_overloaded(void* handle);

// On Linux x64 with memory protection keys, V8 write-protects its JIT code
// with the key of the platform's ThreadIsolatedAllocator. A key's rights are
// per thread, and a thread that started before the key was allocated has no
// access to it. Gives the calling thread V8's default rights for the key
// (read-only) before it runs V8 work: embedder threads (an instance lane, a
// WASIX thread) can predate the platform. Elsewhere it does nothing.
void EdgeV8AllowJitKeyReads();

class EdgeV8Platform final : public v8::Platform {
 public:
  struct FinishedCallback;
  struct IsolateState;

  // With `standalone_workers`, the process-wide worker pool is built now;
  // otherwise only when EnableStandaloneWorkers first runs.
  static std::unique_ptr<EdgeV8Platform> Create(bool standalone_workers);
  // Builds the process-wide worker pool for envs created with no lane bound.
  // Idempotent. False if it could not be built.
  bool EnableStandaloneWorkers();

  // Sets how many background worker threads the platform gets. Zero restores
  // V8's own default, which sizes the pool from the host's processor count --
  // reasonable for a process running one JS app, less so for a host running
  // many, where those threads compete with every tenant's foreground JS and
  // their work is not attributed to anyone.
  //
  // A job that nobody waits for uses at most count - 1 of these threads,
  // leaving one for other worker tasks; a job that is kUserBlocking or joined
  // uses all of them. With a count of 1, every job gets the one thread.
  //
  // The pool is process-wide and serves only envs created with no lane bound.
  // It is built with the first of them, but the setting freezes when the
  // platform is created, also when its first env has a lane; it returns false
  // for a different count afterwards. An instance lane is one thread.
  static bool SetWorkerThreadCount(int count);

  // Sets a function to call when an immediate foreground task is queued
  // where only an event-loop checkpoint runs it. For an isolate with no
  // foreground task target bound (see BindForegroundTaskTarget), a task waits
  // in the default platform's queue until PumpPendingForegroundTasks runs it.
  // V8 posts such tasks from any thread, for example to resolve an
  // Atomics.waitAsync that another thread's Atomics.notify woke, or to finish
  // a WebAssembly.compile. An embedder whose thread sleeps until its next
  // event can wake that thread from this function.
  //
  // The function is process-wide: it is called for every isolate's tasks, on
  // the thread that posted the task, after the task is queued. That thread
  // can be the isolate's own, in the middle of running JS or a garbage
  // collection, and V8 may hold its own locks then. So the function must be
  // quick, must not call into V8, and must not take a lock that the
  // isolate's thread may hold. A delayed task doesn't call it, and runs at
  // the first checkpoint after its delay. nullptr clears it. A thread that
  // read the previous function just before a change may still call it once
  // afterwards. It takes no data pointer, so that one atomic store publishes
  // it. WakeForQueuedForegroundTasks calls it too.
  static void SetForegroundTaskWaker(void (*waker)());

  ~EdgeV8Platform() override;

  bool RegisterIsolate(v8::Isolate* isolate);
  void AddIsolateFinishedCallback(v8::Isolate* isolate,
                                  void (*callback)(void*),
                                  void* data);
  void NotifyIsolateShutdown(v8::Isolate* isolate);
  void DisposeIsolate(v8::Isolate* isolate);
  void UnregisterIsolate(v8::Isolate* isolate);
  bool BindForegroundTaskTarget(v8::Isolate* isolate,
                                napi_env env,
                                unofficial_napi_enqueue_foreground_task_callback callback,
                                void* target);
  void ClearForegroundTaskTarget(v8::Isolate* isolate, napi_env env);
  void AddPendingForegroundTask(const std::shared_ptr<IsolateState>& state);
  void CompletePendingForegroundTask(const std::shared_ptr<IsolateState>& state);
  // Run at most one ready fallback task. Returns true when a task ran so
  // the embedder can admit another loop turn before continuing provider work.
  bool PumpPendingForegroundTasks(v8::Isolate* isolate);
  // Calls the function SetForegroundTaskWaker set, on this thread, when the
  // isolate may have tasks waiting that only a checkpoint runs: it has used
  // the default platform's queue and has tasks pending. Their posts called it
  // once, but a checkpoint that refused to run them (while the env
  // terminated) left them queued, and nothing calls it for them again. The
  // pending tasks may also be delayed ones that aren't due yet, or ones
  // handed to a bound target: the embedder then finds nothing to run and
  // waits again.
  void WakeForQueuedForegroundTasks(v8::Isolate* isolate);

  int NumberOfWorkerThreads() override;
  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
      v8::Isolate* isolate, v8::TaskPriority priority) override;
  bool IdleTasksEnabled(v8::Isolate* isolate) override;
  double MonotonicallyIncreasingTime() override;
  double CurrentClockTimeMillis() override;
  v8::TracingController* GetTracingController() override;
  v8::PageAllocator* GetPageAllocator() override;
  v8::ThreadIsolatedAllocator* GetThreadIsolatedAllocator() override;
  void OnCriticalMemoryPressure() override;
  void DumpWithoutCrashing() override;
  v8::HighAllocationThroughputObserver* GetHighAllocationThroughputObserver() override;
  StackTracePrinter GetStackTracePrinter() override;
  std::unique_ptr<v8::ScopedBlockingCall> CreateBlockingScope(
      v8::BlockingType blocking_type) override;

 protected:
  std::unique_ptr<v8::JobHandle> CreateJobImpl(
      v8::TaskPriority priority,
      std::unique_ptr<v8::JobTask> job_task,
      const v8::SourceLocation& location) override;
  void PostTaskOnWorkerThreadImpl(v8::TaskPriority priority,
                                  std::unique_ptr<v8::Task> task,
                                  const v8::SourceLocation& location) override;
  void PostDelayedTaskOnWorkerThreadImpl(
      v8::TaskPriority priority,
      std::unique_ptr<v8::Task> task,
      double delay_in_seconds,
      const v8::SourceLocation& location) override;

 private:
  class ForegroundTaskRunner;

  explicit EdgeV8Platform(std::unique_ptr<v8::Platform> fallback);
  v8::Platform* StandaloneWorkers();

  std::shared_ptr<IsolateState> EnsureState(v8::Isolate* isolate);
  std::shared_ptr<ForegroundTaskRunner> EnsureRunner(v8::Isolate* isolate);
  std::shared_ptr<IsolateState> GetState(v8::Isolate* isolate);
  void BeginShutdown(const std::shared_ptr<IsolateState>& state);
  void MaybeFinishIsolate(const std::shared_ptr<IsolateState>& state,
                          bool begin_shutdown);

  // Foreground runners, clocks and allocators. It has no worker threads: a
  // worker task belongs to an instance lane or the standalone pool.
  std::unique_ptr<v8::Platform> fallback_;
  // The process-wide pool for envs created with no lane bound.
  std::mutex standalone_workers_mutex_;
  std::unique_ptr<v8::Platform> standalone_workers_;
  std::atomic<v8::Platform*> standalone_workers_ptr_{nullptr};
  std::mutex mutex_;
  std::unordered_map<v8::Isolate*, std::shared_ptr<IsolateState>> isolates_;
};

#endif  // NAPI_V8_EDGE_V8_PLATFORM_H_
