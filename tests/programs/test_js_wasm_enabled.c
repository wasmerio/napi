#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "node_api.h"
#include "unofficial_napi.h"

// Runs under WasmPolicy::EnabledUnmetered, the default: V8's WebAssembly
// works in the env's own realm and in realms that allow wasm code generation,
// and a realm made with allow_code_gen_wasm = false (vm's
// codeGeneration.wasm) refuses to compile.

// (module (func (export "add") (param i32 i32) (result i32)
//   local.get 0 local.get 1 i32.add))
#define ADD_MODULE_BYTES                                                     \
  "new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, "   \
  "0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, " \
  "0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09, 0x01, " \
  "0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b])"

static const char kRunModule[] =
    "(function () {\n"
    "  if (typeof WebAssembly !== 'object') return 'WebAssembly is ' +"
    "      typeof WebAssembly;\n"
    "  var bytes = " ADD_MODULE_BYTES ";\n"
    "  if (!WebAssembly.validate(bytes)) return 'module did not validate';\n"
    "  try {\n"
    "    var instance = new WebAssembly.Instance(new WebAssembly.Module(bytes));\n"
    "    var sum = instance.exports.add(40, 2);\n"
    "    if (sum !== 42) return 'wrong result ' + sum;\n"
    "    var memory = new WebAssembly.Memory({ initial: 1, maximum: 2 });\n"
    "    if (memory.buffer.byteLength !== 65536) return 'wrong memory size';\n"
    "    if (memory.grow(1) !== 1) return 'memory did not grow';\n"
    "  } catch (e) {\n"
    "    return e instanceof WebAssembly.CompileError ? 'refused' : String(e);\n"
    "  }\n"
    "  return 'ok';\n"
    "})()";

// Runs `source` in `realm`, or in the env's own realm when it is NULL.
static napi_status RunIn(napi_env env,
                         unofficial_napi_realm realm,
                         const char* source,
                         napi_value* result) {
  napi_value text = NULL;
  napi_value filename = NULL;
  napi_status status = napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &text);
  if (status != napi_ok) return status;
  status = napi_create_string_utf8(env, "wasm_enabled.js", NAPI_AUTO_LENGTH, &filename);
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

// Makes a realm with or without wasm code generation, with a sandbox (as
// vm.createContext(sandbox) does) or a fresh global.
static napi_status MakeRealm(napi_env env,
                             bool with_sandbox,
                             bool allow_code_gen_wasm,
                             unofficial_napi_realm* realm_out) {
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.allow_code_gen_wasm = allow_code_gen_wasm;
  if (with_sandbox) {
    napi_value sandbox = NULL;
    napi_status status = napi_create_object(env, &sandbox);
    if (status != napi_ok) return status;
    options.sandbox_or_null = sandbox;
  }
  return unofficial_napi_create_realm(env, &options, realm_out, NULL);
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  napi_value result = NULL;

  NAPI_CALL(env, RunIn(env, NULL, kRunModule, &result));
  CHECK_OR_FAIL(IsText(env, result, "ok", "env realm"),
                "V8 WebAssembly is not usable in the env's own realm");

  for (int with_sandbox = 0; with_sandbox <= 1; with_sandbox++) {
    unofficial_napi_realm allowed = NULL;
    NAPI_CALL(env, MakeRealm(env, with_sandbox, true, &allowed));
    NAPI_CALL(env, RunIn(env, allowed, kRunModule, &result));
    CHECK_OR_FAIL(IsText(env, result, "ok", "realm allowing wasm code generation"),
                  "V8 WebAssembly is not usable in a realm that allows it");

    unofficial_napi_realm refused = NULL;
    NAPI_CALL(env, MakeRealm(env, with_sandbox, false, &refused));
    NAPI_CALL(env, RunIn(env, refused, kRunModule, &result));
    CHECK_OR_FAIL(IsText(env, result, "refused", "realm refusing wasm code generation"),
                  "a realm compiled wasm although allow_code_gen_wasm is false");
  }

  puts("JS_WASM_ENABLED_OK");
  return 0;
}
