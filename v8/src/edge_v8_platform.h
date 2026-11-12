#ifndef NAPI_V8_EDGE_V8_PLATFORM_H_
#define NAPI_V8_EDGE_V8_PLATFORM_H_

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <v8-platform.h>

#include "unofficial_napi.h"

class EdgeV8Platform final : public v8::Platform {
 public:
  struct FinishedCallback;
  struct IsolateState;

  static std::unique_ptr<EdgeV8Platform> Create();

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
  // The pool is process-wide and built when the first isolate is created, so
  // this has no effect afterwards; it returns false in that case.
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

  // While a FinalizationRegistry cleanup task waits to run, keeps `action`
  // to run once none is left (PumpPendingForegroundTasks) and returns true;
  // otherwise returns false and the caller runs it now.
  bool DeferUntilFinalizationCleanup(v8::Isolate* isolate, std::function<void()> action);

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

  std::shared_ptr<IsolateState> EnsureState(v8::Isolate* isolate);
  std::shared_ptr<ForegroundTaskRunner> EnsureRunner(v8::Isolate* isolate);
  std::shared_ptr<IsolateState> GetState(v8::Isolate* isolate);
  void BeginShutdown(const std::shared_ptr<IsolateState>& state);
  void MaybeFinishIsolate(const std::shared_ptr<IsolateState>& state,
                          bool begin_shutdown);

  std::unique_ptr<v8::Platform> fallback_;
  std::mutex mutex_;
  std::unordered_map<v8::Isolate*, std::shared_ptr<IsolateState>> isolates_;
};

#endif  // NAPI_V8_EDGE_V8_PLATFORM_H_
