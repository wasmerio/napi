#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "node_api.h"
#include "unofficial_napi.h"

static int wrap_finalizer_count = 0;
static int add_finalizer_count = 0;

static void NAPI_CDECL CountWrapFinalizer(napi_env env,
                                          void* data,
                                          void* hint) {
  (void)env;
  if ((uintptr_t)data == 0x11 && (uintptr_t)hint == 0x12) {
    wrap_finalizer_count++;
  }
}

static void NAPI_CDECL CountAddedFinalizer(napi_env env,
                                           void* data,
                                           void* hint) {
  (void)env;
  if ((uintptr_t)data == 0x21 && (uintptr_t)hint == 0x22) {
    add_finalizer_count++;
  }
}

static int RunInt32Script(napi_env env, const char* source, int32_t* result_out) {
  napi_value script;
  napi_value result;
  napi_status status = napi_create_string_utf8(
      env, source, NAPI_AUTO_LENGTH, &script);
  if (status != napi_ok) return status;
  status = napi_run_script(env, script, &result);
  if (status != napi_ok) return status;
  return napi_get_value_int32(env, result, result_out);
}

static int RunStringScript(napi_env env,
                           const char* source,
                           char* result_out,
                           size_t result_capacity) {
  napi_value script;
  napi_value result;
  size_t result_length = 0;
  napi_status status = napi_create_string_utf8(
      env, source, NAPI_AUTO_LENGTH, &script);
  if (status != napi_ok) return status;
  status = napi_run_script(env, script, &result);
  if (status != napi_ok) return status;
  return napi_get_value_string_utf8(
      env, result, result_out, result_capacity, &result_length);
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  // Run a simple script that returns a string
  napi_value script_str;
  NAPI_CALL(env, napi_create_string_utf8(env, "'Hello' + ', World!'",
                                          NAPI_AUTO_LENGTH, &script_str));
  napi_value result;
  NAPI_CALL(env, napi_run_script(env, script_str, &result));

  // Check the result
  char buf[256];
  size_t len;
  NAPI_CALL(env, napi_get_value_string_utf8(env, result, buf, sizeof(buf), &len));
  CHECK_OR_FAIL(strcmp(buf, "Hello, World!") == 0, "script result mismatch");

  // Run a script that returns a number
  napi_value num_script;
  NAPI_CALL(env, napi_create_string_utf8(env, "2 + 3 * 4",
                                          NAPI_AUTO_LENGTH, &num_script));
  napi_value num_result;
  NAPI_CALL(env, napi_run_script(env, num_script, &num_result));

  double num;
  NAPI_CALL(env, napi_get_value_double(env, num_result, &num));
  CHECK_OR_FAIL(num > 13.9 && num < 14.1, "expected 14");

  // Each N-API environment owns its global context. This is also a control
  // plane boundary: evaluating in the host worker's real global would leak the
  // marker into env2 and allow Edge bootstrap code to replace Wasmer's timers,
  // Promise, console, and worker bridge.
  int32_t marker = 0;
  NAPI_CALL(env, RunInt32Script(
                     env, "globalThis.__napi_env_isolation_marker = 41", &marker));
  CHECK_OR_FAIL(marker == 41, "failed to set the first environment marker");

  napi_env env2 = NULL;
  void* env2_scope = NULL;
  NAPI_CALL(env, unofficial_napi_create_env(8, NULL, &env2, &env2_scope));
  CHECK_OR_FAIL(env2 != NULL && env2_scope != NULL,
                "failed to create the second N-API environment");

  char marker_type[32] = {0};
  NAPI_CALL(env2, RunStringScript(env2,
                                  "typeof globalThis.__napi_env_isolation_marker",
                                  marker_type,
                                  sizeof(marker_type)));
  CHECK_OR_FAIL(strcmp(marker_type, "undefined") == 0,
                "napi_run_script leaked state between environments");

  NAPI_CALL(env2, RunInt32Script(
                      env2, "globalThis.__napi_env_isolation_marker = 99", &marker));
  CHECK_OR_FAIL(marker == 99, "failed to set the second environment marker");
  NAPI_CALL(env, RunInt32Script(
                     env, "globalThis.__napi_env_isolation_marker", &marker));
  CHECK_OR_FAIL(marker == 41, "the second environment mutated the first global");
  NAPI_CALL(env, unofficial_napi_release_env(env2_scope, NULL));

  // Every code-generation path must select its execution scope explicitly.
  // In particular, vm compile-function and module compilation must honor the
  // supplied context rather than falling back to the active environment or
  // the Wasmer worker's control-plane global.
  napi_value undefined_value;
  NAPI_CALL(env, napi_get_undefined(env, &undefined_value));
  napi_value sandbox;
  NAPI_CALL(env, napi_create_object(env, &sandbox));
  napi_value context_marker;
  NAPI_CALL(env, napi_create_int32(env, 73, &context_marker));
  NAPI_CALL(env, napi_set_named_property(
                     env, sandbox, "__napi_context_marker", context_marker));
  napi_value context;
  NAPI_CALL(env, unofficial_napi_contextify_make_context(
                     env,
                     sandbox,
                     undefined_value,
                     undefined_value,
                     true,
                     true,
                     true,
                     undefined_value,
                     &context));

  napi_value context_source_text;
  NAPI_CALL(env, napi_create_string_utf8(
                     env,
                     "globalThis.__napi_context_marker",
                     NAPI_AUTO_LENGTH,
                     &context_source_text));
  const unofficial_napi_js_source context_source =
      unofficial_napi_js_source_from_text(context_source_text);
  napi_value context_result;
  NAPI_CALL(env, unofficial_napi_contextify_run_script(
                     env,
                     context,
                     &context_source,
                     undefined_value,
                     0,
                     0,
                     -1,
                     true,
                     false,
                     false,
                     undefined_value,
                     &context_result));
  NAPI_CALL(env, napi_get_value_int32(env, context_result, &marker));
  CHECK_OR_FAIL(marker == 73,
                "contextify_run_script escaped its explicit context");

  napi_value parameters;
  napi_value context_extensions;
  NAPI_CALL(env, napi_create_array_with_length(env, 0, &parameters));
  NAPI_CALL(env, napi_create_array_with_length(env, 1, &context_extensions));
  napi_value context_extension;
  napi_value extension_marker;
  NAPI_CALL(env, napi_create_object(env, &context_extension));
  NAPI_CALL(env, napi_create_int32(env, 74, &extension_marker));
  NAPI_CALL(env, napi_set_named_property(
                     env,
                     context_extension,
                     "__napi_context_extension_marker",
                     extension_marker));
  NAPI_CALL(env, napi_set_element(
                     env, context_extensions, 0, context_extension));
  napi_value function_source_text;
  NAPI_CALL(env, napi_create_string_utf8(
                     env,
                     "return globalThis.__napi_context_marker + "
                     "__napi_context_extension_marker;",
                     NAPI_AUTO_LENGTH,
                     &function_source_text));
  const unofficial_napi_js_source function_source =
      unofficial_napi_js_source_from_text(function_source_text);
  napi_value compiled;
  NAPI_CALL(env, unofficial_napi_contextify_compile_function(
                     env,
                     &function_source,
                     undefined_value,
                     0,
                     0,
                     context,
                     context_extensions,
                     parameters,
                     undefined_value,
                     &compiled));
  napi_value compiled_function;
  NAPI_CALL(env, napi_get_named_property(
                     env, compiled, "function", &compiled_function));
  napi_value env_global;
  NAPI_CALL(env, napi_get_global(env, &env_global));
  napi_value function_result;
  NAPI_CALL(env, napi_call_function(
                     env,
                     env_global,
                     compiled_function,
                     0,
                     NULL,
                     &function_result));
  NAPI_CALL(env, napi_get_value_int32(env, function_result, &marker));
  CHECK_OR_FAIL(marker == 147,
                "compile_function escaped its parsing context");

  napi_value module_wrapper;
  napi_value module_url;
  napi_value module_source_text;
  NAPI_CALL(env, napi_create_object(env, &module_wrapper));
  NAPI_CALL(env, napi_create_string_utf8(
                     env,
                     "file:///context-module.mjs",
                     NAPI_AUTO_LENGTH,
                     &module_url));
  NAPI_CALL(env, napi_create_string_utf8(
                     env,
                     "export const observed = "
                     "globalThis.__napi_context_marker;",
                     NAPI_AUTO_LENGTH,
                     &module_source_text));
  const unofficial_napi_js_source module_source =
      unofficial_napi_js_source_from_text(module_source_text);
  unofficial_napi_module_create_options module_options = {0};
  module_options.size = sizeof(module_options);
  module_options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  module_options.kind = unofficial_napi_module_source_text;
  module_options.wrapper = module_wrapper;
  module_options.url = module_url;
  module_options.context_or_undefined = context;
  module_options.payload.source_text.source = &module_source;
  module_options.payload.source_text.host_defined_option_id = undefined_value;
  unofficial_napi_module module = NULL;
  NAPI_CALL(env, unofficial_napi_module_wrap_create(env, &module_options, &module));
  CHECK_OR_FAIL(module != NULL,
                "module compilation did not return a handle");
  unofficial_napi_module_state module_state = {0};
  NAPI_CALL(env, unofficial_napi_module_wrap_get_state(
                     env, module, &module_state));
  CHECK_OR_FAIL(module_state.status == 0,
                "new module snapshot did not report uninstantiated status");
  CHECK_OR_FAIL(module_state.error != NULL,
                "module snapshot did not return an undefined error value");
  CHECK_OR_FAIL(!module_state.has_top_level_await,
                "synchronous module reported top-level await");
  CHECK_OR_FAIL(!module_state.has_async_graph,
                "uninstantiated module reported an async graph");
  NAPI_CALL(env, unofficial_napi_module_wrap_link(
                     env, module, 0, NULL));
  NAPI_CALL(env, unofficial_napi_module_wrap_instantiate(env, module));
  NAPI_CALL(env, unofficial_napi_module_wrap_get_state(
                     env, module, &module_state));
  CHECK_OR_FAIL(module_state.status == 2,
                "instantiated module snapshot reported the wrong status");
  CHECK_OR_FAIL(!module_state.has_async_graph,
                "synchronous module reported an async graph");
  napi_value module_result;
  NAPI_CALL(env, unofficial_napi_module_wrap_evaluate_sync(
                     env,
                     module,
                     module_url,
                     module_url,
                     &module_result));
  napi_value module_namespace;
  napi_value module_observed;
  NAPI_CALL(env, unofficial_napi_module_wrap_get_namespace(
                     env, module, &module_namespace));
  NAPI_CALL(env, napi_get_named_property(
                     env, module_namespace, "observed", &module_observed));
  NAPI_CALL(env, napi_get_value_int32(env, module_observed, &marker));
  CHECK_OR_FAIL(marker == 73,
                "module evaluation escaped its explicit context");
  NAPI_CALL(env, unofficial_napi_module_wrap_destroy(env, module));

  // A memory lease, rather than a scope-bound napi_value or its data pointer,
  // owns the value and copy-back. Releasing after the handle scope closes is
  // the contract retained native users such as zlib and async fs require.
  napi_handle_scope lease_scope = NULL;
  NAPI_CALL(env, napi_open_handle_scope(env, &lease_scope));
  napi_value lease_script;
  napi_value lease_value;
  NAPI_CALL(env,
            napi_create_string_utf8(
                env,
                "globalThis.__napi_lease_value = new Uint8Array([10, 20, 30, 40])",
                NAPI_AUTO_LENGTH,
                &lease_script));
  NAPI_CALL(env, napi_run_script(env, lease_script, &lease_value));

  unofficial_napi_buffer_lease lease = NULL;
  uint8_t* lease_data = NULL;
  NAPI_CALL(env,
            unofficial_napi_acquire_buffer_lease(
                env,
                lease_value,
                1,
                2,
                unofficial_napi_buffer_access_readwrite,
                &lease,
                (void**)&lease_data));
  CHECK_OR_FAIL(lease != NULL && lease_data != NULL,
                "buffer lease did not return ownership and data");
  CHECK_OR_FAIL(lease_data[0] == 20 && lease_data[1] == 30,
                "buffer lease copied the wrong range");
  lease_data[0] = 21;
  lease_data[1] = 31;
  NAPI_CALL(env, napi_close_handle_scope(env, lease_scope));
  NAPI_CALL(env, unofficial_napi_release_buffer_lease(env, lease, true));

  NAPI_CALL(env,
            RunInt32Script(env,
                           "__napi_lease_value[1] * 100 + __napi_lease_value[2]",
                           &marker));
  CHECK_OR_FAIL(marker == 2131,
                "buffer lease did not publish writes after scope closure");

  napi_value shared_script;
  napi_value shared_value;
  NAPI_CALL(env,
            napi_create_string_utf8(
                env,
                "globalThis.__napi_shared_lease = new SharedArrayBuffer(4); "
                "new Uint8Array(__napi_shared_lease).set([5, 6, 7, 8]); "
                "__napi_shared_lease",
                NAPI_AUTO_LENGTH,
                &shared_script));
  NAPI_CALL(env, napi_run_script(env, shared_script, &shared_value));
  unofficial_napi_buffer_lease shared_lease = NULL;
  uint8_t* shared_data = NULL;
  NAPI_CALL(env,
            unofficial_napi_acquire_buffer_lease(
                env,
                shared_value,
                1,
                2,
                unofficial_napi_buffer_access_readwrite,
                &shared_lease,
                (void**)&shared_data));
  CHECK_OR_FAIL(shared_lease != NULL && shared_data != NULL &&
                    shared_data[0] == 6 && shared_data[1] == 7,
                "buffer lease did not expose a SharedArrayBuffer range");
  shared_data[0] = 16;
  shared_data[1] = 17;
  NAPI_CALL(env,
            unofficial_napi_release_buffer_lease(env, shared_lease, true));
  NAPI_CALL(env,
            RunInt32Script(env,
                           "new Uint8Array(__napi_shared_lease)[1] * 100 + "
                           "new Uint8Array(__napi_shared_lease)[2]",
                           &marker));
  CHECK_OR_FAIL(marker == 1617,
                "buffer lease did not publish SharedArrayBuffer writes");

  // The host-JS bridge must preserve the public typed-array enum and exact
  // byte width. Float32 used to be treated as an 8-byte element, over-reading
  // the backing store when a guest requested its data pointer.
  napi_value typed_backing;
  void* typed_backing_data = NULL;
  NAPI_CALL(env,
            napi_create_arraybuffer(
                env, 16, &typed_backing_data, &typed_backing));
  CHECK_OR_FAIL(typed_backing_data != NULL,
                "typed-array backing did not expose guest memory");
  napi_value float32_view;
  NAPI_CALL(env,
            napi_create_typedarray(
                env, napi_float32_array, 2, typed_backing, 4, &float32_view));
  napi_typedarray_type float32_kind;
  size_t float32_length = 0;
  size_t float32_offset = 0;
  void* float32_data = NULL;
  NAPI_CALL(env,
            napi_get_typedarray_info(env,
                                     float32_view,
                                     &float32_kind,
                                     &float32_length,
                                     &float32_data,
                                     NULL,
                                     &float32_offset));
  CHECK_OR_FAIL(float32_kind == napi_float32_array &&
                    float32_length == 2 &&
                    float32_offset == 4 &&
                    float32_data == (uint8_t*)typed_backing_data + 4,
                "Float32Array guest range used the wrong element width");

  // External-buffer ABI order is length before data. Swapping the two makes
  // the bridge interpret a byte count as a guest pointer.
  uint8_t external_bytes[4] = {9, 8, 7, 6};
  napi_value external_buffer;
  NAPI_CALL(env,
            napi_create_external_buffer(env,
                                        sizeof(external_bytes),
                                        external_bytes,
                                        NULL,
                                        NULL,
                                        &external_buffer));
  void* external_buffer_data = NULL;
  size_t external_buffer_length = 0;
  NAPI_CALL(env,
            napi_get_buffer_info(env,
                                 external_buffer,
                                 &external_buffer_data,
                                 &external_buffer_length));
  CHECK_OR_FAIL(external_buffer_length == sizeof(external_bytes) &&
                    external_buffer_data != NULL &&
                    memcmp(external_buffer_data,
                           external_bytes,
                           sizeof(external_bytes)) == 0,
                "external buffer ABI order corrupted its guest range");

  napi_value external_value;
  napi_valuetype external_type = napi_undefined;
  NAPI_CALL(env,
            napi_create_external(env,
                                 (void*)(uintptr_t)0x1234,
                                 NULL,
                                 NULL,
                                 &external_value));
  NAPI_CALL(env, napi_typeof(env, external_value, &external_type));
  CHECK_OR_FAIL(external_type == napi_external,
                "napi_typeof classified an external as an object");

  // Escaping the first explicit scope transfers ownership into the env's root
  // frame. It must not remove the value from every frame and leak an unowned
  // handle.
  napi_escapable_handle_scope escape_scope = NULL;
  NAPI_CALL(env, napi_open_escapable_handle_scope(env, &escape_scope));
  napi_value scoped_value;
  napi_value escaped_value;
  NAPI_CALL(env, napi_create_object(env, &scoped_value));
  NAPI_CALL(env,
            napi_escape_handle(env,
                               escape_scope,
                               scoped_value,
                               &escaped_value));
  NAPI_CALL(env, napi_close_escapable_handle_scope(env, escape_scope));
  napi_value escaped_marker;
  NAPI_CALL(env, napi_create_int32(env, 55, &escaped_marker));
  NAPI_CALL(env,
            napi_set_named_property(
                env, escaped_value, "marker", escaped_marker));
  NAPI_CALL(env,
            napi_get_named_property(
                env, escaped_value, "marker", &escaped_marker));
  NAPI_CALL(env, napi_get_value_int32(env, escaped_marker, &marker));
  CHECK_OR_FAIL(marker == 55,
                "outermost escaped handle lost root-frame ownership");

  // Forged guest lengths are rejected before allocating a host scratch
  // buffer. This call must return an error rather than aborting the host.
  napi_status oversized_string_status =
      napi_get_value_string_utf8(env,
                                 result,
                                 buf,
                                 (size_t)INT32_MAX,
                                 &len);
  CHECK_OR_FAIL(oversized_string_status != napi_ok,
                "oversized guest string length was not rejected");

  // Guest wrap/add-finalizer callbacks are dispatched at an explicit provider
  // checkpoint; environment teardown force-drains remaining live records.
  napi_env finalizer_env = NULL;
  void* finalizer_env_scope = NULL;
  NAPI_CALL(env,
            unofficial_napi_create_env(
                8, NULL, &finalizer_env, &finalizer_env_scope));
  napi_value wrapped_object;
  napi_value finalized_object;
  NAPI_CALL(finalizer_env,
            napi_create_object(finalizer_env, &wrapped_object));
  NAPI_CALL(finalizer_env,
            napi_wrap(finalizer_env,
                      wrapped_object,
                      (void*)(uintptr_t)0x11,
                      CountWrapFinalizer,
                      (void*)(uintptr_t)0x12,
                      NULL));
  NAPI_CALL(finalizer_env,
            napi_create_object(finalizer_env, &finalized_object));
  NAPI_CALL(finalizer_env,
            napi_add_finalizer(finalizer_env,
                               finalized_object,
                               (void*)(uintptr_t)0x21,
                               CountAddedFinalizer,
                               (void*)(uintptr_t)0x22,
                               NULL));
  NAPI_CALL(env, unofficial_napi_release_env(finalizer_env_scope, NULL));
  CHECK_OR_FAIL(wrap_finalizer_count == 1 && add_finalizer_count == 1,
                "guest finalizers were not dispatched exactly once");

  // Environment teardown owns the failure/cancellation path for outstanding
  // leases. It must discard the snapshot and host reference without requiring
  // a scope-bound value or an explicit release from already-destroyed Edge
  // state.
  napi_env lease_env = NULL;
  void* lease_env_scope = NULL;
  NAPI_CALL(env, unofficial_napi_create_env(8, NULL, &lease_env, &lease_env_scope));
  napi_value abandoned_script;
  napi_value abandoned_value;
  NAPI_CALL(lease_env,
            napi_create_string_utf8(lease_env,
                                    "new Uint8Array([1, 2, 3, 4])",
                                    NAPI_AUTO_LENGTH,
                                    &abandoned_script));
  NAPI_CALL(lease_env,
            napi_run_script(lease_env, abandoned_script, &abandoned_value));
  unofficial_napi_buffer_lease abandoned_lease = NULL;
  void* abandoned_data = NULL;
  NAPI_CALL(lease_env,
            unofficial_napi_acquire_buffer_lease(
                lease_env,
                abandoned_value,
                0,
                4,
                unofficial_napi_buffer_access_read,
                &abandoned_lease,
                &abandoned_data));
  CHECK_OR_FAIL(abandoned_lease != NULL && abandoned_data != NULL,
                "failed to acquire teardown lease");
  NAPI_CALL(env, unofficial_napi_release_env(lease_env_scope, NULL));

  return PrintSuccess("RUN_SCRIPT_TEST");
}
