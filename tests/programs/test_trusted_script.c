// unofficial_napi_create_trusted_script and unofficial_napi_is_executing_js
// through the guest imports.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

static napi_value MakeTrusted(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value source = NULL;
  napi_value result = NULL;
  if (napi_get_cb_info(env, info, &argc, &source, NULL, NULL) != napi_ok ||
      argc != 1 ||
      unofficial_napi_create_trusted_script(env, source, &result) != napi_ok) {
    napi_throw_type_error(env, NULL, "Expected a primitive string");
    return NULL;
  }
  return result;
}

static napi_value Executing(napi_env env, napi_callback_info info) {
  (void)info;
  bool active = false;
  napi_value result = NULL;
  if (unofficial_napi_is_executing_js(env, &active) != napi_ok ||
      napi_get_boolean(env, active, &result) != napi_ok) {
    return NULL;
  }
  return result;
}

static bool ScriptIsTrue(napi_env env, const char* code) {
  napi_value script = NULL;
  napi_value result = NULL;
  bool value = false;
  if (napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &script) !=
          napi_ok ||
      napi_run_script(env, script, &result) != napi_ok ||
      napi_get_value_bool(env, result, &value) != napi_ok) {
    bool pending = false;
    napi_value exception = NULL;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_get_and_clear_last_exception(env, &exception);
    }
    return false;
  }
  return value;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  napi_value global = NULL;
  napi_value function = NULL;
  NAPI_CALL(env, napi_get_global(env, &global));
  NAPI_CALL(env, napi_create_function(env, "makeTrusted", NAPI_AUTO_LENGTH,
                                      MakeTrusted, NULL, &function));
  NAPI_CALL(env, napi_set_named_property(env, global, "makeTrusted", function));
  NAPI_CALL(env, napi_create_function(env, "executing", NAPI_AUTO_LENGTH,
                                      Executing, NULL, &function));
  NAPI_CALL(env, napi_set_named_property(env, global, "executing", function));

  bool active = true;
  NAPI_CALL(env, unofficial_napi_is_executing_js(env, &active));
  CHECK_OR_FAIL(!active, "no JavaScript is executing at top level");
  CHECK_OR_FAIL(ScriptIsTrue(env, "executing() === true"),
                "a native callback runs inside JavaScript");

  CHECK_OR_FAIL(
      ScriptIsTrue(env, "(() => { let lexical = 40; "
                        "return eval(makeTrusted('lexical + 2')) === 42; })()"),
      "direct eval of a trusted script keeps lexical scope");
  CHECK_OR_FAIL(
      ScriptIsTrue(env, "(() => { let lexical = 23; "
                        "const source = makeTrusted('lexical'); "
                        "source.toString = () => { throw 1; }; "
                        "return eval(source) === 23; })()"),
      "direct eval ignores an author toString");
  CHECK_OR_FAIL(
      ScriptIsTrue(env, "(() => { const copy = Object.create(makeTrusted('1')); "
                        "return eval(copy) === copy; })()"),
      "the brand is not inherited");

  // Argument validation.
  napi_value number = NULL;
  napi_value source = NULL;
  napi_value result = NULL;
  NAPI_CALL(env, napi_create_uint32(env, 1, &number));
  NAPI_CALL(env, napi_create_string_utf8(env, "1", NAPI_AUTO_LENGTH, &source));
  result = number;
  CHECK_OR_FAIL(unofficial_napi_create_trusted_script(env, number, &result) ==
                        napi_string_expected &&
                    result == NULL,
                "a non-string source is rejected");
  CHECK_OR_FAIL(unofficial_napi_create_trusted_script(env, source, NULL) ==
                    napi_invalid_arg,
                "null result pointer is rejected");
  CHECK_OR_FAIL(unofficial_napi_is_executing_js(env, NULL) == napi_invalid_arg,
                "null result pointer is rejected");
#ifdef __wasm__
  // Results outside guest memory fail with napi_invalid_arg.
  CHECK_OR_FAIL(unofficial_napi_create_trusted_script(
                    env, source, (napi_value*)(uintptr_t)0xfffffffeu) ==
                    napi_invalid_arg,
                "out-of-bounds result pointer is rejected");
  CHECK_OR_FAIL(unofficial_napi_is_executing_js(
                    env, (bool*)(uintptr_t)0xffffffffu) == napi_invalid_arg,
                "out-of-bounds result pointer is rejected");
#endif

  NAPI_CALL(env, unofficial_napi_create_trusted_script(env, source, &result));
  napi_valuetype type = napi_undefined;
  NAPI_CALL(env, napi_typeof(env, result, &type));
  CHECK_OR_FAIL(type == napi_object, "a trusted script is an object");

  return PrintSuccess("TEST_TRUSTED_SCRIPT");
}
