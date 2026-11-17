// napi's implementation of node_api.h's Buffer, env cleanup hook and fatal
// calls. Deprecated: these are Node's runtime, not the engine's. Buffer's
// prototype is made by Node's lib/, so an embedder such as Edge.js defines
// these calls itself (building Buffers with unofficial_napi_create_uint8array)
// and builds napi with NAPI_PROVIDE_NODE_API=OFF, which leaves this file out
// of the provider library (napi's own tests then link it as a library of its
// own). See docs/node-api.md.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "internal/napi_node_api.h"
#include "node_api.h"

extern "C" {

void NAPI_CDECL napi_fatal_error(const char* location,
                                 size_t location_len,
                                 const char* message,
                                 size_t message_len) {
  const char* loc = location == nullptr ? "" : location;
  const char* msg = message == nullptr ? "" : message;
  size_t loc_len = location_len == NAPI_AUTO_LENGTH ? std::strlen(loc) : location_len;
  size_t msg_len = message_len == NAPI_AUTO_LENGTH ? std::strlen(msg) : message_len;
  std::fprintf(stderr, "FATAL ERROR: %.*s %.*s\n", static_cast<int>(loc_len), loc,
               static_cast<int>(msg_len), msg);
  std::fflush(stderr);
  std::abort();
}

napi_status NAPI_CDECL napi_fatal_exception(napi_env env, napi_value err) {
  return napi_quickjs_fatal_exception(env, err);
}

napi_status NAPI_CDECL napi_add_env_cleanup_hook(node_api_basic_env env,
                                                 napi_cleanup_hook fun,
                                                 void* arg) {
  return napi_quickjs_add_env_cleanup_hook(env, fun, arg);
}

napi_status NAPI_CDECL napi_remove_env_cleanup_hook(node_api_basic_env env,
                                                    napi_cleanup_hook fun,
                                                    void* arg) {
  return napi_quickjs_remove_env_cleanup_hook(env, fun, arg);
}

napi_status NAPI_CDECL napi_create_buffer(napi_env env,
                                          size_t length,
                                          void** data,
                                          napi_value* result) {
  return napi_quickjs_create_buffer(env, length, data, result);
}

napi_status NAPI_CDECL napi_create_buffer_copy(napi_env env,
                                               size_t length,
                                               const void* data,
                                               void** result_data,
                                               napi_value* result) {
  return napi_quickjs_create_buffer_copy(env, length, data, result_data, result);
}

napi_status NAPI_CDECL napi_create_external_buffer(napi_env env,
                                                   size_t length,
                                                   void* data,
                                                   node_api_basic_finalize finalize_cb,
                                                   void* finalize_hint,
                                                   napi_value* result) {
  return napi_quickjs_create_external_buffer(env, length, data, finalize_cb, finalize_hint, result);
}

napi_status NAPI_CDECL napi_is_buffer(napi_env env, napi_value value, bool* result) {
  return napi_quickjs_is_buffer(env, value, result);
}

napi_status NAPI_CDECL napi_get_buffer_info(napi_env env,
                                            napi_value value,
                                            void** data,
                                            size_t* length) {
  return napi_quickjs_get_buffer_info(env, value, data, length);
}

napi_status NAPI_CDECL node_api_create_buffer_from_arraybuffer(napi_env env,
                                                               napi_value arraybuffer,
                                                               size_t byte_offset,
                                                               size_t byte_length,
                                                               napi_value* result) {
  return napi_quickjs_create_buffer_from_arraybuffer(env, arraybuffer, byte_offset, byte_length,
                                                result);
}

}  // extern "C"
