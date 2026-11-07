// Public native SDK cancellation fixture. Deliberately absent from manifest.json:
// the native fixture harness has no owner that could stop this infinite loop.
// The marker comes from a callback reached inside the evaluated JavaScript loop,
// not from C before napi_run_script. It proves loop entry, not the exact V8
// instruction running when the host later requests cancellation.
#include <stdio.h>

#include "napi_test_helpers.h"

static napi_value entered_callback(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value undefined;
  if (napi_get_undefined(env, &undefined) != napi_ok) return NULL;
  printf("SDK_JS_ACTIVE_LOOP_ENTERED\n");
  fflush(stdout);
  return undefined;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  napi_value global;
  napi_value entered;
  NAPI_CALL(env, napi_get_global(env, &global));
  NAPI_CALL(env, napi_create_function(env, "sdkEntered", NAPI_AUTO_LENGTH,
                                     entered_callback, NULL, &entered));
  NAPI_CALL(env, napi_set_named_property(env, global, "sdkEntered", entered));
  napi_value script;
  NAPI_CALL(env,
            napi_create_string_utf8(
                env,
                "for (let i = 0; ; i++) { if (i === 100000) sdkEntered(); }",
                NAPI_AUTO_LENGTH, &script));
  napi_value result;
  napi_status status = napi_run_script(env, script, &result);
  // The SDK may stop the guest before any continuation after the evaluation.
  // A successful return would be a fixture error, never the cancellation signal.
  CHECK_OR_FAIL(status != napi_ok, "infinite JavaScript unexpectedly returned");
  return 0;
}
