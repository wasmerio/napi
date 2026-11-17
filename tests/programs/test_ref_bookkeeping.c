#include <stdio.h>
#include <stdlib.h>

#include "napi_test_helpers.h"

// Creates references until the bridge refuses one (or a ceiling is reached),
// then deletes them all and creates one more.
int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  napi_value object;
  NAPI_CALL(env, napi_create_object(env, &object));

  const size_t ceiling = 4 * 1024 * 1024;
  napi_ref* refs = malloc(ceiling * sizeof(napi_ref));
  CHECK_OR_FAIL(refs != NULL, "malloc failed");
  size_t count = 0;
  int refused = 0;
  while (count < ceiling) {
    if (napi_create_reference(env, object, 1, &refs[count]) != napi_ok) {
      refused = 1;
      break;
    }
    count++;
  }
  printf("REFS=%zu REFUSED=%d\n", count, refused);
  for (size_t i = 0; i < count; i++) NAPI_CALL(env, napi_delete_reference(env, refs[i]));
  napi_ref again;
  NAPI_CALL(env, napi_create_reference(env, object, 1, &again));
  NAPI_CALL(env, napi_delete_reference(env, again));
  free(refs);
  puts("REF_BOOKKEEPING_OK");
  return 0;
}
