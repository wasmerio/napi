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

  // Outside a JavaScript callback, the bridge may report
  // napi_pending_exception even when the arguments reached V8. Match the
  // existing throw test's convention, clearing only after napi_ok.
  napi_status status = napi_throw_error(env, "E_GUEST", "guest error");
  CHECK_OR_FAIL(status == napi_ok || status == napi_pending_exception,
                "napi_throw_error returned an unexpected status");
  if (status == napi_ok) {
    NAPI_CALL(env, napi_get_and_clear_last_exception(env, &got));
  }
  status = napi_throw_type_error(env, "E_TYPE", "guest type error");
  CHECK_OR_FAIL(status == napi_ok || status == napi_pending_exception,
                "napi_throw_type_error returned an unexpected status");
  if (status == napi_ok) {
    NAPI_CALL(env, napi_get_and_clear_last_exception(env, &got));
  }
  status = napi_throw_range_error(env, "E_RANGE", "guest range error");
  CHECK_OR_FAIL(status == napi_ok || status == napi_pending_exception,
                "napi_throw_range_error returned an unexpected status");
  if (status == napi_ok) {
    NAPI_CALL(env, napi_get_and_clear_last_exception(env, &got));
  }
  puts("CHARGED_CSTRING_BRIDGE_OK");
  return 0;
}
