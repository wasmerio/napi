#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

static napi_value no_op(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value result = NULL;
  napi_get_undefined(env, &result);
  return result;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  static const char name[] = {'a', '\0', 'b'};

  napi_value function;
  NAPI_CALL(env, napi_create_function(env, name, sizeof(name), no_op, NULL,
                                      &function));
  napi_value function_name;
  NAPI_CALL(env, napi_get_named_property(env, function, "name", &function_name));
  char value[sizeof(name) + 1] = {0};
  size_t length = 0;
  NAPI_CALL(env, napi_get_value_string_utf8(env, function_name, value,
                                            sizeof(value), &length));
  CHECK_OR_FAIL(length == sizeof(name), "explicit function name was truncated");
  CHECK_OR_FAIL(memcmp(value, name, sizeof(name)) == 0,
                "explicit function name bytes changed");

  napi_value klass;
  NAPI_CALL(env, napi_define_class(env, name, sizeof(name), no_op, NULL,
                                   0, NULL, &klass));
  napi_value private_symbol;
  NAPI_CALL(env, unofficial_napi_create_private_symbol(env, name,
                                                        sizeof(name),
                                                        &private_symbol));

  CHECK_OR_FAIL(napi_create_function(env, NULL, sizeof(name), no_op, NULL,
                                      &function) == napi_invalid_arg,
                "missing explicit function name was accepted");
  CHECK_OR_FAIL(napi_define_class(env, NULL, sizeof(name), no_op, NULL,
                                   0, NULL, &klass) == napi_invalid_arg,
                "missing explicit class name was accepted");
  CHECK_OR_FAIL(unofficial_napi_create_private_symbol(env, NULL, sizeof(name),
                                                       &private_symbol) == napi_invalid_arg,
                "missing explicit private description was accepted");
  puts("EMBEDDED_NUL_NAMES_OK");
  return 0;
}
