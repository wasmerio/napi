#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

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
  NAPI_CALL(env, unofficial_napi_create_env(8, &env2, &env2_scope));
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
  NAPI_CALL(env, unofficial_napi_release_env(env2_scope));

  return PrintSuccess("RUN_SCRIPT_TEST");
}
