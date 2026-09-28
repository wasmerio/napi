#include <stdio.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  NAPI_CALL(env, unofficial_napi_collect_garbage(env));
  printf("GC_DONE\n");
  return 0;
}
