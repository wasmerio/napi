// Exercise the execution-state import without depending on trusted scripts.
#include <stdbool.h>
#include <stdint.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

static napi_value Executing(napi_env env, napi_callback_info info) {
  (void)info;
  bool active = false;
  napi_value result = NULL;
  if (unofficial_napi_is_executing_js(env, &active) != napi_ok ||
      napi_get_boolean(env, active, &result) != napi_ok) return NULL;
  return result;
}

static bool ScriptIsTrue(napi_env env, const char* text) {
  napi_value source = NULL, result = NULL;
  bool value = false;
  return napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &source) == napi_ok &&
         napi_run_script(env, source, &result) == napi_ok &&
         napi_get_value_bool(env, result, &value) == napi_ok && value;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "env is initialized");
  bool active = true;
  NAPI_CALL(env, unofficial_napi_is_executing_js(env, &active));
  CHECK_OR_FAIL(!active, "idle C entry reports false");
  CHECK_OR_FAIL(unofficial_napi_is_executing_js(env, NULL) == napi_invalid_arg,
                "null result pointer is rejected");
#ifdef __wasm__
  CHECK_OR_FAIL(unofficial_napi_is_executing_js(
                    env, (bool*)(uintptr_t)0xffffffffu) == napi_invalid_arg,
                "out-of-bounds result pointer is rejected");
#endif

  napi_value global = NULL, callback = NULL, result = NULL;
  NAPI_CALL(env, napi_get_global(env, &global));
  NAPI_CALL(env, napi_create_function(env, "executing", NAPI_AUTO_LENGTH,
                                      Executing, NULL, &callback));
  NAPI_CALL(env, napi_set_named_property(env, global, "executing", callback));
  CHECK_OR_FAIL(ScriptIsTrue(env, "executing() === true"),
                "JavaScript caller reports true");
  NAPI_CALL(env, napi_call_function(env, global, callback, 0, NULL, &result));
  NAPI_CALL(env, napi_get_value_bool(env, result, &active));
  CHECK_OR_FAIL(!active, "direct C-to-native call reports false");

  CHECK_OR_FAIL(ScriptIsTrue(env,
      "globalThis.jobActive = false; "
      "Promise.resolve().then(executing).then(value => jobActive = value); true"),
      "promise jobs are queued");
  NAPI_CALL(env, unofficial_napi_event_loop_checkpoint(
      env, unofficial_napi_event_loop_checkpoint_microtasks, false, NULL));
  CHECK_OR_FAIL(ScriptIsTrue(env, "jobActive === true"),
                "native promise callback reports true");

  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  unofficial_napi_realm realm = NULL;
  napi_value realm_global = NULL;
  NAPI_CALL(env, unofficial_napi_create_realm(env, &options, UNOFFICIAL_NAPI_REALM_MODE_FRESH, 0, NULL, &realm, &realm_global, NULL));
  NAPI_CALL(env, napi_set_named_property(env, realm_global, "executing", callback));
  unofficial_napi_realm_scope scope = 0;
  NAPI_CALL(env, unofficial_napi_enter_realm(env, realm, &scope));
  NAPI_CALL(env, unofficial_napi_is_executing_js(env, &active));
  CHECK_OR_FAIL(!active, "entering a realm alone reports false");
  CHECK_OR_FAIL(ScriptIsTrue(env, "executing() === true"),
                "callback sees JavaScript in another realm");
  NAPI_CALL(env, unofficial_napi_leave_realm(env, scope));

  napi_value source = NULL, exception = NULL;
  NAPI_CALL(env, napi_create_string_utf8(env, "throw 42", NAPI_AUTO_LENGTH, &source));
  CHECK_OR_FAIL(napi_run_script(env, source, &result) == napi_pending_exception,
                "script throws");
  NAPI_CALL(env, unofficial_napi_is_executing_js(env, &active));
  CHECK_OR_FAIL(!active, "throwing script has unwound");
  bool pending = false;
  NAPI_CALL(env, napi_is_exception_pending(env, &pending));
  CHECK_OR_FAIL(pending, "query preserves pending exception");
  NAPI_CALL(env, napi_get_and_clear_last_exception(env, &exception));
  int32_t thrown = 0;
  NAPI_CALL(env, napi_get_value_int32(env, exception, &thrown));
  CHECK_OR_FAIL(thrown == 42, "query preserves exception value");
  return PrintSuccess("TEST_EXECUTION_STATE");
}
