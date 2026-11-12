#ifndef NAPI_V8_INTERNAL_NAPI_SNAPSHOT_H_
#define NAPI_V8_INTERNAL_NAPI_SNAPSHOT_H_

#include "unofficial_napi.h"

#if !defined(__wasm__)
#if defined(__GNUC__)
#define NAPI_V8_SNAPSHOT_PRIVATE __attribute__((visibility("hidden")))
#else
#define NAPI_V8_SNAPSHOT_PRIVATE
#endif

// Wasmer validates one artifact per snapshot configuration. Each restore
// environment retains its own reference, independent of the host handle.
struct NapiV8SnapshotHandle;
extern "C" {
NAPI_V8_SNAPSHOT_PRIVATE napi_status NapiV8OpenSnapshot(
    const void* bytes, size_t length, const void* key, size_t key_length,
    NapiV8SnapshotHandle** artifact_out);
NAPI_V8_SNAPSHOT_PRIVATE void NapiV8CloseSnapshot(NapiV8SnapshotHandle* artifact);
NAPI_V8_SNAPSHOT_PRIVATE napi_status NapiV8CreateEnvWithSnapshot(
    int32_t module_api_version,
    const unofficial_napi_env_create_options* env_options,
    const unofficial_napi_snapshot_env_options* snapshot_options,
    NapiV8SnapshotHandle* retained_artifact,
    napi_env* env_out, unofficial_napi_env_owner* owner_out);
}

#undef NAPI_V8_SNAPSHOT_PRIVATE
#endif  // !defined(__wasm__)

#endif  // NAPI_V8_INTERNAL_NAPI_SNAPSHOT_H_
