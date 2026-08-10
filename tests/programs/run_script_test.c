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

  // Environment teardown owns the failure/cancellation path for outstanding
  // leases. It must discard the snapshot and host reference without requiring
  // a scope-bound value or an explicit release from already-destroyed Edge
  // state.
  napi_env lease_env = NULL;
  void* lease_env_scope = NULL;
  NAPI_CALL(env, unofficial_napi_create_env(8, &lease_env, &lease_env_scope));
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
  NAPI_CALL(env, unofficial_napi_release_env(lease_env_scope));

  return PrintSuccess("RUN_SCRIPT_TEST");
}
