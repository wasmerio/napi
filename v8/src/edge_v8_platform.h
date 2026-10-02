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

// Reasons passed to snapi_v8_wasm_accounting::on_code_limit.
enum : uint32_t {
  // The context's committed wasm code exceeded code_budget_bytes.
  SNAPI_V8_WASM_CODE_LIMIT_BUDGET = 1,
  // charge_code refused a commit (the memory limit is exhausted).
  SNAPI_V8_WASM_CODE_LIMIT_MEMORY = 2,
};

// Per-context WebAssembly limits and accounting, attached to a lane that
// already has a page accountant (same `context`). With it, the lane's context
// is metered for WebAssembly:
//
// * Wasm memories (no-access reservations aligned to the 64 KiB wasm page)
//   are counted and capped by number (`max_memories`) and reserved address
//   space (`max_reserved_bytes`); a reservation over a cap is refused, which
//   V8 reports as a RangeError. Their committed bytes are charged through
//   `charge_memory`, which must deny without side effects (V8 turns a refusal
//   into a failed memory.grow or a RangeError).
// * Committed wasm code is charged after the fact through `charge_code`:
//   V8 aborts the process if a code commit fails, so it is never refused.
//   When `charge_code` refuses or the context's committed code exceeds
//   `code_budget_bytes`, `on_code_limit` runs once (possibly on a V8
//   background thread, inside V8): it must stop the context without
//   blocking on V8 or N-API locks. New compilations in the context are
//   refused from then on (CompileError), as they are while the process-wide
//   code budget is exhausted.
//
// Without it a lane's wasm memories are charged like other page-backed
// buffers and wasm code is not metered. Returns false if the lane has no page
// accountant or already has wasm accounting.
struct snapi_v8_wasm_accounting {
  uint32_t size;  // sizeof(snapi_v8_wasm_accounting)
  uint32_t max_memories;
  uint64_t max_reserved_bytes;
  uint64_t code_budget_bytes;
  bool (*charge_memory)(void* context, uint64_t bytes);
  void (*uncharge_memory)(void* context, uint64_t bytes);
  bool (*charge_code)(void* context, uint64_t bytes);
  void (*uncharge_code)(void* context, uint64_t bytes);
  void (*on_code_limit)(void* context, uint32_t reason, uint64_t committed,
                        uint64_t limit);
};
extern "C" bool snapi_v8_lane_set_wasm_accounting(
    void* handle, const snapi_v8_wasm_accounting* accounting);

struct snapi_v8_wasm_usage {
  uint64_t memories;          // live wasm memories reserved by the context
  uint64_t reserved_bytes;    // their reserved address space
  uint64_t code_bytes;        // committed wasm code charged to the context
};
// Zeroes `out` for a lane without wasm accounting.
extern "C" void snapi_v8_lane_wasm_usage(void* handle,
                                         snapi_v8_wasm_usage* out);

struct snapi_v8_wasm_stats {
  uint64_t code_committed_bytes;   // all metered wasm code in the process
  uint64_t code_budget_bytes;      // process-wide soft budget (0: unmetered)
  uint64_t memory_cap_denials;     // wasm memory reservations over a cap
  uint64_t codegen_denials;        // compilations refused by a code budget
  uint64_t code_limit_stops;       // contexts stopped by on_code_limit
  uint64_t code_decommitted_bytes; // wasm code returned by V8 (cumulative)
  uint64_t memory_committed_bytes; // committed bytes of metered wasm memories
  uint64_t memories;               // live metered wasm memories
};
extern "C" void snapi_v8_wasm_process_stats(snapi_v8_wasm_stats* out);

// Process-wide V8 WebAssembly limits for metered contexts. Must be set before
// the runtime is configured (the first environment is created): the derived
// V8 flags are process-wide and frozen when V8 initializes. Repeating the
// same configuration later succeeds; a different one fails.
//
// Sets --wasm-max-mem-pages, --wasm-max-module-size, --max-wasm-functions,
// --no-wasm-native-module-cache and, if `liftoff_only`, --liftoff-only, and
// enables wasm code metering with the given process-wide soft code budget.
// Returns napi_ok, napi_invalid_arg for out-of-range values, or
// napi_generic_failure once V8 runs with a different configuration.
struct snapi_v8_wasm_engine_config {
  uint32_t size;                // sizeof(snapi_v8_wasm_engine_config)
  uint32_t max_memory_pages;    // 1..=65536 (64 KiB pages, per memory)
  uint64_t max_module_bytes;    // 16..=1 GiB
  uint32_t max_functions;       // 1..=1,000,000 per module
  uint32_t liftoff_only;        // 0 or 1
  uint64_t process_code_budget_bytes;  // 1 MiB..=3 GiB
};
extern "C" int snapi_v8_configure_wasm_engine(
    const snapi_v8_wasm_engine_config* config);

// Process-wide wasm code metering, enabled once by the runtime configuration
// before V8 initializes. `process_code_budget_bytes` is the soft budget past
// which new compilations in metered contexts are refused.
void EdgeV8EnableWasmCodeMetering(uint64_t process_code_budget_bytes);
// Whether the calling thread's lane meters WebAssembly and the process
// meters wasm code: required to create an environment with metered
// WebAssembly.
bool EdgeV8WasmMeteringReady();
// Dynamic part of the wasm code-generation policy: false when the calling
// thread's metered context or the process exhausted its code budget. Always
// true for contexts without wasm accounting.
bool EdgeV8AdmitWasmCodegen();

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
