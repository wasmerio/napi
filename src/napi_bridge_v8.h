#ifndef NAPI_BRIDGE_V8_H_
#define NAPI_BRIDGE_V8_H_

#include "internal/napi_snapshot.h"
#include "unofficial_napi.h"

using BridgeCallbackDataHook = void (*)(napi_env, void*, void*);

void SetBridgeCallbackDataHooks(napi_env env, BridgeCallbackDataHook retain,
                                BridgeCallbackDataHook release, void* user);
uint64_t napi_v8_get_arraybuffer_backing_store_token(napi_env env, napi_value value);
uint64_t napi_v8_get_arraybuffer_view_backing_store_token(napi_env env, napi_value value);

// Private provider entry points used by the native Wasmer bridge.
napi_status napi_v8_create_external_arraybuffer_with_free(
    napi_env env, void* data, size_t byte_length, void (*free_cb)(void*, void*),
    void* free_hint, napi_value* result);
napi_status napi_v8_create_external_buffer_with_free(
    napi_env env, size_t length, void* data, void (*free_cb)(void*, void*),
    void* free_hint, napi_value* result);

extern "C" {
napi_status NapiV8DefineBindingPreflight(napi_env env, uint32_t kind);
napi_status NapiV8RealmPreflight(napi_env env, uint32_t mode);
napi_status NapiV8SnapshotCommitPreflight(napi_env env);
napi_status NapiV8SnapshotCommitWithManifest(napi_env, unofficial_napi_realm, napi_value,
    const void*, size_t, uint32_t*, const char*, size_t);
napi_status unofficial_napi_snapshot_static_table(napi_env, uint32_t, napi_callback*, void**,
                                                  uint32_t* role, uint32_t* count);
napi_status unofficial_napi_snapshot_set_static(napi_env, uint32_t, napi_callback, void*, uint32_t);
napi_status unofficial_napi_snapshot_loaded_manifest(napi_env, const char**, size_t*);
void NapiV8RejectSnapshotManifest(napi_env);
}

#endif  // NAPI_BRIDGE_V8_H_
