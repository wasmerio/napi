#ifndef UNOFFICIAL_NAPI_H_
#define UNOFFICIAL_NAPI_H_

#include <stdint.h>

#include "js_native_api.h"

// Marks the API kept only for compatibility with code written against
// version 1 of these extensions: the contextify calls, the context-token
// hooks and the module options' `context_or_undefined`. It still works and
// is removed in a future major release. Define
// UNOFFICIAL_NAPI_NO_DEPRECATION_WARNINGS to silence the warnings.
#if defined(UNOFFICIAL_NAPI_NO_DEPRECATION_WARNINGS)
#define UNOFFICIAL_NAPI_DEPRECATED(message)
#elif defined(__GNUC__) || defined(__clang__)
#define UNOFFICIAL_NAPI_DEPRECATED(message) __attribute__((deprecated(message)))
#elif defined(_MSC_VER)
#define UNOFFICIAL_NAPI_DEPRECATED(message) __declspec(deprecated(message))
#else
#define UNOFFICIAL_NAPI_DEPRECATED(message)
#endif

// The same for struct fields and enumerators, which MSVC cannot mark.
#if !defined(UNOFFICIAL_NAPI_NO_DEPRECATION_WARNINGS) && \
    (defined(__GNUC__) || defined(__clang__))
#define UNOFFICIAL_NAPI_DEPRECATED_MEMBER(message) __attribute__((deprecated(message)))
#else
#define UNOFFICIAL_NAPI_DEPRECATED_MEMBER(message)
#endif

// Silences deprecation warnings in between, for code that implements the
// deprecated API. Around a struct with deprecated fields it also keeps C++
// compilers from warning wherever the struct is merely copied.
#if defined(__GNUC__) || defined(__clang__)
#define UNOFFICIAL_NAPI_SUPPRESS_DEPRECATED_BEGIN \
  _Pragma("GCC diagnostic push")                  \
  _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#define UNOFFICIAL_NAPI_SUPPRESS_DEPRECATED_END _Pragma("GCC diagnostic pop")
#elif defined(_MSC_VER)
#define UNOFFICIAL_NAPI_SUPPRESS_DEPRECATED_BEGIN \
  __pragma(warning(push)) __pragma(warning(disable : 4996))
#define UNOFFICIAL_NAPI_SUPPRESS_DEPRECATED_END __pragma(warning(pop))
#else
#define UNOFFICIAL_NAPI_SUPPRESS_DEPRECATED_BEGIN
#define UNOFFICIAL_NAPI_SUPPRESS_DEPRECATED_END
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct uv_loop_s;

// Provider-owned resources used during environment construction and teardown.
// Their representations are private to the provider; callers may only move
// them through the operations that name the corresponding handle type.
typedef struct unofficial_napi_env_owner__* unofficial_napi_env_owner;
typedef struct unofficial_napi_guest_heap__* unofficial_napi_guest_heap;

enum { UNOFFICIAL_NAPI_ENV_CREATE_OPTIONS_VERSION = 1 };

enum { UNOFFICIAL_NAPI_RUNTIME_OPTIONS_VERSION = 1 };

// Configuration for the provider's process-global JavaScript runtime. This
// must be applied before creating an environment. Repeating an identical
// configuration is idempotent; a conflicting configuration is rejected.
// Pointed-to strings are observed only for the duration of the call.
typedef struct {
  uint32_t size;
  uint32_t version;
  const char* engine_flags;
  size_t engine_flags_length;
} unofficial_napi_runtime_options;

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_configure_runtime(
    const unofficial_napi_runtime_options* options_or_null);

// All provider configuration needed before engine/isolate creation. The
// descriptor is observed only for the duration of unofficial_napi_create_env;
// pointed-to strings remain owned by the caller. `size` and `version` make
// additions ABI-compatible. Process-global configuration is deliberately
// excluded and belongs to unofficial_napi_configure_runtime.
typedef struct {
  uint32_t size;
  uint32_t version;
  uint64_t total_memory;
  uint64_t constrained_memory;
  size_t max_young_generation_size_in_bytes;
  size_t max_old_generation_size_in_bytes;
  size_t code_range_size_in_bytes;
  void* stack_limit;
  /* Opaque guest-heap resource (see napi_host_guest_heap_alloc). When set, the
   * env's array-buffer allocator places every backing store in the guest's
   * linear memory from isolate birth. Ownership transfers only after size and
   * version validation proves that this field is present, before validating
   * any other argument. It is then released exactly once via
   * napi_host_guest_heap_release (by the allocator destructor, or on any later
   * env-creation failure). */
  unofficial_napi_guest_heap guest_heap;
} unofficial_napi_env_create_options;

// Deprecated with the context-token hooks below.
typedef void (*unofficial_napi_context_token_callback)(napi_env env,
                                                       void* token,
                                                       void* data);
typedef void (*unofficial_napi_foreground_task_callback)(napi_env env,
                                                         void* data);
typedef void (*unofficial_napi_foreground_task_cleanup)(napi_env env,
                                                        void* data);
// Embedder hook that queues provider work (engine tasks, finalizer drains) to
// run on the env's thread. The provider may call it from any thread, including
// from the middle of a garbage collection. Each call hands over one task:
//
// - The hook owns `data` from the moment it is called, whatever it returns.
//   The provider never touches `data` after handing it over, so the hook may
//   run or release the task, on any thread, before the call has returned.
// - The hook never runs `callback` from inside the call. It runs
//   `callback(env, data)` at most once, later, on the env's thread, no sooner
//   than `delay_millis` from now.
// - When `cleanup` is not NULL, the hook calls `cleanup(env, data)` exactly
//   once, as the last use of `data`: right after `callback` returns, or
//   instead of it when the task is dropped (refused, or discarded when the env
//   shuts down). This holds when the hook returns an error, too. `cleanup` may
//   be called from any thread, including from inside the call.
// - The hook returns napi_ok when the task may still run, and an error only
//   when `callback` will never run.
//
// The provider does not retry or reroute a refused task; it is dropped.
typedef napi_status (*unofficial_napi_enqueue_foreground_task_callback)(
    void* target,
    unofficial_napi_foreground_task_callback callback,
    void* data,
    unofficial_napi_foreground_task_cleanup cleanup,
    uint64_t delay_millis);
typedef void (*unofficial_napi_fatal_error_callback)(napi_env env,
                                                     const char* location,
                                                     const char* message);
typedef void (*unofficial_napi_oom_error_callback)(napi_env env,
                                                   const char* location,
                                                   bool is_heap_oom,
                                                   const char* detail);

typedef enum {
  unofficial_napi_env_hook_context_token_assign
      UNOFFICIAL_NAPI_DEPRECATED_MEMBER("realms replace context tokens") = 1u << 0,
  unofficial_napi_env_hook_context_token_unassign
      UNOFFICIAL_NAPI_DEPRECATED_MEMBER("realms replace context tokens") = 1u << 1,
  unofficial_napi_env_hook_enqueue_foreground_task = 1u << 2,
  unofficial_napi_env_hook_fatal_error = 1u << 3,
  unofficial_napi_env_hook_oom_error = 1u << 4,
} unofficial_napi_env_hook;

enum { UNOFFICIAL_NAPI_ENV_HOOKS_VERSION = 1 };

