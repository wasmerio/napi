#include <stdio.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  napi_value source;
  napi_value expected;
  NAPI_CALL(env, napi_create_object(env, &source));
  NAPI_CALL(env, napi_create_int32(env, 42, &expected));
  NAPI_CALL(env, napi_set_named_property(env, source, "answer", expected));

  unofficial_napi_message message = NULL;
  NAPI_CALL(env, unofficial_napi_message_create(env, source, &message));
  CHECK_OR_FAIL(message != NULL, "message creation returned NULL");
  napi_value received = NULL;
  NAPI_CALL(env, unofficial_napi_message_take(env, message, &received));
  napi_value answer;
  int32_t actual = 0;
  NAPI_CALL(env, napi_get_named_property(env, received, "answer", &answer));
  NAPI_CALL(env, napi_get_value_int32(env, answer, &actual));
  CHECK_OR_FAIL(actual == 42, "received message changed across the bridge");

  // Deliberately leave one payload pending: the owning NapiCtx must release
  // its bytes and native handle when that workload is destroyed.
  unofficial_napi_message pending = NULL;
  NAPI_CALL(env, unofficial_napi_message_create(env, source, &pending));
  CHECK_OR_FAIL(pending != NULL, "pending message creation returned NULL");
  puts("MESSAGE_BUDGET_OK");
  return 0;
}
