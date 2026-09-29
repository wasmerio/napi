#include <stdio.h>

#include "napi_test_helpers.h"

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  // Value handles are charged to the memory budget, never capped at a fixed
  // count: HTTP write batches and certificate enumeration create thousands of
  // transient values in one scope.
  for (int i = 0; i < 5000; ++i) {
    napi_value value = NULL;
    NAPI_CALL(env, napi_create_int32(env, i, &value));
    CHECK_OR_FAIL(value != NULL, "value handle was refused");
  }

  puts("UNLIMITED_VALUE_HANDLES_OK");
  return 0;
}
