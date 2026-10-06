#include <stdio.h>
#include <unistd.h>

#include "napi_test_helpers.h"

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  printf("ENV_READY\n");
  fflush(stdout);
  sleep(5);
  return 0;
}
