// Provider-neutral handle-scope semantics: refs survive scope closure,
// scopes close in LIFO order, and repeated scope churn remains bounded.
#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  // N-API values are invalid after their owning scope closes. Using one after
  // close is outside the N-API contract, so a cross-provider test must not
  // require either rejection or continued validity from that invalid handle.
  napi_handle_scope scope;
  napi_valuetype vtype;

  // 1. A ref taken inside the scope must survive the close.
  NAPI_CALL(env, napi_open_handle_scope(env, &scope));
  napi_value obj;
  NAPI_CALL(env, napi_create_object(env, &obj));
  napi_ref ref;
  NAPI_CALL(env, napi_create_reference(env, obj, 1, &ref));
  NAPI_CALL(env, napi_close_handle_scope(env, scope));
  napi_value resolved = NULL;
  NAPI_CALL(env, napi_get_reference_value(env, ref, &resolved));
  CHECK_OR_FAIL(resolved != NULL, "ref should resolve after scope close");
  NAPI_CALL(env, napi_typeof(env, resolved, &vtype));
  CHECK_OR_FAIL(vtype == napi_object, "ref-resolved value should be an object");
  NAPI_CALL(env, napi_delete_reference(env, ref));

  // 2. Out-of-order close is a safe mismatch error, and recovery works.
  napi_handle_scope outer_scope, inner_scope;
  NAPI_CALL(env, napi_open_handle_scope(env, &outer_scope));
  NAPI_CALL(env, napi_open_handle_scope(env, &inner_scope));
  napi_status mismatch = napi_close_handle_scope(env, outer_scope);
  CHECK_OR_FAIL(mismatch != napi_ok, "closing outer scope first should fail");
  NAPI_CALL(env, napi_close_handle_scope(env, inner_scope));
  NAPI_CALL(env, napi_close_handle_scope(env, outer_scope));

  // 3. Leak check: churn far more values than the 2^20 slot-table capacity.
  // If closing a scope failed to reclaim slots, minting would start failing
  // partway through this loop.
  for (int turn = 0; turn < 20000; turn++) {
    napi_handle_scope churn_scope;
    NAPI_CALL(env, napi_open_handle_scope(env, &churn_scope));
    for (int i = 0; i < 100; i++) {
      napi_value num;
      NAPI_CALL(env, napi_create_int32(env, turn * 100 + i, &num));
    }
    NAPI_CALL(env, napi_close_handle_scope(env, churn_scope));
  }

  return PrintSuccess("TEST_HANDLE_SCOPE_LIFETIME");
}