// Immutable embedder callbacks attached as one complete environment state
// transition. `size` and `version` make additions ABI-compatible. `data` is
// owned by the embedder and must remain valid until the environment is released.
//
// Deprecated: the context-token callbacks. Use unofficial_napi_create_realm and
// keep per-realm state keyed by the realm's global instead. They are kept for
// compatibility and removed in a future major release. A provider that accepts
// them (V8) gives every realm a token, an opaque pointer that stays the same
// for the realm and is not reused while assigned. It calls
// `context_token_assign_callback(env, token, data)` when the realm is created
// (unofficial_napi_create_realm, and so unofficial_napi_contextify_make_context),
// and `context_token_unassign_callback(env, token, data)` exactly once for
// each token, before the env is freed: when the provider frees the record of
// a collected realm (in unofficial_napi_create_realm and
// unofficial_napi_collect_garbage), or, for a realm still alive, during the
// env's release (unofficial_napi_release_env), which unassigns the tokens of
// the realms alive when it begins before the cleanup hooks run, and those of
// realms made while it runs too. Both run in the env's realm. Realms made
// before the hooks were attached get no token. QuickJS does not accept them
// and never calls them.
//
// Outside the release, V8 calls them as they are: in no TryCatch, with no
// pending exception set aside. An exception the assign callback leaves
// pending is still pending when unofficial_napi_create_realm returns napi_ok.
// The unassign callback of a collected realm, which
// unofficial_napi_create_realm or unofficial_napi_collect_garbage runs, sees
// its caller's state: an exception the caller has pending is pending in the
// callback too, and one the callback leaves pending replaces it and is the
// caller's when the call returns its own status. At the release, what they
// throw is dropped (see unofficial_napi_release_env).
UNOFFICIAL_NAPI_SUPPRESS_DEPRECATED_BEGIN
typedef struct {
  uint32_t size;
  uint32_t version;
  void* data;
  unofficial_napi_context_token_callback context_token_assign_callback
      UNOFFICIAL_NAPI_DEPRECATED_MEMBER("realms replace context tokens");
  unofficial_napi_context_token_callback context_token_unassign_callback
      UNOFFICIAL_NAPI_DEPRECATED_MEMBER("realms replace context tokens");
  unofficial_napi_enqueue_foreground_task_callback enqueue_foreground_task_callback;
  unofficial_napi_fatal_error_callback fatal_error_callback;
  unofficial_napi_oom_error_callback oom_error_callback;
} unofficial_napi_env_hooks;

// Reads the deprecated fields too, without warning.
static inline uint64_t unofficial_napi_env_hooks_requested(
    const unofficial_napi_env_hooks* hooks) {
  if (hooks == NULL) return 0;
  uint64_t requested = 0;
  if (hooks->context_token_assign_callback != NULL)
    requested |= unofficial_napi_env_hook_context_token_assign;
  if (hooks->context_token_unassign_callback != NULL)
    requested |= unofficial_napi_env_hook_context_token_unassign;
  if (hooks->enqueue_foreground_task_callback != NULL)
    requested |= unofficial_napi_env_hook_enqueue_foreground_task;
  if (hooks->fatal_error_callback != NULL)
    requested |= unofficial_napi_env_hook_fatal_error;
  if (hooks->oom_error_callback != NULL)
    requested |= unofficial_napi_env_hook_oom_error;
  return requested;
}
UNOFFICIAL_NAPI_SUPPRESS_DEPRECATED_END

// V8 provider: when the host process sets NAPI_V8_CPU_PROFILE_DIR, each env
// made by unofficial_napi_create_env records a CPU profile, which
// unofficial_napi_release_env writes to that directory. See v8/README.md,
// "Diagnostics".
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_create_env(
    int32_t module_api_version,
    const unofficial_napi_env_create_options* options_or_null,
    napi_env* env_out,
    unofficial_napi_env_owner* owner_out);
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_attach_env(
    napi_env env,
    const unofficial_napi_env_hooks* hooks,
    uint64_t* accepted_hooks_out);
// Releases an env made by unofficial_napi_create_env and the engine instance
// it owns. Before anything is freed, the release runs what the env still
// holds, in this order (the providers' contract; Node up to 20 also finalized
// the instance data after everything else, Node 25 interleaves it with the
// other finalizers):
//
// 1. The cleanup hooks run, most recent first. A hook added while releasing,
//    by a hook or by a finalizer, runs too.
// 2. The finalizer of every value still pending runs: napi_wrap,
//    napi_create_external, napi_add_finalizer, external ArrayBuffers and
//    Buffers. Values of one kind are finalized most recent first, as Node's
//    FinalizeAll does (also when a finalizer runs
//    unofficial_napi_event_loop_checkpoint); the order between kinds is not
//    specified. An external ArrayBuffer or Buffer is detached before its
//    finalizer runs, as Node's CallbackInfo::Clean does, so the finalizer and
//    the code that runs after it see it detached (no data, byte length 0),
//    never the freed memory. The exception is a buffer that
//    ArrayBuffer.prototype.transfer moved the memory to: neither provider can
//    find it to detach it (QuickJS learns of it only once it is freed, and
//    Node cannot either), so the finalizer still runs in its turn and frees
//    memory that buffer still exposes; code that reads it later in the
//    release reads freed memory, as in Node. (Waiting for that buffer to be
//    freed would leak the memory instead: a buffer still alive is freed only
//    after this pass.) A wrapped object lets go of its wrap before the wrap's
//    finalizer runs: from then on napi_unwrap and napi_remove_wrap return
//    napi_invalid_arg for it, as for an object never wrapped. 1 and 2 repeat
//    until neither has anything left.
// 3. The instance-data finalizer runs, after all of those, whatever the data
//    (NULL too), as in Node.
// 4. 1 to 3 repeat for whatever the instance-data finalizer leaves pending
//    (values it made and kept, hooks it added, instance data it set), until
//    nothing is pending, as Node's FinalizeAll finalizes until its lists are
//    empty. An addon that makes a new value in every finalizer never
//    finishes, as in Node.
//
// With the deprecated context-token callbacks (unofficial_napi_env_hooks;
// only V8 accepts them), each pass of 1 and 2 unassigns the token of every
// realm that has one when it begins, and again between 1 and 2. A realm made
// while releasing is pending until the first of these sweeps after it is
// made unassigns its token (or earlier, if it is collected and its record
// freed first). So the realms alive when the release begins are unassigned
// before any cleanup hook runs, and a realm made during 1 (by a cleanup hook,
// or by a finalizer that runs inside one) is unassigned before the
// finalizers of 2 run, as main did. A realm a finalizer of 2 or the
// instance-data finalizer makes is unassigned when the next pass begins: the
// finalizers after it in 2 see its token still assigned. One an unassign
// callback makes while a sweep runs it is unassigned in the same sweep,
// before the pass goes on. One made by an unassign callback that
// unofficial_napi_create_realm or unofficial_napi_collect_garbage runs (for
// a collected realm, from a hook or finalizer of the release) waits, as any
// realm made while releasing, for the first sweep after it. So every token
// is unassigned once before the env is freed. What an unassign callback
// makes besides (values, hooks, instance data) is released as the rest. An
// unassign callback that makes a realm every time it is called never lets
// the release finish, as a finalizer that makes a new value every time does.
//
// After each hook, token callback and finalizer, an exception it left
// pending is dropped: nothing can catch it then (Node's CallIntoModule drops
// it too once JS can no longer run). An engine may run a finalizer in the
// middle of other code, when that code frees the last reference to a value
// (QuickJS does; V8 runs the unassign callback of a collected realm inside
// unofficial_napi_create_realm and unofficial_napi_collect_garbage): there
// only the finalizer's own exception is dropped, and one that JS is throwing,
// or that a hook has pending, is kept. Each runs on the env's thread, in the
// env's realm (also when another realm was entered when the release began),
// with N-API usable: scripts run, and so do the native functions they or
// native code call (napi_create_function, napi_define_class, the serdes
// binding). Node runs no JavaScript there. References made with
// napi_create_reference keep their values until all of them have run. A
// finalizer the engine would call after this pass, while it frees the engine
// instance, is dropped, and nothing touches the env once it is freed. Called
// again for the same owner while its release runs (from a hook, a finalizer
// or a native function they reach), it returns napi_invalid_arg and does
// nothing; the release under way goes on.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_release_env(
    unofficial_napi_env_owner owner,
    struct uv_loop_s* loop_or_null);

