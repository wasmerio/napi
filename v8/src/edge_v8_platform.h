#ifndef NAPI_V8_EDGE_V8_PLATFORM_H_
#define NAPI_V8_EDGE_V8_PLATFORM_H_

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <v8-platform.h>

#include "unofficial_napi.h"

// An embedder-owned background execution lane. The returned handle is owned by
// the embedder until Stop, Run and all associated N-API environments have
// quiesced. A null current lane uses the process-wide worker pool only after a
// standalone environment explicitly enables it.
extern "C" void* snapi_v8_lane_new(size_t max_queued_tasks,
                                     void* scope_context,
                                     void* (*enter_scope)(void*),
                                     bool (*leave_scope)(void*, void*),
                                     void (*on_overload)(void*));
extern "C" void snapi_v8_lane_run(void* handle);
extern "C" void snapi_v8_lane_stop(void* handle);
extern "C" void snapi_v8_lane_delete(void* handle);
extern "C" void* snapi_v8_lane_swap_current(void* handle);
extern "C" void* snapi_v8_lane_current();
extern "C" bool snapi_v8_lane_is_running(void* handle);
extern "C" bool snapi_v8_lane_overloaded(void* handle);
// Attaches the embedder's memory accountant to a lane. Commits of page-backed
// buffers (resizable ArrayBuffers, growable SharedArrayBuffers) reserved while
// the lane is bound are charged through `charge`, which must deny without side
// effects, and returned through `uncharge`. On success the lane and every
// region it reserved share ownership of `context`; `release` runs once when
// the last of them is gone. Returns false, leaving `context` with the caller,
// if the lane already has an accountant.
//
// Once any lane has an accountant, a buffer-shaped reservation made with no
// attributable lane bound (and outside runtime/isolate setup) is refused,
// failing closed in JS as a RangeError. A process that also enabled the
// standalone worker pool (it runs contexts without a lane) opts into
// leniency instead: such reservations pass through unmetered. Both cases are
// counted by snapi_v8_unattributed_page_reservations().
extern "C" bool snapi_v8_lane_set_page_accountant(
    void* handle, void* context, bool (*charge)(void*, uint64_t),
    void (*uncharge)(void*, uint64_t), void (*release)(void*));

// While alive on a thread, page reservations are treated as V8's own and
// never attributed to the bound lane. Only for runtime and isolate setup,
// where no guest code runs.
class EdgeV8PageAttributionPause {
 public:
  EdgeV8PageAttributionPause();
  ~EdgeV8PageAttributionPause();
  EdgeV8PageAttributionPause(const EdgeV8PageAttributionPause&) = delete;
  EdgeV8PageAttributionPause& operator=(const EdgeV8PageAttributionPause&) = delete;
};

class EdgeV8Platform final : public v8::Platform {
 public:
  struct FinishedCallback;
  struct IsolateState;

  static std::unique_ptr<EdgeV8Platform> Create(bool standalone_workers);
  bool EnableStandaloneWorkers();

  // Sets how many background worker threads the platform gets. Zero restores
  // V8's own default, which sizes the pool from the host's processor count --
  // reasonable for a process running one JS app, less so for a host running
  // many, where those threads compete with every tenant's foreground JS and
  // their work is not attributed to anyone.
  //
  // The standalone pool is process-wide and built only when a standalone
  // environment is configured. The setting freezes with V8 platform creation,
  // even when the first environment is managed.
  static bool SetWorkerThreadCount(int count);

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
  void PumpPendingForegroundTasks(v8::Isolate* isolate);

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

  std::unique_ptr<v8::Platform> fallback_;
  // Meters page-backed buffers; see MeteringPageAllocator.
  std::unique_ptr<v8::PageAllocator> page_allocator_;
  // Managed isolates dispatch every worker task to their instance lane. The
  // process-wide V8 pool exists only when a standalone environment is used.
  std::mutex standalone_workers_mutex_;
  std::unique_ptr<v8::Platform> standalone_workers_;
  std::atomic<v8::Platform*> standalone_workers_ptr_{nullptr};
  std::mutex mutex_;
  std::unordered_map<v8::Isolate*, std::shared_ptr<IsolateState>> isolates_;
};

#endif  // NAPI_V8_EDGE_V8_PLATFORM_H_
