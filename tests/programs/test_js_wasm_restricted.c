#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "node_api.h"
#include "unofficial_napi.h"

// Runs under WasmPolicy::Restricted: no realm of the env has V8's
// WebAssembly, also realms that ask for wasm code generation.

// Runs `source` in `realm`, or in the env's own realm when it is NULL.
static napi_status RunIn(napi_env env,
                         unofficial_napi_realm realm,
                         const char* source,
                         napi_value* result) {
  napi_value text = NULL;
  napi_value filename = NULL;
  napi_status status = napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &text);
  if (status != napi_ok) return status;
  status = napi_create_string_utf8(env, "wasm_restricted.js", NAPI_AUTO_LENGTH, &filename);
  if (status != napi_ok) return status;
  const unofficial_napi_js_source script = unofficial_napi_js_source_from_text(text);
  return unofficial_napi_run_script(
      env, realm, &script, filename, 0, 0, -1, true, false, false, NULL, result);
}

static bool IsText(napi_env env, napi_value value, const char* expected, const char* what) {
  char text[256] = {0};
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, text, sizeof(text), &length) != napi_ok) {
    printf("FAIL: %s did not return a string\n", what);
    return false;
  }
  if (strcmp(text, expected) != 0) {
    printf("FAIL: %s: expected '%s', got '%s'\n", what, expected, text);
    return false;
  }
  return true;
}

// Every own global whose name starts with "WebAssembly", joined.
static const char kWasmGlobals[] =
    "Object.getOwnPropertyNames(globalThis)"
    ".filter(function (name) { return /^WebAssembly/.test(name); }).join()";

// A ShadowRealm gets a fresh global from V8, not from the provider. V8 does
// not ship ShadowRealm; this fails when it does, so that path is handled then.
static const char kShadowRealm[] =
    "typeof ShadowRealm === 'undefined' ? 'undefined' : "
    "new ShadowRealm().evaluate('typeof WebAssembly')";

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  napi_value result = NULL;

  NAPI_CALL(env, RunIn(env, NULL, "typeof WebAssembly", &result));
  CHECK_OR_FAIL(IsText(env, result, "undefined", "env realm"),
                "the env's own realm has WebAssembly");
  NAPI_CALL(env, RunIn(env, NULL, kWasmGlobals, &result));
  CHECK_OR_FAIL(IsText(env, result, "", "env realm globals"),
                "the env's own realm has a WebAssembly global");
  NAPI_CALL(env, RunIn(env, NULL, kShadowRealm, &result));
  CHECK_OR_FAIL(IsText(env, result, "undefined", "ShadowRealm"),
                "a ShadowRealm has WebAssembly");

  // A realm with a fresh global that allows wasm code generation.
  unofficial_napi_realm realm = NULL;
  NAPI_CALL(env, unofficial_napi_create_realm(env, NULL, &realm, NULL));
  CHECK_OR_FAIL(realm != NULL, "create_realm did not return a realm");
  NAPI_CALL(env, RunIn(env, realm, "typeof WebAssembly", &result));
  CHECK_OR_FAIL(IsText(env, result, "undefined", "fresh realm"),
                "a realm made with default options has WebAssembly");

  // A sandbox realm, as vm.createContext(sandbox) makes.
  napi_value sandbox = NULL;
  NAPI_CALL(env, napi_create_object(env, &sandbox));
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = sandbox;
  options.allow_code_gen_wasm = true;
  unofficial_napi_realm sandbox_realm = NULL;
  NAPI_CALL(env, unofficial_napi_create_realm(env, &options, &sandbox_realm, NULL));
  CHECK_OR_FAIL(sandbox_realm != NULL, "create_realm did not return the sandbox realm");
  NAPI_CALL(env, RunIn(env, sandbox_realm, "typeof WebAssembly", &result));
  CHECK_OR_FAIL(IsText(env, result, "undefined", "sandbox realm"),
                "a sandbox realm has WebAssembly");

  puts("JS_WASM_RESTRICTED_OK");
  return 0;
}