// Realms. An engine instance holds realms, each with its own global and
// intrinsics: the env's own realm (the engine instance's main realm) and the
// realms unofficial_napi_create_realm makes, such as vm contexts. A realm is
// not an env: one env serves all of them, and its hooks, instance data and
// cleanup hooks are shared by all of them. A hook fires for code running in
// any realm.
//
// N-API acts in one realm at a time, the current realm, as an ECMAScript
// engine runs code in the realm of its running execution context:
//
// - A call that creates something creates it in the current realm, with that
//   realm's intrinsics: objects, arrays, functions, classes, errors (also the
//   ones napi_throw_* makes), promises, dates, array buffers, typed arrays and
//   DataViews. Nothing reads the realm's global for them, which its scripts
//   control. A buffer is the current realm's Uint8Array with the env realm's
//   Buffer.prototype, since Buffer belongs to the env. napi_get_global returns the current realm's global (a sandbox realm's
//   sandbox), and napi_run_script runs there. (QuickJS copies a sandbox in
//   and out around each script; see docs/realms.md.)
// - A native function belongs to the realm that was current when it was made,
//   as an ECMAScript built-in function has a [[Realm]]. That covers
//   napi_create_function, and the constructors, methods and accessors of
//   napi_define_class and napi_define_properties. Its callback runs with that
//   realm current, whichever realm calls it.
// - A promise from napi_create_promise belongs to the current realm, and
//   napi_resolve_deferred and napi_reject_deferred settle it in that realm.
// - unofficial_napi_enter_realm makes a realm current until the matching
//   unofficial_napi_leave_realm.
// - Otherwise the current realm is the env's own. An embedder that never
//   enters a realm only ever acts in the env's realm.
// - Finalizers, cleanup hooks, env hooks and interrupt callbacks run in the
//   env's realm.
//
// The calls that run code in a realm take it explicitly
// (unofficial_napi_run_script, unofficial_napi_compile_function, module
// creation and unofficial_napi_bytecode_open). There a NULL realm means the
// env's own realm, also while another realm is current.
//
// N-API's threadsafe functions and async work belong to the embedder. To run
// their callbacks in the realm that made them, it keeps a reference to the
// global napi_get_global returns when they are made, and enters the realm
// unofficial_napi_get_realm finds for that global around each callback.
//
// The host-JS provider's realms share the host realm's intrinsics: there,
// entering a realm selects its global, and objects still come from the host.
//
// A realm lives while its global (the sandbox, when it has one) or anything
// made in it is reachable, and is then collected. A realm handle, like a
// napi_value, is valid only while the handle scope it was obtained in is open,
// and two handles for one realm may differ; unofficial_napi_get_realm finds
// the realm again from its global. Using a handle after its handle scope has
// closed is undefined behavior that the provider may not detect: once the
// realm is collected, a newer realm can get the same handle. To use a realm
// later, keep its global (in a napi_ref, say) and call
// unofficial_napi_get_realm on it each time.
typedef struct unofficial_napi_realm__* unofficial_napi_realm;

enum { UNOFFICIAL_NAPI_REALM_OPTIONS_VERSION = 1 };

// With a sandbox object, global property access in the new realm reads and
// writes the sandbox, as with vm.createContext(sandbox). Otherwise the realm
// gets a fresh global. `name`, `origin` and `host_defined_option_id` may be
// NULL. The descriptor is observed only for the duration of the call.
//
// Each provider honors only some fields. It accepts the rest, ignores them
// and still returns napi_ok:
// - `sandbox_or_null`: every provider.
// - `host_defined_option_id`: V8 and QuickJS.
// - `allow_code_gen_strings`: V8 only. When false, eval and the Function
//   constructors throw an EvalError in the realm.
// - `own_microtask_queue`: V8 only.
// - `allow_code_gen_wasm`, `name` and `origin`: no provider. WebAssembly code
//   generation stays allowed wherever the engine has WebAssembly.
typedef struct {
  uint32_t size;
  uint32_t version;
  napi_value sandbox_or_null;
  napi_value name;
  napi_value origin;
  napi_value host_defined_option_id;
  bool allow_code_gen_strings;
  bool allow_code_gen_wasm;
  bool own_microtask_queue;
} unofficial_napi_realm_options;

static inline void unofficial_napi_realm_options_init(
    unofficial_napi_realm_options* options) {
  if (options == NULL) return;
#ifdef __cplusplus
  *options = {};
#else
  *options = (unofficial_napi_realm_options){0};
#endif
  options->size = sizeof(*options);
  options->version = UNOFFICIAL_NAPI_REALM_OPTIONS_VERSION;
  options->allow_code_gen_strings = true;
  options->allow_code_gen_wasm = true;
}

// Creates a realm in `env`'s engine instance. `global_out` receives the
// sandbox when one was given, else the realm's global; it may be NULL. A NULL
// `options` means the defaults of unofficial_napi_realm_options_init. A sandbox
// belongs to at most one realm; the env's own global may still be one.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_create_realm(
    napi_env env,
    const unofficial_napi_realm_options* options_or_null,
    unofficial_napi_realm* realm_out,
    napi_value* global_out);

// Returns the realm whose global is `value`: a realm's own global, or the
// sandbox given to unofficial_napi_create_realm. `*realm_out` is NULL for any
// other object, the env's own global included. Fails with
// napi_object_expected when `value` is not an object.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_get_realm(
    napi_env env,
    napi_value value,
    unofficial_napi_realm* realm_out);

// A scope opened by unofficial_napi_enter_realm. A token is never 0 and never
// reused within its env.
typedef uint64_t unofficial_napi_realm_scope;

// Makes `realm_or_null` the current realm (NULL: the env's own) until
// unofficial_napi_leave_realm closes the returned scope. Scopes nest and close
// in LIFO order, and a scope keeps its realm alive until it is closed. A scope
// that a native callback leaves open closes when that callback returns, and
// every scope closes when the env is released.
//
// No JavaScript runs, so entering also succeeds while execution is
// terminating. Fails with napi_invalid_arg for a realm of another env. A
// handle used after its handle scope has closed is undefined behavior (see
// unofficial_napi_realm): it may fail, or enter a newer realm that got the
// same handle.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_enter_realm(
    napi_env env,
    unofficial_napi_realm realm_or_null,
    unofficial_napi_realm_scope* scope_out);

// Closes `scope`, which must be the innermost scope opened in the running
// native callback (or outside any callback). A pending exception stays
// pending. Any other token fails with napi_invalid_arg and changes nothing:
// one already closed, one opened by an outer callback, or one with a scope
// nested inside it still open.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_leave_realm(
    napi_env env,
    unofficial_napi_realm_scope scope);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_collect_garbage(
    napi_env env);

// The levels of unofficial_napi_memory_pressure (V8's MemoryPressureLevel).
typedef enum unofficial_napi_memory_pressure_level {
  unofficial_napi_memory_pressure_none = 0,
  unofficial_napi_memory_pressure_moderate = 1,
  unofficial_napi_memory_pressure_critical = 2,
} unofficial_napi_memory_pressure_level;

// Unofficial helper. Tells the engine the embedder is short of memory. Call
// it on the env's thread, between scripts. `level` is an
// unofficial_napi_memory_pressure_level, passed as int32_t so that a value
// outside it can be refused: napi_invalid_arg.
//
// V8: Isolate::MemoryPressureNotification. Moderate and critical abort
// in-flight concurrent optimization jobs. Critical runs a memory-reducing
// full collection now; when much is left, it runs a second one, or starts
// incremental marking if the first was slow. Moderate starts memory-reducing
// incremental marking, which the platform's tasks finish. The level returns
// to none once handled. Before V8 13.7, neither level empties the
// compilation cache, which unofficial_napi_collect_garbage
// (LowMemoryNotification) does; V8 13.7 and later empty it here too. Each
// full collection ages compiled code as any full collection does, so a
// script that does not run again is still flushed after several calls.
//
// QuickJS: moderate and critical run a full cycle collection.
//
// What a collection here frees is finalized as after any other collection.
// V8 runs no JavaScript and no finalizer in the call: it queues the
// finalizers for the env's thread, where a task it posts through the enqueue
// hook (in the call), or else the next unofficial_napi_event_loop_checkpoint,
// runs them. QuickJS runs the finalizers of the values it frees in the call,
// in the env's realm, as unofficial_napi_collect_garbage does. While the env
// terminates, they still run there (V8's in the posted task, QuickJS's in
// the call), and a call one makes into JavaScript is refused, as any is
// then. V8's checkpoint fails then and runs none of them, so with no
// enqueue hook V8 runs them at the first checkpoint after
// unofficial_napi_cancel_terminate_execution. Called while the env is
// released (by a hook or finalizer that
// unofficial_napi_release_env runs), both engines leave the finalizers of
// external ArrayBuffers and Buffers to the release's pass, in its order, as
// for any collection then. The call frees no record of a collected realm, so
// it runs no context-token callback (deprecated, V8 only). On V8 that differs
// from unofficial_napi_collect_garbage, which frees them: a realm the call
// collects keeps its token until the next unofficial_napi_create_realm or
// unofficial_napi_collect_garbage, or the env's release. QuickJS frees them
// in the next unofficial_napi_create_realm after either call.
//
// Host-JavaScript providers: napi_generic_failure for every valid level.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_memory_pressure(
    napi_env env,
    int32_t level);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_set_prepare_stack_trace_callback(
    napi_env env,
    napi_value callback);

