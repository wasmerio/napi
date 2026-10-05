#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

static int check_undefined(napi_env env, napi_value result) {
  char text[16] = {0};
  size_t length = 0;
  if (napi_get_value_string_utf8(env, result, text, sizeof(text), &length) !=
      napi_ok) {
    return 0;
  }
  return length == strlen("undefined") && strcmp(text, "undefined") == 0;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  napi_value source;
  NAPI_CALL(env, napi_create_string_utf8(env, "typeof WebAssembly",
                                          NAPI_AUTO_LENGTH, &source));
  napi_value result;
  NAPI_CALL(env, napi_run_script(env, source, &result));
  CHECK_OR_FAIL(check_undefined(env, result),
                "V8 WebAssembly is available in the root context");

  // A fresh realm would reinstate the constructor if V8 ever enables
  // ShadowRealm by default. Keep that escape path in this regression.
  napi_value realm_source;
  NAPI_CALL(env, napi_create_string_utf8(
                     env,
                     "typeof ShadowRealm === 'undefined' ? 'undefined' : "
                     "new ShadowRealm().evaluate('typeof WebAssembly')",
                     NAPI_AUTO_LENGTH, &realm_source));
  NAPI_CALL(env, napi_run_script(env, realm_source, &result));
  CHECK_OR_FAIL(check_undefined(env, result),
                "V8 WebAssembly is available through a fresh realm");

  napi_value undefined_value;
  napi_value sandbox;
  napi_value context;
  NAPI_CALL(env, napi_get_undefined(env, &undefined_value));
  NAPI_CALL(env, napi_create_object(env, &sandbox));
  NAPI_CALL(env, unofficial_napi_contextify_make_context(
                     env, sandbox, undefined_value, undefined_value, true,
                     true, true, undefined_value, &context));
  const unofficial_napi_js_source context_source =
      unofficial_napi_js_source_from_text(source);
  NAPI_CALL(env, unofficial_napi_contextify_run_script(
                     env, context, &context_source, undefined_value, 0, 0,
                     -1, true, false, false, undefined_value, &result));
  CHECK_OR_FAIL(check_undefined(env, result),
                "V8 WebAssembly is available in a vm context");
  puts("JS_WASM_DISABLED_OK");
  return 0;
}
