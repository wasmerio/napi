#include <stdio.h>

#include "napi_test_helpers.h"

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  // A standalone provider has no per-instance host-memory quota. It must not
  // silently apply the managed tenant's 4096-handle cap: HTTP write batches
  // and certificate enumeration create more transient values in one scope.
  for (int i = 0; i < 5000; ++i) {
    napi_value value = NULL;
    NAPI_CALL(env, napi_create_int32(env, i, &value));
    CHECK_OR_FAIL(value != NULL, "value handle was refused");
  }

  puts("UNLIMITED_VALUE_HANDLES_OK");
  return 0;
}