typedef enum unofficial_napi_event_loop_checkpoint_mode {
  // Drain promise/microtask work without suspending for a host task turn. A
  // synchronous provider still reports a provider task it ran (see
  // unofficial_napi_event_loop_checkpoint_state_host_tasks_admitted).
  unofficial_napi_event_loop_checkpoint_microtasks = 0,
  // Admit a host task turn as well as draining engine work. Host-JavaScript
  // providers suspend through JSPI. Synchronous providers report that no host
  // task was admitted, unless the checkpoint ran a provider task (see
  // unofficial_napi_event_loop_checkpoint_state_host_tasks_admitted), so the
  // runtime can wait on its native event source.
  unofficial_napi_event_loop_checkpoint_host_tasks = 1,
} unofficial_napi_event_loop_checkpoint_mode;

typedef enum unofficial_napi_event_loop_checkpoint_state {
  unofficial_napi_event_loop_checkpoint_state_none = 0,
  // The provider still owns work which can make JavaScript runnable.
  unofficial_napi_event_loop_checkpoint_state_pending_provider_work = 1 << 0,
  // The checkpoint admitted a host task turn, so the runtime starts its next
  // loop turn without waiting on its native event source.
  //
  // - Host-JavaScript providers set it after suspending through JSPI.
  // - Synchronous providers set it, together with
  //   unofficial_napi_event_loop_checkpoint_state_pending_provider_work, when
  //   the checkpoint ran a provider task (for example a V8 foreground task),
  //   in either mode. They run at most one such task per checkpoint, because
  //   a task may queue its successor, so the event loop gets a turn between
  //   them. A checkpoint that runs no task leaves the bit unset.
  //
  // When this bit is absent, the runtime remains responsible for waiting on
  // its native event source.
  unofficial_napi_event_loop_checkpoint_state_host_tasks_admitted = 1 << 1,
} unofficial_napi_event_loop_checkpoint_state;

// Complete one provider-owned event-loop checkpoint. The mode describes Node
// semantics, while the provider owns how those semantics are implemented. The
// returned state distinguishes an asynchronous host turn from a synchronous
// engine checkpoint without requiring a separate provider-kind query. The
// checkpoint may be called with no handle scope open; it leaves no values in
// the caller's scope.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_event_loop_checkpoint(
    napi_env env,
    unofficial_napi_event_loop_checkpoint_mode mode,
    bool has_runnable_work,
    uint32_t* state_out);
// Acquires an exact byte range for native access. The returned pointer remains
// valid until release, including across asynchronous native work. Readable
// ranges are copied from JavaScript on acquire; writable ranges are published
// back to JavaScript on release. This avoids treating every raw N-API pointer
// as a dirty copy of the value's entire backing store.
typedef enum unofficial_napi_buffer_access_mode {
  unofficial_napi_buffer_access_read = 1,
  unofficial_napi_buffer_access_write = 2,
  unofficial_napi_buffer_access_readwrite = 3,
} unofficial_napi_buffer_access_mode;
// Opaque ownership token for an exact native byte range. The token retains the
// JavaScript value and any provider-owned snapshot until release; callers must
// not derive the token from the returned data pointer or inspect its contents.
typedef struct unofficial_napi_buffer_lease__* unofficial_napi_buffer_lease;
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_acquire_buffer_lease(
    napi_env env,
    napi_value value,
    size_t byte_offset,
    size_t byte_length,
    unofficial_napi_buffer_access_mode mode,
    unofficial_napi_buffer_lease* lease,
    void** data);
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_release_buffer_lease(
    napi_env env, unofficial_napi_buffer_lease lease, bool modified);
// Creates a TypedArray whose backing store is guest WebAssembly memory. This
// is for native/JavaScript control blocks that require true shared visibility;
// bulk host-owned data should use scoped buffer access instead.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_create_guest_backed_typedarray(
    napi_env env,
    napi_typedarray_type type,
    size_t length,
    void** data,
    napi_value* result);
// Creates an ArrayBuffer using the provider's native ownership policy without
// exposing a raw pointer to Edge. Embedded providers may adopt an uninitialized
// native allocation; host-JavaScript providers allocate in the host engine and
// therefore may return zeroed storage when the engine has no unsafe allocator.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_create_uninitialized_arraybuffer(
    napi_env env,
    size_t length,
    bool zero_fill,
    napi_value* result);

// Unofficial helper. Terminates current JS execution in the env's engine.
// This is used for worker-style shutdown semantics where the process must
// survive but the current env must stop executing JS immediately. The JS stack
// unwinds without running catch or finally blocks, and calls into JS fail with
// napi_pending_exception until the termination is cancelled. Any thread may
// call this.
//
// A call the termination stops or refuses leaves it as the pending exception
// unless another one is pending. napi_get_and_clear_last_exception returns
// null for a termination on both providers; QuickJS keeps its uncatchable
// engine marker internal. Cancelling also makes stored QuickJS module
// evaluation errors catchable.
// When a native callback returns while the env terminates, what it left
// pending is dropped and the JS that called it unwinds. V8 acts on a
// termination requested from another thread at its next stack check, and on
// one an interrupt callback's JS runs into at the stack check after the one
// that ran the callback. On V8, a builtin called directly from native code
// (for example forEach or a promise reaction job), with no user JS below its
// native callback, may finish before another stack check. Such a call can
// return napi_ok after requesting termination. QuickJS stops it immediately.
//
// These unofficial termination helpers deliberately return
// napi_pending_exception for a refused call for every module_api_version and
// leave a pending termination marker. Node's shutdown preamble instead uses
// napi_cannot_run_js for modules targeting N-API >= 10 and leaves no exception.
// V8 permits error creation without a code while terminating; with a code it
// refuses because an inherited setter may run. QuickJS refuses both cases
// because constructing an error can read Error.prepareStackTrace.
//
// Known QuickJS gap: QuickJS-ng reads Error.prepareStackTrace, which may be a
// getter, whenever it gives an error its stack, also while a termination
// unwinds, so such a getter runs as JS then. It also drops whatever that
// getter throws: a termination the getter runs into does not stop the code
// after it (a catch block included) until the next call into JS, which
// fails. Closing that needs a QuickJS-ng change.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_terminate_execution(napi_env env);
// Clears a previously requested engine termination on the current env. This is
// used when embedder code intentionally stops a worker but still needs the
// current JS stack to unwind normally. Call it on the env's engine thread. A
// pending exception that stands for the termination is dropped: nothing threw
// it, so a native callback that cancels returns to its caller as after any
// call. Any other pending exception stays. V8: the checkpoints the termination
// refused left the engine's tasks queued, so when some wait for a checkpoint,
// this calls the host's foreground task waker (wasmer_napi's
// set_foreground_task_waker) again. QuickJS preserves promise jobs remaining
// after an interrupted checkpoint and runs them at a checkpoint after cancel;
// V8 drops the remaining microtasks when a running microtask is terminated.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_cancel_terminate_execution(napi_env env);

