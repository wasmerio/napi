#include <stdio.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

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

  puts("UNMANAGED_CONTROLS_REJECTED");
  return 0;
}
