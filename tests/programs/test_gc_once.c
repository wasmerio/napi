#include <stdio.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

// Makes some garbage, then collects it: V8 posts background work for the
// collection (sweeping, parallel marking and compaction jobs).
int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  napi_value source = NULL;
  napi_value result = NULL;
  NAPI_CALL(env, napi_create_string_utf8(
                     env,
                     "let keep = []; for (let i = 0; i < 2000; i++) "
                     "keep.push({ i, text: 'x'.repeat(64), list: new Array(64).fill(i) }); "
                     "keep = null; 0",
                     NAPI_AUTO_LENGTH, &source));
  NAPI_CALL(env, napi_run_script(env, source, &result));
  NAPI_CALL(env, unofficial_napi_collect_garbage(env));
  puts("GC_DONE");
  return 0;
}