// Provider intrinsic trusted source. V8 evaluates it through its native code
// generation hook: direct eval retains lexical scope and never calls author
// toString/@@toPrimitive. Its private brand survives prototype changes, cannot
// be copied, inherited or recreated (unofficial_napi_create_private_symbol
// cannot name it), and does not bypass a context's codegen restriction.
// Creation acts in the current N-API realm: both the object and its native
// default toString belong to that realm. No explicit realm argument is needed.
// Eval uses its caller's lexical scope and the receiving realm's codegen
// restriction, even when the trusted source was made in another realm.
// Function(trusted) uses ordinary string coercion; only eval's source lookup
// bypasses author toString/@@toPrimitive. The default native toString enables
// Function(trusted) but is not a hardened Function-constructor integration.
// Source must be a primitive string. While terminating, V8 refuses creation
// with napi_pending_exception. Creation and the query below remain usable
// during cleanup hooks while the env and its agent are still live.
// QuickJS and host-JS are unsupported: they return napi_generic_failure and
// zero the output. QuickJS support needs a native direct-eval engine hook.
// The hook is isolate-wide: the first call on an isolate installs this
// provider's ModifyCodeGenerationFromStrings callback, replacing any callback
// the embedder installed. V8 cannot report the previous callback, so it is not
// chained. A callback the embedder installs afterwards is kept, and trusted
// scripts then follow that callback instead. For other values the hook keeps
// V8's behavior: eval returns non-string objects unchanged, and strings follow
// each context's code generation setting.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_create_trusted_script(
    napi_env env, napi_value source, napi_value* result);
// Owning VM thread only. Reports true only while the VM has a JavaScript frame
// on its stack (in any context, including other security origins) or is
// running a microtask checkpoint (V8) or job (QuickJS). A native function called
// from C through napi_call_function, directly or through a builtin, with no JavaScript frame
// below it reports false. Does not execute JS, take provider locks, or replace
// an existing pending exception, so it may be called from any provider
// callback, including during termination. It covers the agent's realms;
// entering a realm alone does not count as executing JavaScript. Unsupported
// engines return napi_generic_failure with *result set to false. V8 and
// QuickJS support this query; host JS does not expose native execution state.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_is_executing_js(
    napi_env env, bool* result);

typedef void (*unofficial_napi_interrupt_callback)(napi_env env, void* data);

// Unofficial helper. Requests execution of a callback on the target env's
// engine thread at the next interrupt point. The callback runs entered into
// that env's isolate/context.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_request_interrupt(
    napi_env env,
    unofficial_napi_interrupt_callback callback,
    void* data);

// Unofficial helper. Enqueues a JS function into V8 microtask queue.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_enqueue_microtask(napi_env env, napi_value callback);

// Unofficial helper. Sets the per-env PromiseReject callback used by
// internal/process/promises via internalBinding('task_queue').
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_set_promise_reject_callback(napi_env env,
                                                                    napi_value callback);

// Unofficial helper. Sets the per-env Promise lifecycle hooks used by
// internal/promise_hooks via internalBinding('async_wrap').
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_set_promise_hooks(napi_env env,
                                                          napi_value init,
                                                          napi_value before,
                                                          napi_value after,
                                                          napi_value resolve);

typedef size_t (*unofficial_napi_near_heap_limit_callback)(
    napi_env env,
    void* data,
    size_t current_heap_limit,
    size_t initial_heap_limit);

// Atomically configures the environment's one near-heap-limit callback slot.
// A non-null callback installs or replaces the slot and requires
// `restored_heap_limit` to be zero. A null callback removes the slot, requires
// `data` to be null, and restores the provider heap limit to the supplied
// value.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_configure_near_heap_limit_callback(
    napi_env env,
    unofficial_napi_near_heap_limit_callback callback_or_null,
    void* data,
    size_t restored_heap_limit);
// Unofficial helpers used by util/options parity work in edge.
// These expose engine-specific data that is not available in the public N-API.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_get_promise_details(napi_env env,
                                                            napi_value promise,
                                                            int32_t* state_out,
                                                            napi_value* result_out,
                                                            bool* has_result_out);

typedef enum {
  unofficial_napi_error_metadata_current = 0,
  unofficial_napi_error_metadata_take_preserved = 1,
  unofficial_napi_error_metadata_positions_only = 2,
  // Return only the provider stack's "Thrown at" text. Unlike `current`, this
  // mode must not request source-map formatting or invoke embedder JavaScript.
  unofficial_napi_error_metadata_thrown_at_only = 3,
} unofficial_napi_error_metadata_mode;

typedef struct {
  napi_value source_line;
  napi_value script_resource_name;
  napi_value stderr_line;
  napi_value thrown_at;
  int32_t line_number;
  int32_t start_column;
  int32_t end_column;
} unofficial_napi_error_metadata;

// Unofficial helpers for Node-style exception/message parity.
// These expose engine message/source metadata that is not available in the
// public Node-API.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_get_error_metadata(
    napi_env env,
    napi_value error,
    unofficial_napi_error_metadata_mode mode,
    unofficial_napi_error_metadata* out);

// Preserve the current engine-generated source arrow/message for an Error
// object so later rethrows do not overwrite it with the rethrow callsite.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_preserve_error_source_message(
    napi_env env,
    napi_value error);
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_configure_source_maps(
    napi_env env,
    bool enabled,
    napi_value callback);

