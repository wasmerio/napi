#ifndef NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_
#define NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_

#include <v8.h>

#include "unofficial_napi.h"

// Detach a freshly created owner from the current isolate stack. The WASM
// bridge enters its isolate separately for each call.
void NapiV8DetachEnvOwner(unofficial_napi_env_owner owner);

// The WASM bridge may keep an isolate entered on a thread between its calls
// (its resident isolate). An env the provider creates stays entered, so
// before the provider enters a new env's isolate it calls the hook the bridge
// installed here, which leaves the resident isolate unless a bridge call is
// on the thread's stack. Otherwise the resident isolate would be left entered
// below the new one, where it could not be disposed.
using NapiV8IsolateEntryHook = void (*)();
void NapiV8SetIsolateEntryHook(NapiV8IsolateEntryHook hook);

bool NapiV8LookupForegroundTaskTarget(v8::Isolate* isolate,
                                      napi_env* env_out,
                                      unofficial_napi_enqueue_foreground_task_callback* callback_out);
bool NapiV8IsContextifyContext(napi_env env, v8::Local<v8::Context> context);
// The AllowWasmCodeGenerationCallback of the isolates the provider creates:
// false in every context of a restricted agent, else a realm's
// allow_code_gen_wasm, and true in other contexts.
bool NapiV8AllowWasmCodeGeneration(v8::Local<v8::Context> context,
                                   v8::Local<v8::String> source);
void NapiV8ApplyPromiseHooksToContext(napi_env env, v8::Local<v8::Context> context);
void NapiV8ApplyPromiseHooksToContextifyContexts(napi_env env);
// Frees the records of collected realms, unassigning their context tokens.
void NapiV8ReleaseCollectedRealms(napi_env env);
// Unassigns the context token of every realm that still has one, also of
// realms the unassign hooks make.
void NapiV8UnassignRealmTokens(napi_env env);
// True while a realm of the env has a context token.
bool NapiV8HasRealmTokens(napi_env env);
void NapiV8TrackProviderPromise(v8::Isolate* isolate,
                                v8::Local<v8::Promise> promise);
bool NapiV8HasPendingProviderWork(napi_env env);

// Whether a SharedArrayBuffer backing store in `message` overlaps the guest
// memory range [begin, end). The bridge uses this to keep that memory mapped
// while the message is queued and to check the receiver's memory.
napi_status NapiV8MessageSharesRange(unofficial_napi_message message,
                                     uintptr_t begin,
                                     uintptr_t end,
                                     bool* result);

struct NapiV8HeapUsage {
  // Bytes committed to the spaces governed by V8's old-generation limit.
  size_t committed_old_generation;
  // The first young large object can be allocated without a limit check.
  size_t young_large_objects;
  // Live bytes in all spaces. V8 restores a limit of at least 1.25 times this.
  size_t used;
};

// If env is null during release, read the isolate entered on this thread.
napi_status NapiV8GetHeapUsage(napi_env env, NapiV8HeapUsage* result);

// Collect if young large objects and the old generation together exceed the
// limit. Call on the isolate's thread after running JavaScript. A zero limit
// uses V8's total heap limit, and *collected reports whether collection ran.
napi_status NapiV8CollectGarbageIfOverHeapLimit(napi_env env,
                                              size_t old_generation_limit,
                                              bool* collected);

// Clear the engine's termination state without notifying the embedder.
napi_status NapiV8ClearTermination(napi_env env);
// Notify the embedder after cancellation. The waker may call into the bridge.
void NapiV8WakeAfterTermination(napi_env env);

#endif  // NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_
