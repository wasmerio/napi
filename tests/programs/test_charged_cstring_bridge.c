#include <stdio.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  napi_value object;
  napi_value value;
  napi_value got;
  bool has = false;
  int32_t number = 0;
  NAPI_CALL(env, napi_create_object(env, &object));
  NAPI_CALL(env, napi_create_int32(env, 42, &value));
  NAPI_CALL(env, napi_set_named_property(env, object, "charged", value));
  NAPI_CALL(env, napi_get_named_property(env, object, "charged", &got));
  NAPI_CALL(env, napi_get_value_int32(env, got, &number));
  CHECK_OR_FAIL(number == 42, "named property value changed");
  NAPI_CALL(env, napi_has_named_property(env, object, "charged", &has));
  CHECK_OR_FAIL(has, "named property was lost");

  NAPI_CALL(env, napi_throw_error(env, "E_GUEST", "guest error"));
  NAPI_CALL(env, napi_get_and_clear_last_exception(env, &got));
  NAPI_CALL(env, napi_throw_type_error(env, "E_TYPE", "guest type error"));
  NAPI_CALL(env, napi_get_and_clear_last_exception(env, &got));
  NAPI_CALL(env, napi_throw_range_error(env, "E_RANGE", "guest range error"));
  NAPI_CALL(env, napi_get_and_clear_last_exception(env, &got));
  puts("CHARGED_CSTRING_BRIDGE_OK");
  return 0;
}