// Unofficial helper used by module_wrap parity paths to tell the runtime's
// PromiseReject callback machinery that a rejected promise is being handled
// synchronously, matching Node's native ThrowIfPromiseRejected() helper.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_mark_promise_as_handled(
    napi_env env,
    napi_value promise);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_get_proxy_details(napi_env env,
                                                          napi_value proxy,
                                                          napi_value* target_out,
                                                          napi_value* handler_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_preview_entries(napi_env env,
                                                        napi_value value,
                                                        napi_value* entries_out,
                                                        bool* is_key_value_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_get_call_sites(napi_env env,
                                                       uint32_t frames,
                                                       napi_value* callsites_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_arraybuffer_view_has_buffer(napi_env env,
                                                                    napi_value value,
                                                                    bool* result_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_get_constructor_name(napi_env env,
                                                             napi_value value,
                                                             napi_value* name_out);

// Unofficial helper for Node's internalBinding('util').privateSymbols.
// Returns a JS-visible private symbol value backed by the engine's hidden
// private property machinery.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_create_private_symbol(napi_env env,
                                                              const char* utf8description,
                                                              size_t length,
                                                              napi_value* result_out);

// Unofficial helper for internalBinding('messaging').structuredClone().
// This mirrors the engine's structured clone path closely enough to preserve
// SharedArrayBuffer backing stores during clone/deserialization.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_structured_clone(
    napi_env env,
    napi_value value,
    napi_value transfer_list_or_null,
    napi_value* result_out);

// Opaque provider-owned message which may cross N-API environments. A message
// is consumed by message_take on both success and failure, or explicitly
// destroyed with message_drop while it is still queued.
typedef struct unofficial_napi_message__* unofficial_napi_message;

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_message_create(
    napi_env env,
    napi_value value,
    unofficial_napi_message* message_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_message_take(
    napi_env env,
    unofficial_napi_message message,
    napi_value* result_out);

NAPI_EXTENSION_WASMER_EXTERN void unofficial_napi_message_drop(unofficial_napi_message message);

// Returns own property keys while filtering indexed elements inside the
// provider. This bulk operation avoids materializing every array index and
// crossing the guest/host boundary once per key.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_get_own_non_index_properties(
    napi_env env,
    napi_value value,
    uint32_t filter_bits,
    napi_value* result_out);

// Unofficial helper for Node's internalBinding('v8').getHashSeed().
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_get_hash_seed(napi_env env,
                                                      uint64_t* hash_seed_out);

#define UNOFFICIAL_NAPI_HEAP_SPACE_NAME_MAX_LENGTH 64

typedef enum {
  unofficial_napi_heap_stat_total_heap_size = 1u << 0,
  unofficial_napi_heap_stat_total_heap_size_executable = 1u << 1,
  unofficial_napi_heap_stat_total_physical_size = 1u << 2,
  unofficial_napi_heap_stat_total_available_size = 1u << 3,
  unofficial_napi_heap_stat_used_heap_size = 1u << 4,
  unofficial_napi_heap_stat_heap_size_limit = 1u << 5,
  unofficial_napi_heap_stat_does_zap_garbage = 1u << 6,
  unofficial_napi_heap_stat_malloced_memory = 1u << 7,
  unofficial_napi_heap_stat_peak_malloced_memory = 1u << 8,
  unofficial_napi_heap_stat_number_of_native_contexts = 1u << 9,
  unofficial_napi_heap_stat_number_of_detached_contexts = 1u << 10,
  unofficial_napi_heap_stat_total_global_handles_size = 1u << 11,
  unofficial_napi_heap_stat_used_global_handles_size = 1u << 12,
  unofficial_napi_heap_stat_external_memory = 1u << 13,
  unofficial_napi_heap_stat_array_buffer_memory = 1u << 14,
  unofficial_napi_heap_stat_all = (1u << 15) - 1,
} unofficial_napi_heap_stat_field;

enum { UNOFFICIAL_NAPI_HEAP_STATISTICS_VERSION = 1 };

typedef struct {
  uint32_t size;
  uint32_t version;
  uint64_t valid_fields;
  uint64_t total_heap_size;
  uint64_t total_heap_size_executable;
  uint64_t total_physical_size;
  uint64_t total_available_size;
  uint64_t used_heap_size;
  uint64_t heap_size_limit;
  uint64_t does_zap_garbage;
  uint64_t malloced_memory;
  uint64_t peak_malloced_memory;
  uint64_t number_of_native_contexts;
  uint64_t number_of_detached_contexts;
  uint64_t total_global_handles_size;
  uint64_t used_global_handles_size;
  uint64_t external_memory;
  uint64_t array_buffer_memory;
} unofficial_napi_heap_statistics;

static inline void unofficial_napi_heap_statistics_init(
    unofficial_napi_heap_statistics* stats) {
  if (stats == NULL) return;
#ifdef __cplusplus
  *stats = {};
#else
  *stats = (unofficial_napi_heap_statistics){0};
#endif
  stats->size = sizeof(*stats);
  stats->version = UNOFFICIAL_NAPI_HEAP_STATISTICS_VERSION;
}

// Providers identify measured fields through `valid_fields`; unsupported
// fields have no provider-neutral meaning. Consumers that expose a dense
// Node-compatible object should normalize once at the boundary instead of
// accidentally treating provider scratch values as observations.
static inline void unofficial_napi_heap_statistics_normalize(
    unofficial_napi_heap_statistics* stats) {
  if (stats == NULL) return;
#define UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(bit, field) \
  do {                                                   \
    if ((stats->valid_fields & (bit)) == 0) {           \
      stats->field = 0;                                 \
    }                                                    \
  } while (0)
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_total_heap_size, total_heap_size);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_total_heap_size_executable,
      total_heap_size_executable);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_total_physical_size, total_physical_size);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_total_available_size, total_available_size);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_used_heap_size, used_heap_size);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_heap_size_limit, heap_size_limit);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_does_zap_garbage, does_zap_garbage);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_malloced_memory, malloced_memory);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_peak_malloced_memory, peak_malloced_memory);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_number_of_native_contexts,
      number_of_native_contexts);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_number_of_detached_contexts,
      number_of_detached_contexts);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_total_global_handles_size,
      total_global_handles_size);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_used_global_handles_size,
      used_global_handles_size);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_external_memory, external_memory);
  UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD(
      unofficial_napi_heap_stat_array_buffer_memory, array_buffer_memory);
  stats->valid_fields &= unofficial_napi_heap_stat_all;
#undef UNOFFICIAL_NAPI_NORMALIZE_HEAP_FIELD
}

typedef struct {
  char space_name[UNOFFICIAL_NAPI_HEAP_SPACE_NAME_MAX_LENGTH];
  uint64_t space_size;
  uint64_t space_used_size;
  uint64_t space_available_size;
  uint64_t physical_space_size;
} unofficial_napi_heap_space_statistics;

typedef struct {
  uint64_t code_and_metadata_size;
  uint64_t bytecode_and_metadata_size;
  uint64_t external_script_source_size;
  uint64_t cpu_profiler_metadata_size;
} unofficial_napi_heap_code_statistics;

typedef struct unofficial_napi_profile__* unofficial_napi_profile;

typedef enum {
  unofficial_napi_profile_cpu = 1,
  unofficial_napi_profile_heap = 2,
} unofficial_napi_profile_kind;

typedef enum {
  unofficial_napi_profile_start_ok = 0,
  unofficial_napi_profile_start_busy = 1,
} unofficial_napi_profile_start_result;

typedef struct {
  bool expose_internals;
  bool expose_numeric_values;
} unofficial_napi_heap_snapshot_options;

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_get_heap_statistics(
    napi_env env,
    unofficial_napi_heap_statistics* stats_out);

// Takes one provider snapshot, writes at most `capacity` entries, and reports
// the snapshot's full entry count through `count_out`. A null `stats_out` is
// valid only when `capacity` is zero, allowing callers to query capacity.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_get_heap_space_statistics(
    napi_env env,
    unofficial_napi_heap_space_statistics* stats_out,
    uint32_t capacity,
    uint32_t* count_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_get_heap_code_statistics(
    napi_env env,
    unofficial_napi_heap_code_statistics* stats_out);

// Unofficial helpers for worker-thread profiling/snapshot support. These must
// be called on the target env's engine thread, typically from
// unofficial_napi_request_interrupt(). A successful start returns one opaque,
// env-owned session. Stop leaves the session active when it returns
// napi_cannot_run_js, so the caller can retry from the target engine thread.
// Once stop accepts the thread/env preconditions it consumes the session even
// if producing the final result fails. Diagnostic JSON is returned as UTF-8
// bytes in a Uint8Array whose backing store is outside the inspected heap.
// Sessions still active at env teardown are stopped and released by the
// provider.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_profile_start(
    napi_env env,
    unofficial_napi_profile_kind kind,
    unofficial_napi_profile_start_result* result_out,
    unofficial_napi_profile* profile_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_profile_stop(
    napi_env env,
    unofficial_napi_profile profile,
    napi_value* utf8_json_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_take_heap_snapshot(
    napi_env env,
    const unofficial_napi_heap_snapshot_options* options,
    napi_value* utf8_json_out);

// Unofficial helpers for Node's async_context_frame parity. These expose the
// engine continuation-preserved embedder data used by AsyncContextFrame.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_get_continuation_preserved_embedder_data(
    napi_env env,
    napi_value* result_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_set_continuation_preserved_embedder_data(
    napi_env env,
    napi_value value);

// Unofficial helper. Refreshes V8 date/timezone configuration after TZ changes.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_notify_datetime_configuration_change(napi_env env);

// Unofficial helper. Creates the native internalBinding('serdes') object
// containing Serializer and Deserializer constructors.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_create_serdes_binding(napi_env env,
                                                              napi_value* result_out);

// Script, function and module compilation. These are engine-specific APIs
// and are not part of the public Node-API. Each runs or compiles code in
// `realm_or_null`, or in the env's own realm when it is NULL. The deprecated
// forms (the contextify calls and version 1 of the option structs) work in
// the current realm instead, as they did before realms existed.

// Provider-owned compiled JavaScript, compiled for one realm. The handle is
// bound to its env and must be released explicitly.
typedef struct unofficial_napi_bytecode__* unofficial_napi_bytecode;

// A tagged JS source for compile/eval APIs. Providers reject descriptors whose
// selected field is null or whose unselected field is non-null.
typedef enum {
  unofficial_napi_js_source_text = 0,
  unofficial_napi_js_source_bytecode = 1,
} unofficial_napi_js_source_kind;

typedef struct unofficial_napi_js_source {
  int32_t kind;
  napi_value text;
  unofficial_napi_bytecode bytecode;
} unofficial_napi_js_source;

static inline bool unofficial_napi_js_source_is_valid(
    const unofficial_napi_js_source* source) {
  return source != NULL &&
         ((source->kind == unofficial_napi_js_source_text &&
           source->text != NULL && source->bytecode == NULL) ||
          (source->kind == unofficial_napi_js_source_bytecode &&
           source->text == NULL && source->bytecode != NULL));
}

static inline unofficial_napi_js_source unofficial_napi_js_source_from_text(
    napi_value text) {
  unofficial_napi_js_source source = {
      unofficial_napi_js_source_text, text, NULL};
  return source;
}

