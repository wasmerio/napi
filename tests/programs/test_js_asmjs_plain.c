#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"

// V8 translates validated asm.js modules to WebAssembly and compiles them
// with its wasm pipeline, without consulting the embedder's wasm
// code-generation policy. The provider disables that translation, so an asm.js
// module must run as ordinary JavaScript.
//
// The probe distinguishes the two by inspecting the asm.js function's stack
// frame from a foreign function it calls: in sloppy-mode JavaScript,
// CallSite.getFunction() returns the very function object that is running; a
// frame of translated asm.js code is a wasm frame and never yields that
// function.
static const char kScript[] =
    "(function () {\n"
    "  var observed = 'no frame';\n"
    "  function probe() {\n"
    "    var saved = Error.prepareStackTrace;\n"
    "    Error.prepareStackTrace = function (_error, frames) { return frames; };\n"
    "    var frames = new Error().stack;\n"
    "    Error.prepareStackTrace = saved;\n"
    "    for (var i = 0; i < frames.length; i++) {\n"
    "      if (frames[i].getFunctionName() === 'asmAdd') {\n"
    "        observed = frames[i].getFunction();\n"
    "        break;\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  function AsmModule(stdlib, foreign) {\n"
    "    'use asm';\n"
    "    var probe = foreign.probe;\n"
    "    function asmAdd(a, b) {\n"
    "      a = a | 0;\n"
    "      b = b | 0;\n"
    "      probe();\n"
    "      return (a + b) | 0;\n"
    "    }\n"
    "    return { asmAdd: asmAdd };\n"
    "  }\n"
    "  var exports = AsmModule(globalThis, { probe: probe });\n"
    "  var sum = exports.asmAdd(40, 2);\n"
    "  if (sum !== 42) return 'wrong result ' + sum;\n"
    "  if (observed === exports.asmAdd) return 'plain';\n"
    "  return 'translated (frame function: ' + typeof observed + ')';\n"
    "})()";

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  napi_value source;
  NAPI_CALL(env, napi_create_string_utf8(env, kScript, NAPI_AUTO_LENGTH,
                                          &source));
  napi_value result;
  NAPI_CALL(env, napi_run_script(env, source, &result));

  char text[128] = {0};
  size_t length = 0;
  NAPI_CALL(env, napi_get_value_string_utf8(env, result, text, sizeof(text),
                                            &length));
  if (strcmp(text, "plain") != 0) {
    printf("FAIL: asm.js module did not run as plain JavaScript: %s\n", text);
    return 1;
  }
  puts("ASMJS_RUNS_AS_PLAIN_JS_OK");
  return 0;
}
