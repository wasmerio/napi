#ifndef NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_
#define NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_

#include <cstdint>
#include <v8.h>

#include "unofficial_napi.h"

bool NapiV8LookupForegroundTaskTarget(v8::Isolate* isolate,
                                      napi_env* env_out,
                                      unofficial_napi_enqueue_foreground_task_callback* callback_out);
bool NapiV8IsContextifyContext(napi_env env, v8::Local<v8::Context> context);
void NapiV8ApplyPromiseHooksToContext(napi_env env, v8::Local<v8::Context> context);
void NapiV8ApplyPromiseHooksToContextifyContexts(napi_env env);
void NapiV8TrackProviderPromise(v8::Isolate* isolate,
                                v8::Local<v8::Promise> promise);
bool NapiV8HasPendingProviderWork(napi_env env);

// Bridge-created envs enter their isolate only during physical foreground calls.
// Root handle/context scopes remain isolate-local and are released by their owner.
napi_status NapiV8DetachBridgeIsolateScope(unofficial_napi_env_owner owner);

// Physical, thread-local callback depth; never exchanged with coroutine TLS.
// Destruction only restores metadata and must not flush deferred cleanup.
extern "C" int snapi_v8_allocator_active() noexcept;
extern "C" uint64_t snapi_v8_allocator_enter() noexcept;
extern "C" void snapi_v8_allocator_leave(uint64_t previous) noexcept;
class NapiV8AllocatorCallbackScope {
 public:
  NapiV8AllocatorCallbackScope() : previous_(snapi_v8_allocator_enter()) {}
  ~NapiV8AllocatorCallbackScope() { snapi_v8_allocator_leave(previous_); }
  NapiV8AllocatorCallbackScope(const NapiV8AllocatorCallbackScope&) = delete;
  NapiV8AllocatorCallbackScope& operator=(const NapiV8AllocatorCallbackScope&) = delete;
 private:
  uint64_t previous_;
};

#endif  // NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_