static inline unofficial_napi_js_source unofficial_napi_js_source_from_bytecode(
    unofficial_napi_bytecode bytecode) {
  unofficial_napi_js_source source = {
      unofficial_napi_js_source_bytecode, NULL, bytecode};
  return source;
}

// Compile shape of a bytecode artifact; bytecode is only usable by APIs that
// compile the same shape.
typedef enum {
  unofficial_napi_bytecode_shape_script = 0,        // whole-script eval
  unofficial_napi_bytecode_shape_cjs_function = 1,  // function compiled with params
  unofficial_napi_bytecode_shape_module = 2,        // ES module
} unofficial_napi_bytecode_shape;

// Versioned input for opening a compiled artifact. When `has_cache` is nonzero,
// the provider first validates the supplied bytes against every compile input.
// `cache_policy` determines whether a rejected cache is atomically replaced
// by compiling source or reported without the discarded fallback compile.
typedef enum {
  unofficial_napi_bytecode_cache_compile_on_reject = 0,
  unofficial_napi_bytecode_cache_validate_only = 1,
} unofficial_napi_bytecode_cache_policy;

// Version 2 appended `realm_or_null`, the realm the code is compiled for (NULL:
// the env's own). Version 1, which ends before it (`size` at least
// UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_V1_SIZE), is deprecated but still
// accepted: it compiles for the current realm, as
// unofficial_napi_contextify_run_script finds it for a NULL `sandbox_or_null`
// (see its QuickJS note).
//
// Zero-initialize the struct (`= {0}` in C, `{}` in C++) before setting its
// fields. Source written before version 2 that sets `size` to sizeof,
// `version` to UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION and only the
// fields it knew sends version 2 with an indeterminate `realm_or_null`.
#define UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION 2u
typedef struct unofficial_napi_bytecode_open_options {
  uint32_t size;
  uint32_t version;
  napi_value source_text;
  napi_value filename;
  int32_t shape;
  napi_value params_or_undefined;
  napi_value host_defined_option_id;
  int32_t line_offset;
  int32_t column_offset;
  const uint8_t* cache_bytes;
  size_t cache_byte_length;
  uint8_t has_cache;
  uint8_t cache_policy;
  // Version 2.
  unofficial_napi_realm realm_or_null;
} unofficial_napi_bytecode_open_options;

#define UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_V1_SIZE \
  offsetof(unofficial_napi_bytecode_open_options, realm_or_null)

typedef struct unofficial_napi_bytecode_open_result {
  unofficial_napi_bytecode bytecode;
  uint8_t cache_rejected;
  uint8_t can_parse_as_module;
} unofficial_napi_bytecode_open_result;

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_bytecode_open(
    napi_env env,
    const unofficial_napi_bytecode_open_options* options,
    unofficial_napi_bytecode_open_result* result);

// Engine bytes for persistence, as a Uint8Array (V8: raw CachedData;
// QuickJS: self-validating QJSB header [shape, source, params, filename,
// payload hashes] + JS_WriteObject output).
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_bytecode_serialize(
    napi_env env,
    unofficial_napi_bytecode bytecode,
    napi_value* buffer_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_bytecode_release(
    napi_env env,
    unofficial_napi_bytecode bytecode);

// Runs a script in a realm. When the realm has its own microtask queue
// (unofficial_napi_realm_options.own_microtask_queue), the queue is drained
// after the script returns.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_run_script(
    napi_env env,
    unofficial_napi_realm realm_or_null,
    const unofficial_napi_js_source* source,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    int64_t timeout,
    bool display_errors,
    bool break_on_sigint,
    bool break_on_first_line,
    napi_value host_defined_option_id,
    napi_value* result_out);

// Compiles a function in a realm, as vm.compileFunction does.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_compile_function(
    napi_env env,
    unofficial_napi_realm realm_or_null,
    const unofficial_napi_js_source* source,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    napi_value context_extensions_or_undefined,
    napi_value params_or_undefined,
    napi_value host_defined_option_id,
    napi_value* result_out);

// Deprecated: the contextify calls, which take contextified objects instead of
// realms. Use unofficial_napi_create_realm, unofficial_napi_run_script and
// unofficial_napi_compile_function. They are kept for compatibility and
// removed in a future major release. Each is a thin wrapper over the realm
// API, so the objects and realms of both mix freely.

// Deprecated: use unofficial_napi_create_realm. An object or function sandbox
// becomes the sandbox of a new realm; a symbol (vm.constants.DONT_CONTEXTIFY)
// makes a realm with a fresh global. The other arguments become the realm
// options of the same names (`origin_or_undefined` is `origin`). `result_out`
// receives the realm's global as create_realm reports it: the sandbox, or the
// fresh global. Fails with napi_invalid_arg when `name` is NULL, for any other
// sandbox, and for a sandbox that already belongs to a realm.
UNOFFICIAL_NAPI_DEPRECATED("use unofficial_napi_create_realm")
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_contextify_make_context(
    napi_env env,
    napi_value sandbox_or_symbol,
    napi_value name,
    napi_value origin_or_undefined,
    bool allow_code_gen_strings,
    bool allow_code_gen_wasm,
    bool own_microtask_queue,
    napi_value host_defined_option_id,
    napi_value* result_out);

// Deprecated: use unofficial_napi_run_script. The script runs in the realm
// `sandbox_or_null` names:
// - A contextified object (a realm's global or sandbox): its realm. The env's
//   own global is one when it is a realm's sandbox (vm.createContext with
//   globalThis), and then names that realm, as on main's V8.
// - The env's own global otherwise: the env's realm.
// - NULL, undefined or null: the current realm. Unlike
//   unofficial_napi_run_script with a NULL realm, that is the realm entered or
//   of the running callback, which is the env's own unless the embedder
//   enters realms. While a realm whose sandbox is the env's own global is
//   current, QuickJS takes the env's realm instead: napi_get_global returns
//   that sandbox there, and the env's own global names the env's realm.
// Any other value fails with napi_invalid_arg.
UNOFFICIAL_NAPI_DEPRECATED("use unofficial_napi_run_script")
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_contextify_run_script(
    napi_env env,
    napi_value sandbox_or_null,
    const unofficial_napi_js_source* source,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    int64_t timeout,
    bool display_errors,
    bool break_on_sigint,
    bool break_on_first_line,
    napi_value host_defined_option_id,
    napi_value* result_out);

