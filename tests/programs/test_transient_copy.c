#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "napi_test_helpers.h"

// The host copies a string's bytes out of guest memory before V8 sees them:
// a copy the budget cannot hold fails, a smaller one works.
int main(int argc, char** argv) {
  CHECK_OR_FAIL(argc == 2, "usage: test_transient_copy <MiB>");
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  const size_t size = strtoul(argv[1], NULL, 10) * 1024 * 1024;
  char* bytes = malloc(size);
  CHECK_OR_FAIL(bytes != NULL, "malloc failed");
  memset(bytes, 'a', size);

  napi_value value;
  napi_status status = napi_create_string_latin1(env, bytes, size, &value);
  printf("BIG_COPY_STATUS=%d\n", (int)status);
  CHECK_OR_FAIL(status != napi_ok, "the big copy was not refused");
  NAPI_CALL(env, napi_create_string_latin1(env, bytes, 1024 * 1024, &value));
  free(bytes);
  puts("TRANSIENT_COPY_OK");
  return 0;
}
