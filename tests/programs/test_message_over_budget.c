#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

static int check_clone_out_of_memory(napi_env env) {
  napi_value error;
  NAPI_CALL(env, napi_get_and_clear_last_exception(env, &error));
  napi_value text;
  NAPI_CALL(env, napi_coerce_to_string(env, error, &text));
  char buffer[256] = {0};
  size_t length = 0;
  NAPI_CALL(env, napi_get_value_string_utf8(env, text, buffer, sizeof buffer, &length));
  printf("REFUSAL=%s\n", buffer);
  CHECK_OR_FAIL(strstr(buffer, "out of memory") != NULL, "not a clone out-of-memory error");
  return 0;
}

// Message creation and structured clone both report a retrievable exception
// when serialization exceeds the remaining budget.
int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  // 40 MiB of guest memory: serialized, it does not fit what the budget has
  // left.
  napi_value big;
  void* data = NULL;
  NAPI_CALL(env, napi_create_arraybuffer(env, 40 * 1024 * 1024, &data, &big));

  unofficial_napi_message message = NULL;
  napi_status status = unofficial_napi_message_create(env, big, &message);
  CHECK_OR_FAIL(status == napi_pending_exception, "the big message was not refused");
  CHECK_OR_FAIL(message == NULL, "a refused message was returned");
  if (check_clone_out_of_memory(env) != 0) return 1;

  napi_value cloned = NULL;
  status = unofficial_napi_structured_clone(env, big, NULL, &cloned);
  CHECK_OR_FAIL(status == napi_pending_exception, "the big clone was not refused");
  CHECK_OR_FAIL(cloned == NULL, "a refused clone was returned");
  if (check_clone_out_of_memory(env) != 0) return 1;

  napi_value small;
  NAPI_CALL(env, napi_create_int32(env, 7, &small));
  NAPI_CALL(env, unofficial_napi_structured_clone(env, small, NULL, &cloned));
  NAPI_CALL(env, unofficial_napi_message_create(env, small, &message));
  napi_value received;
  NAPI_CALL(env, unofficial_napi_message_take(env, message, &received));
  puts("MESSAGE_OVER_BUDGET_OK");
  return 0;
}
