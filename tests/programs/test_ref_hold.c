#include <stdio.h>
#include <stdint.h>

#include "napi_test_helpers.h"

// Retains a strong reference per object until the embedder's budget refuses
// the bookkeeping. The refusal is an out-of-memory condition for the whole
// env, so JS must not run afterwards.
#define MAX_REFS (4u * 1024u * 1024u)

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  puts("REF_HOLD_START");

  // Prepared up front: once the budget is exhausted, nothing that copies
  // guest data to the host (such as creating a string) can be charged.
  napi_value script;
  napi_ref script_ref;
  NAPI_CALL(env, napi_create_string_utf8(env, "1 + 1", NAPI_AUTO_LENGTH,
                                         &script));
  NAPI_CALL(env, napi_create_reference(env, script, 1, &script_ref));

  unsigned held = 0;
  napi_ref last = NULL;
  napi_status status = napi_ok;
  while (held < MAX_REFS) {
    napi_handle_scope scope;
    NAPI_CALL(env, napi_open_handle_scope(env, &scope));
    napi_value obj;
    NAPI_CALL(env, napi_create_object(env, &obj));
    napi_ref ref;
    status = napi_create_reference(env, obj, 1, &ref);
    NAPI_CALL(env, napi_close_handle_scope(env, scope));
    if (status != napi_ok) break;
    last = ref;
    ++held;
  }
  CHECK_OR_FAIL(status != napi_ok, "the budget never refused a reference");
  printf("REF_HOLD_REFUSED held=%u status=%d\n", held, (int)status);

  // Make room for the script's own handles: the env must stay terminated
  // even though the budget could now cover them.
  CHECK_OR_FAIL(last != NULL, "no reference was ever held");
  NAPI_CALL(env, napi_delete_reference(env, last));
  napi_value result;
  NAPI_CALL(env, napi_get_reference_value(env, script_ref, &script));
  status = napi_run_script(env, script, &result);
  printf("JS_AFTER_REFUSAL status=%d\n", (int)status);
  return 0;
}
