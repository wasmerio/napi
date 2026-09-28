#include <stdio.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

static void NAPI_CDECL UnmanagedInterrupt(napi_env env, void* data) {
  (void)env;
  (void)data;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  unofficial_napi_profile_start_result profile_result =
      unofficial_napi_profile_start_ok;
  unofficial_napi_profile profile = NULL;
  CHECK_OR_FAIL(unofficial_napi_profile_start(env, unofficial_napi_profile_cpu,
                                              &profile_result, &profile) != napi_ok,
                "guest started an unbounded native CPU profiler");
  CHECK_OR_FAIL(profile == NULL, "rejected profiler returned a handle");
  CHECK_OR_FAIL(unofficial_napi_request_interrupt(env, UnmanagedInterrupt,
                                                  NULL) != napi_ok,
                "guest queued an unbounded native interrupt");

  napi_value unsupported = NULL;
  CHECK_OR_FAIL(unofficial_napi_take_heap_snapshot(env, NULL, &unsupported) !=
                    napi_ok,
                "guest created an uncharged native heap snapshot");
  CHECK_OR_FAIL(unsupported == NULL, "rejected snapshot returned a value");
  CHECK_OR_FAIL(unofficial_napi_create_serdes_binding(env, &unsupported) !=
                    napi_ok,
                "guest created an uncharged native serializer");
  CHECK_OR_FAIL(unsupported == NULL, "rejected serializer returned a value");

  unofficial_napi_bytecode_open_options bytecode_options = {0};
  bytecode_options.size = sizeof(bytecode_options);
  bytecode_options.version = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION;
  NAPI_CALL(env, napi_create_string_utf8(env, "1 + 1", NAPI_AUTO_LENGTH,
                                         &bytecode_options.source_text));
  NAPI_CALL(env, napi_create_string_utf8(env, "script.js", NAPI_AUTO_LENGTH,
                                         &bytecode_options.filename));
  bytecode_options.shape = unofficial_napi_bytecode_shape_script;
  NAPI_CALL(env, napi_get_undefined(env,
                                    &bytecode_options.params_or_undefined));
  bytecode_options.host_defined_option_id =
      bytecode_options.params_or_undefined;
  unofficial_napi_bytecode_open_result bytecode_result = {0};
  CHECK_OR_FAIL(unofficial_napi_bytecode_open(env, &bytecode_options,
                                               &bytecode_result) != napi_ok,
                "guest created an uncharged native bytecode handle");
  CHECK_OR_FAIL(bytecode_result.bytecode == NULL,
                "rejected bytecode open returned a handle");

  napi_value source;
  napi_value undefined_value;
  napi_value result = NULL;
  NAPI_CALL(env, napi_create_string_utf8(env, "1 + 1", NAPI_AUTO_LENGTH,
                                         &source));
  NAPI_CALL(env, napi_get_undefined(env, &undefined_value));
  const unofficial_napi_js_source script =
      unofficial_napi_js_source_from_text(source);

  CHECK_OR_FAIL(unofficial_napi_contextify_run_script(
                    env, undefined_value, &script, undefined_value, 0, 0,
                    0, true, false, false, undefined_value, &result) != napi_ok,
                "guest started an unmanaged timeout watchdog");
  CHECK_OR_FAIL(unofficial_napi_contextify_run_script(
                    env, undefined_value, &script, undefined_value, 0, 0,
                    -1, true, true, false, undefined_value, &result) != napi_ok,
                "guest installed a process-wide SIGINT watchdog");
  NAPI_CALL(env, unofficial_napi_contextify_run_script(
                     env, undefined_value, &script, undefined_value, 0, 0,
                     -1, true, false, false, undefined_value, &result));

  napi_value sparse_source;
  napi_value sparse_array;
  NAPI_CALL(env, napi_create_string_utf8(env, "new Array(4294967295)",
                                         NAPI_AUTO_LENGTH, &sparse_source));
  NAPI_CALL(env, napi_run_script(env, sparse_source, &sparse_array));
  const unofficial_napi_js_source function_source =
      unofficial_napi_js_source_from_text(source);
  CHECK_OR_FAIL(unofficial_napi_contextify_compile_function(
                    env, &function_source, source, 0, 0, undefined_value,
                    undefined_value, sparse_array, undefined_value,
                    &result) != napi_ok,
                "sparse parameter array caused a native reserve");
  CHECK_OR_FAIL(unofficial_napi_contextify_compile_function(
                    env, &function_source, source, 0, 0, undefined_value,
                    sparse_array, undefined_value, undefined_value,
                    &result) != napi_ok,
                "sparse context extension array caused a native reserve");

  puts("UNMANAGED_CONTROLS_REJECTED");
  return 0;
}
