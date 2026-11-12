#ifndef NAPI_EXPERIMENTAL
#define NAPI_EXPERIMENTAL
#endif

#include "napi_bridge_v8.h"
#include "edge_v8_platform.h"
#include "internal/napi_v8_env.h"

void SetBridgeCallbackDataHooks(napi_env env, BridgeCallbackDataHook retain,
                                BridgeCallbackDataHook release, void* user) {
  env->callback_data_retain = retain;
  env->callback_data_release = release;
  env->callback_data_hooks_user = user;
}

static uint64_t BackingStoreToken(const std::shared_ptr<v8::BackingStore>& backing_store) {
  return backing_store == nullptr
             ? 0
             : static_cast<uint64_t>(reinterpret_cast<uintptr_t>(backing_store.get()));
}

uint64_t napi_v8_get_arraybuffer_backing_store_token(napi_env, napi_value value) {
  if (value == nullptr) return 0;

  v8::Local<v8::Value> raw = napi_v8_unwrap_value(value);
  if (raw.IsEmpty() || !raw->IsArrayBuffer()) return 0;

  return BackingStoreToken(raw.As<v8::ArrayBuffer>()->GetBackingStore());
}

uint64_t napi_v8_get_arraybuffer_view_backing_store_token(napi_env, napi_value value) {
  if (value == nullptr) return 0;

  v8::Local<v8::Value> raw = napi_v8_unwrap_value(value);
  if (raw.IsEmpty() || !raw->IsArrayBufferView()) return 0;

  return BackingStoreToken(raw.As<v8::ArrayBufferView>()->Buffer()->GetBackingStore());
}

// Host-only: set the size of the V8 worker pool before platform creation.
extern "C" int snapi_bridge_set_v8_worker_thread_count(uint32_t count) {
  return EdgeV8Platform::SetWorkerThreadCount(static_cast<int>(count)) ? 1 : 0;
}

// Host-only: notify the event loop when V8 queues foreground work. NULL clears it.
extern "C" void snapi_bridge_set_foreground_task_waker(void (*waker)()) {
  EdgeV8Platform::SetForegroundTaskWaker(waker);
}
