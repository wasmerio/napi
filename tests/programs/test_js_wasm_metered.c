#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"

// Runs with the embedder's metered WebAssembly policy: WebAssembly works,
// a memory.grow bomb and a huge memory are refused softly (the script keeps
// running), and a compile bomb past the context's code budget stops the
// JavaScript.

static const char kSoftDenials[] =
    "(function () {\n"
    "  if (typeof WebAssembly !== 'object') return 'WebAssembly is ' +\n"
    "      typeof WebAssembly;\n"
    "  var add = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00,\n"
    "    0x00, 0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x03,\n"
    "    0x02, 0x01, 0x00, 0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00,\n"
    "    0x00, 0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a,\n"
    "    0x0b]);\n"
    "  var sum = new WebAssembly.Instance(new WebAssembly.Module(add))\n"
    "      .exports.add(40, 2);\n"
    "  if (sum !== 42) return 'wrong result ' + sum;\n"
    "  var memory = new WebAssembly.Memory({ initial: 1 });\n"
    "  var grows = 0;\n"
    "  try { for (;;) { memory.grow(16); grows++; } }\n"
    "  catch (e) { if (!(e instanceof RangeError)) return String(e); }\n"
    "  if (grows === 0) return 'memory never grew';\n"
    "  try { new WebAssembly.Memory({ initial: 16384 }); return 'huge'; }\n"
    "  catch (e) { if (!(e instanceof RangeError)) return String(e); }\n"
    "  globalThis.keep = memory;\n"
    "  return 'ok';\n"
    "})()";

// Builds a module with 512 exported functions, each chaining 400 bounds
// checked loads (about 12 KiB of baseline code apiece), and calls them all.
static const char kCodeBomb[] =
    "(function () {\n"
    "  function leb(o, n) { do { var b = n & 0x7f; n = Math.floor(n / 128);\n"
    "    if (n) b |= 0x80; o.push(b); } while (n); }\n"
    "  function sec(o, id, body) { o.push(id); leb(o, body.length);\n"
    "    for (var i = 0; i < body.length; i++) o.push(body[i]); }\n"
    "  var count = 512, out = [0, 0x61, 0x73, 0x6d, 1, 0, 0, 0];\n"
    "  sec(out, 1, [1, 0x60, 1, 0x7f, 1, 0x7f]);\n"
    "  var f = []; leb(f, count); for (var i = 0; i < count; i++) f.push(0);\n"
    "  sec(out, 3, f);\n"
    "  sec(out, 5, [1, 0, 1]);\n"
    "  var e = []; leb(e, count);\n"
    "  for (var i = 0; i < count; i++) { var n = 'f' + i; leb(e, n.length);\n"
    "    for (var j = 0; j < n.length; j++) e.push(n.charCodeAt(j));\n"
    "    e.push(0); leb(e, i); }\n"
    "  sec(out, 7, e);\n"
    "  var body = [0, 0x20, 0];\n"
    "  for (var i = 0; i < 400; i++) body.push(0x28, 2, 0);\n"
    "  body.push(0x0b);\n"
    "  var c = []; leb(c, count);\n"
    "  for (var i = 0; i < count; i++) { leb(c, body.length);\n"
    "    for (var j = 0; j < body.length; j++) c.push(body[j]); }\n"
    "  sec(out, 10, c);\n"
    "  var exports = new WebAssembly.Instance(\n"
    "      new WebAssembly.Module(new Uint8Array(out))).exports;\n"
    "  for (var i = 0; i < count; i++) exports['f' + i](0);\n"
    "  return 'not stopped';\n"
    "})()";

static int run(napi_env env, const char* script, napi_value* result) {
  napi_value source;
  NAPI_CALL(env, napi_create_string_utf8(env, script, NAPI_AUTO_LENGTH,
                                          &source));
  return (int)napi_run_script(env, source, result);
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  napi_value result;
  CHECK_OR_FAIL(run(env, kSoftDenials, &result) == napi_ok,
                "the soft-denial script threw");
  char text[256] = {0};
  size_t length = 0;
  NAPI_CALL(env, napi_get_value_string_utf8(env, result, text, sizeof(text),
                                            &length));
  CHECK_OR_FAIL(strcmp(text, "ok") == 0, text);
  puts("WASM_SOFT_DENIALS_OK");
  fflush(stdout);

  int status = run(env, kCodeBomb, &result);
  printf("CODE_BOMB_STATUS=%d\n", status);
  return 0;
}