// Deprecated: use unofficial_napi_compile_function. The function is compiled
// in the realm `parsing_context_or_undefined` names, which resolves as
// unofficial_napi_contextify_run_script's `sandbox_or_null` does.
UNOFFICIAL_NAPI_DEPRECATED("use unofficial_napi_compile_function")
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_contextify_compile_function(
    napi_env env,
    const unofficial_napi_js_source* source,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    napi_value parsing_context_or_undefined,
    napi_value context_extensions_or_undefined,
    napi_value params_or_undefined,
    napi_value host_defined_option_id,
    napi_value* result_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_contextify_contains_module_syntax(
    napi_env env,
    napi_value code,
    napi_value filename,
    napi_value resource_name_or_undefined,
    bool cjs_var_in_scope,
    bool* result_out);

// Unofficial helpers for implementing internalBinding('module_wrap') on embedders.
// These keep V8 module objects behind an opaque native handle so bindings stay N-API only.
typedef struct unofficial_napi_module__* unofficial_napi_module;

// Versioned, tagged transaction for creating either kind of module. Keeping
// creation behind one descriptor gives providers one validation boundary and
// lets the ABI grow without adding one import per module kind.
//
// Version 2 appended `realm_or_null`, the realm the module lives in. Version
// 1, which ends before it (`size` at least
// UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_V1_SIZE), is deprecated but still
// accepted. The realm is:
// - version 2 with `realm_or_null` set: that realm. `context_or_undefined`
//   must then be NULL, undefined or null, else napi_invalid_arg.
// - otherwise, from the deprecated `context_or_undefined`, as
//   unofficial_napi_contextify_run_script's `sandbox_or_null`: the realm of a
//   contextified object (a realm's global or sandbox, which the env's own
//   global may be), the env's realm for the env's own global otherwise, and
//   for NULL, undefined or null the env's realm (version 2) or the current
//   realm as that call finds it, QuickJS note included (version 1). Any
//   other value is napi_invalid_arg.
//
// Zero-initialize the struct (`= {0}` in C, `{}` in C++) before setting its
// fields. Source written before version 2 that sets `size` to sizeof,
// `version` to UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION and only the
// fields it knew sends version 2 with an indeterminate `realm_or_null`.
#define UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION 2u

typedef enum {
  unofficial_napi_module_source_text = 1,
  unofficial_napi_module_synthetic = 2,
} unofficial_napi_module_kind;

typedef struct {
  const unofficial_napi_js_source* source;
  int32_t line_offset;
  int32_t column_offset;
  napi_value host_defined_option_id;
} unofficial_napi_source_text_module_options;

typedef struct {
  napi_value export_names;
  napi_value synthetic_evaluation_steps;
} unofficial_napi_synthetic_module_options;

UNOFFICIAL_NAPI_SUPPRESS_DEPRECATED_BEGIN
typedef struct {
  size_t size;
  uint32_t version;
  unofficial_napi_module_kind kind;
  napi_value wrapper;
  napi_value url;
  // Deprecated: use `realm_or_null`, with unofficial_napi_get_realm on the
  // contextified object. Kept for compatibility and removed in a future major
  // release.
  napi_value context_or_undefined
      UNOFFICIAL_NAPI_DEPRECATED_MEMBER("use realm_or_null");
  union {
    unofficial_napi_source_text_module_options source_text;
    unofficial_napi_synthetic_module_options synthetic;
  } payload;
  // Version 2.
  unofficial_napi_realm realm_or_null;
} unofficial_napi_module_create_options;
UNOFFICIAL_NAPI_SUPPRESS_DEPRECATED_END

#define UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_V1_SIZE \
  offsetof(unofficial_napi_module_create_options, realm_or_null)

// Immutable metadata produced by the same transaction that creates a module.
// The layout is selected by options.version, so adding fields requires a new
// create-options version rather than another query function.
typedef struct {
  unofficial_napi_module module;
  napi_value module_requests;
  bool has_top_level_await;
} unofficial_napi_module_create_result;

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_create(
    napi_env env,
    const unofficial_napi_module_create_options* options,
    unofficial_napi_module_create_result* result_out);

// Records retain their module and realm until destroy or env release. Their
// wrapper is weak: an embedder may destroy the record from its wrapper's
// finalizer. Linking retains dependency wrappers through private JS edges,
// so unreachable wrapper cycles can be collected. A native handle alone does
// not retain its wrapper; embedders must keep it alive while using the handle.
// Explicitly destroying a dependency invalidates its incoming native links;
// V8 refuses later instantiation through them, while QuickJS can continue
// with its retained engine module. Never use a destroyed module handle.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_destroy(
    napi_env env,
    unofficial_napi_module module);

// Only uninstantiated modules may be linked. All dependencies must belong
// to the same realm, else ERR_VM_MODULE_DIFFERENT_CONTEXT is pending.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_link(
    napi_env env,
    unofficial_napi_module module,
    size_t count,
    const unofficial_napi_module* linked_modules);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_instantiate(
    napi_env env,
    unofficial_napi_module module);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_evaluate(
    napi_env env,
    unofficial_napi_module module,
    int64_t timeout,
    bool break_on_sigint,
    napi_value* result_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_evaluate_sync(
    napi_env env,
    unofficial_napi_module module,
    napi_value filename,
    napi_value parent_filename,
    napi_value* result_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_get_namespace(
    napi_env env,
    unofficial_napi_module module,
    napi_value* result_out);

// Returns the requested fields from one observation of the provider-owned
// module record. At least one output must be non-null. Expensive fields are
// computed only when requested; has_async_graph is false before instantiation.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_get_state(
    napi_env env,
    unofficial_napi_module module,
    int32_t* status_out,
    napi_value* error_out,
    bool* has_async_graph_out);

// QuickJS warnings identify the module URL; unlike V8, they do not include
// the source line and column of the stalled await.
NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_check_unsettled_top_level_await(
    napi_env env,
    unofficial_napi_module module,
    bool warnings,
    bool* settled_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_set_export(
    napi_env env,
    unofficial_napi_module module,
    napi_value export_name,
    napi_value export_value);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_set_module_source_object(
    napi_env env,
    unofficial_napi_module module,
    napi_value source_object);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_get_module_source_object(
    napi_env env,
    unofficial_napi_module module,
    napi_value* result_out);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_create_cached_data(
    napi_env env,
    unofficial_napi_module module,
    napi_value* result_out);

#define UNOFFICIAL_NAPI_MODULE_HOOKS_VERSION 1u

typedef struct {
  size_t size;
  uint32_t version;
  napi_value import_module_dynamically;
  napi_value initialize_import_meta_object;
} unofficial_napi_module_hooks;

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_set_hooks(
    napi_env env,
    const unofficial_napi_module_hooks* hooks);

NAPI_EXTENSION_WASMER_EXTERN napi_status unofficial_napi_module_wrap_create_required_module_facade(
    napi_env env,
    unofficial_napi_module module,
    napi_value* result_out);

#include "unofficial_napi_wasm32_abi.h"

#if defined(__wasm32__)
#if defined(__cplusplus)
static_assert(sizeof(unofficial_napi_env_create_options) ==
              sizeof(unofficial_napi_wasm32_env_create_options_v1));
static_assert(sizeof(unofficial_napi_runtime_options) ==
              sizeof(unofficial_napi_wasm32_runtime_options_v1));
static_assert(sizeof(unofficial_napi_env_hooks) ==
              sizeof(unofficial_napi_wasm32_env_hooks_v1));
static_assert(sizeof(unofficial_napi_js_source) ==
              sizeof(unofficial_napi_wasm32_js_source));
static_assert(sizeof(unofficial_napi_bytecode_open_options) ==
              sizeof(unofficial_napi_wasm32_bytecode_open_options_v2));
static_assert(sizeof(unofficial_napi_module_create_options) ==
              sizeof(unofficial_napi_wasm32_module_create_options_v2));
static_assert(sizeof(unofficial_napi_module_hooks) ==
              sizeof(unofficial_napi_wasm32_module_hooks_v1));
static_assert(sizeof(unofficial_napi_realm_options) ==
              sizeof(unofficial_napi_wasm32_realm_options_v1));
#else
_Static_assert(sizeof(unofficial_napi_env_create_options) ==
                   sizeof(unofficial_napi_wasm32_env_create_options_v1),
               "public env-create layout differs from its wasm32 wire ABI");
_Static_assert(sizeof(unofficial_napi_runtime_options) ==
                   sizeof(unofficial_napi_wasm32_runtime_options_v1),
               "public runtime-options layout differs from its wasm32 wire ABI");
_Static_assert(sizeof(unofficial_napi_env_hooks) ==
                   sizeof(unofficial_napi_wasm32_env_hooks_v1),
               "public env-hooks layout differs from its wasm32 wire ABI");
_Static_assert(sizeof(unofficial_napi_js_source) ==
                   sizeof(unofficial_napi_wasm32_js_source),
               "public JS-source layout differs from its wasm32 wire ABI");
_Static_assert(sizeof(unofficial_napi_bytecode_open_options) ==
                   sizeof(unofficial_napi_wasm32_bytecode_open_options_v2),
               "public bytecode-open layout differs from its wasm32 wire ABI");
_Static_assert(sizeof(unofficial_napi_module_create_options) ==
                   sizeof(unofficial_napi_wasm32_module_create_options_v2),
               "public module-create layout differs from its wasm32 wire ABI");
_Static_assert(sizeof(unofficial_napi_module_hooks) ==
                   sizeof(unofficial_napi_wasm32_module_hooks_v1),
               "public module-hooks layout differs from its wasm32 wire ABI");
_Static_assert(sizeof(unofficial_napi_realm_options) ==
                   sizeof(unofficial_napi_wasm32_realm_options_v1),
               "public realm-options layout differs from its wasm32 wire ABI");
#endif
#endif

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // UNOFFICIAL_NAPI_H_
