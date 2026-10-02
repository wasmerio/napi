#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

// Runs with the embedder's unmetered WebAssembly policy enabled: V8's
// WebAssembly must be usable in the root context and in `vm` contexts, while
// a `vm` context created with wasm code generation disabled must still refuse
// to compile.

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

static int expect_string(napi_env env, napi_value value, const char* expected,
                         const char* what) {
  char text[256] = {0};
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, text, sizeof(text), &length) !=
      napi_ok) {
    printf("FAIL: %s did not return a string\n", what);
    return 0;
  }
  if (strcmp(text, expected) != 0) {
    printf("FAIL: %s: expected '%s', got '%s'\n", what, expected, text);
    return 0;
  }
  return 1;
}

static int run_in_vm_context(napi_env env, bool allow_code_gen_wasm,
                             napi_value source, napi_value* result) {
  napi_value undefined_value;
  napi_value sandbox;
  napi_value context;
  NAPI_CALL(env, napi_get_undefined(env, &undefined_value));
  NAPI_CALL(env, napi_create_object(env, &sandbox));
  NAPI_CALL(env, unofficial_napi_contextify_make_context(
                     env, sandbox, undefined_value, undefined_value, true,
                     allow_code_gen_wasm, true, undefined_value, &context));
  const unofficial_napi_js_source context_source =
      unofficial_napi_js_source_from_text(source);
  NAPI_CALL(env, unofficial_napi_contextify_run_script(
                     env, context, &context_source, undefined_value, 0, 0,
                     -1, true, false, false, undefined_value, result));
  return 0;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  napi_value source;
  napi_value result;
  NAPI_CALL(env, napi_create_string_utf8(env, kRunModule, NAPI_AUTO_LENGTH,
                                          &source));
  NAPI_CALL(env, napi_run_script(env, source, &result));
  CHECK_OR_FAIL(expect_string(env, result, "ok", "root context"),
                "V8 WebAssembly is not usable in the root context");

  CHECK_OR_FAIL(run_in_vm_context(env, true, source, &result) == 0,
                "vm context with wasm code generation failed to run");
  CHECK_OR_FAIL(expect_string(env, result, "ok", "vm context"),
                "V8 WebAssembly is not usable in a vm context");

  CHECK_OR_FAIL(run_in_vm_context(env, false, source, &result) == 0,
                "vm context without wasm code generation failed to run");
  CHECK_OR_FAIL(
      expect_string(env, result, "refused",
                    "vm context with codeGeneration.wasm = false"),
      "a vm context compiled wasm although its code generation is disabled");

  // Realms the provider did not create carry no code-generation policy and
  // fail closed. ShadowRealm is not shipped by default; keep the check for
  // when it is.
  napi_value realm_source;
  NAPI_CALL(env,
            napi_create_string_utf8(
                env,
                "typeof ShadowRealm === 'undefined' ? 'refused' : "
                "new ShadowRealm().evaluate(\"(function () { try { new "
                "WebAssembly.Module(" ADD_MODULE_BYTES
                "); return 'compiled'; } catch (e) { return 'refused'; } "
                "})()\")",
                NAPI_AUTO_LENGTH, &realm_source));
  NAPI_CALL(env, napi_run_script(env, realm_source, &result));
  CHECK_OR_FAIL(expect_string(env, result, "refused", "ShadowRealm"),
                "a fresh realm compiled wasm without a provider policy");

  puts("JS_WASM_UNMETERED_OK");
  return 0;
}
