#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

// Provider calls that succeed with no object: a second heap profile while one
// runs (busy), and a validate-only bytecode open whose cache is rejected.
// They report success with a null handle.
int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  unofficial_napi_profile_start_result started;
  unofficial_napi_profile first = NULL;
  NAPI_CALL(env, unofficial_napi_profile_start(env, unofficial_napi_profile_heap, &started, &first));
  CHECK_OR_FAIL(started == unofficial_napi_profile_start_ok && first != NULL, "no profile");
  unofficial_napi_profile second = (unofficial_napi_profile)0x1;
  napi_status status =
      unofficial_napi_profile_start(env, unofficial_napi_profile_heap, &started, &second);
  printf("BUSY_STATUS=%d RESULT=%d\n", (int)status, (int)started);
  CHECK_OR_FAIL(status == napi_ok, "a busy profiler failed");
  CHECK_OR_FAIL(started == unofficial_napi_profile_start_busy, "not busy");
  CHECK_OR_FAIL(second == NULL, "a busy profiler returned a handle");
  napi_value json;
  NAPI_CALL(env, unofficial_napi_profile_stop(env, first, &json));

  napi_value source;
  napi_value filename;
  NAPI_CALL(env, napi_create_string_utf8(env, "1 + 1", NAPI_AUTO_LENGTH, &source));
  NAPI_CALL(env, napi_create_string_utf8(env, "v.js", NAPI_AUTO_LENGTH, &filename));
  static const uint8_t bad_cache[64] = {1, 2, 3, 4};
  unofficial_napi_bytecode_open_options options;
  memset(&options, 0, sizeof options);
  options.size = sizeof options;
  options.version = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION;
  options.source_text = source;
  options.filename = filename;
  options.shape = unofficial_napi_bytecode_shape_script;
  options.cache_bytes = bad_cache;
  options.cache_byte_length = sizeof bad_cache;
  options.has_cache = 1;
  options.cache_policy = unofficial_napi_bytecode_cache_validate_only;
  unofficial_napi_bytecode_open_result result;
  memset(&result, 0, sizeof result);
  status = unofficial_napi_bytecode_open(env, &options, &result);
  printf("VALIDATE_ONLY=%d REJECTED=%d\n", (int)status, (int)result.cache_rejected);
  CHECK_OR_FAIL(status == napi_ok, "a validate-only rejection failed");
  CHECK_OR_FAIL(result.cache_rejected == 1, "a bad cache was accepted");
  CHECK_OR_FAIL(result.bytecode == NULL, "a rejected validate-only open returned bytecode");

  puts("NULL_PROVIDER_RESULTS_OK");
  return 0;
}
