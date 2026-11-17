#ifndef NAPI_V8_NODE_API_H_
#define NAPI_V8_NODE_API_H_

#include "js_native_api.h"
#include "node_api_types.h"

// The provider's implementations of node_api.h's Buffer, env cleanup hook and
// fatal calls. node_api_v8.cc defines the public symbols over them when
// NAPI_PROVIDE_NODE_API is on (deprecated: an embedder such as Edge.js
// defines them itself); napi's own code calls these names so it never lands
// in an embedder's definitions.
extern "C" {
void NAPI_CDECL napi_v8_fatal_error(const char* location,
                                    size_t location_len,
                                    const char* message,
                                    size_t message_len);
napi_status NAPI_CDECL napi_v8_fatal_exception(napi_env env, napi_value err);
napi_status NAPI_CDECL napi_v8_add_env_cleanup_hook(node_api_basic_env env,
                                                    napi_cleanup_hook fun,
                                                    void* arg);
napi_status NAPI_CDECL napi_v8_remove_env_cleanup_hook(node_api_basic_env env,
                                                       napi_cleanup_hook fun,
                                                       void* arg);
napi_status NAPI_CDECL napi_v8_create_buffer(napi_env env,
                                             size_t length,
                                             void** data,
                                             napi_value* result);
napi_status NAPI_CDECL napi_v8_create_buffer_copy(napi_env env,
                                                  size_t length,
                                                  const void* data,
                                                  void** result_data,
                                                  napi_value* result);
napi_status NAPI_CDECL napi_v8_create_external_buffer(napi_env env,
                                                      size_t length,
                                                      void* data,
                                                      node_api_basic_finalize finalize_cb,
                                                      void* finalize_hint,
                                                      napi_value* result);
napi_status NAPI_CDECL napi_v8_is_buffer(napi_env env, napi_value value, bool* result);
napi_status NAPI_CDECL napi_v8_get_buffer_info(napi_env env,
                                               napi_value value,
                                               void** data,
                                               size_t* length);
napi_status NAPI_CDECL napi_v8_create_buffer_from_arraybuffer(napi_env env,
                                                              napi_value arraybuffer,
                                                              size_t byte_offset,
                                                              size_t byte_length,
                                                              napi_value* result);
}  // extern "C"

#endif  // NAPI_V8_NODE_API_H_
