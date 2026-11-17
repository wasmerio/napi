#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

// argv[1] is "limited" (a budgeted or managed context) or "standalone".
// Under resource limits the host refuses tools that write or sample outside
// the budget, charges node:v8's serializer to it, does not consume a code
// cache the guest supplies, and leaves WebAssembly out by default. A
// standalone context keeps them.

static napi_value Eval(napi_env env, const char* source) {
  napi_value script = NULL;
  napi_value result = NULL;
  if (napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &script) != napi_ok) return NULL;
  if (napi_run_script(env, script, &result) != napi_ok) return NULL;
  return result;
}

static int OpenBytecode(napi_env env, napi_value source, napi_value filename,
                        const uint8_t* cache, size_t cache_length,
                        unofficial_napi_bytecode_open_result* result) {
  unofficial_napi_bytecode_open_options options;
  memset(&options, 0, sizeof options);
  options.size = sizeof options;
  options.version = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION;
  options.source_text = source;
  options.filename = filename;
  options.shape = unofficial_napi_bytecode_shape_script;
  options.cache_bytes = cache;
  options.cache_byte_length = cache_length;
  options.has_cache = cache != NULL;
  options.cache_policy = unofficial_napi_bytecode_cache_compile_on_reject;
  return unofficial_napi_bytecode_open(env, &options, result) == napi_ok;
}

static int FromHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// argv[2] (limited only): a code cache a standalone run of this guest printed.
int main(int argc, char** argv) {
  CHECK_OR_FAIL(argc >= 2, "usage: test_resource_limit_rules limited <cache hex>|standalone");
  const int limited = strcmp(argv[1], "limited") == 0;
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  // Profiler, heap snapshot, node:v8 serdes.
  unofficial_napi_profile_start_result started;
  unofficial_napi_profile profile = NULL;
  napi_status status =
      unofficial_napi_profile_start(env, unofficial_napi_profile_cpu, &started, &profile);
  printf("PROFILE_START=%d\n", (int)status);
  CHECK_OR_FAIL((status == napi_ok) == !limited, "profiler policy");
  if (status == napi_ok) {
    napi_value json;
    NAPI_CALL(env, unofficial_napi_profile_stop(env, profile, &json));
  }
  napi_value snapshot = NULL;
  status = unofficial_napi_take_heap_snapshot(env, NULL, &snapshot);
  printf("HEAP_SNAPSHOT=%d\n", (int)status);
  CHECK_OR_FAIL((status == napi_ok) == !limited, "heap snapshot policy");
  // node:v8's binding loads either way (lib/v8.js destructures it), and
  // round-trips a value.
  napi_value serdes = NULL;
  NAPI_CALL(env, unofficial_napi_create_serdes_binding(env, &serdes));
  napi_value global;
  NAPI_CALL(env, napi_get_global(env, &global));
  NAPI_CALL(env, napi_set_named_property(env, global, "serdes", serdes));
  napi_value round_trip = Eval(env,
      "var s = new serdes.Serializer(); s.writeHeader(); s.writeValue({ a: 41 });"
      "var d = new serdes.Deserializer(s.releaseBuffer()); d.readHeader(); d.readValue().a + 1");
  CHECK_OR_FAIL(round_trip != NULL, "the serdes round trip failed");
  int32_t answer = 0;
  NAPI_CALL(env, napi_get_value_int32(env, round_trip, &answer));
  CHECK_OR_FAIL(answer == 42, "the serdes round trip lost the value");
  if (limited) {
    // A serialization the budget cannot hold fails like V8's own
    // out-of-memory clone error, which JavaScript catches.
    napi_value outcome = Eval(env,
        "(function () { var big = 'x'.repeat(200000000); var s = new serdes.Serializer();"
        " s.writeHeader(); try { s.writeValue(big); return 'wrote'; }"
        " catch (e) { return String(e.message).indexOf('out of memory') >= 0 ? 'refused'"
        " : 'other: ' + e; } })()");
    CHECK_OR_FAIL(outcome != NULL, "the big serialization did not return");
    char text[96] = {0};
    size_t text_length = 0;
    NAPI_CALL(env, napi_get_value_string_utf8(env, outcome, text, sizeof text, &text_length));
    printf("SERDES_OVER_BUDGET=%s\n", text);
    CHECK_OR_FAIL(strcmp(text, "refused") == 0, "an over-budget serialization was not refused");
  }

  // A code cache the guest hands back is consumed only when standalone.
  // Under limits none is produced, and one from elsewhere is rejected.
  napi_value source;
  napi_value filename;
  NAPI_CALL(env, napi_create_string_utf8(env, "(function(){ return 6 * 7; })()",
                                         NAPI_AUTO_LENGTH, &source));
  NAPI_CALL(env, napi_create_string_utf8(env, "cache.js", NAPI_AUTO_LENGTH, &filename));
  unofficial_napi_bytecode_open_result opened;
  memset(&opened, 0, sizeof opened);
  CHECK_OR_FAIL(OpenBytecode(env, source, filename, NULL, 0, &opened), "compile failed");
  napi_value cache_array;
  NAPI_CALL(env, unofficial_napi_bytecode_serialize(env, opened.bytecode, &cache_array));
  NAPI_CALL(env, unofficial_napi_bytecode_release(env, opened.bytecode));
  napi_typedarray_type type;
  size_t length = 0;
  void* data = NULL;
  NAPI_CALL(env, napi_get_typedarray_info(env, cache_array, &type, &length, &data, NULL, NULL));
  printf("PRODUCED=%zu\n", length);
  uint8_t* cache = (uint8_t*)data;
  size_t cache_length = length;
  if (limited) {
    CHECK_OR_FAIL(length == 0, "a limited isolate produced a code cache");
    CHECK_OR_FAIL(argc == 3, "no cache to offer");
    cache_length = strlen(argv[2]) / 2;
    cache = malloc(cache_length);
    CHECK_OR_FAIL(cache != NULL, "malloc failed");
    for (size_t i = 0; i < cache_length; i++) {
      cache[i] = (uint8_t)(FromHex(argv[2][2 * i]) << 4 | FromHex(argv[2][2 * i + 1]));
    }
  } else {
    CHECK_OR_FAIL(length > 0, "no code cache was produced");
    printf("CACHE_HEX=");
    for (size_t i = 0; i < length; i++) printf("%02x", ((const uint8_t*)data)[i]);
    printf("\n");
  }
  memset(&opened, 0, sizeof opened);
  CHECK_OR_FAIL(OpenBytecode(env, source, filename, cache, cache_length, &opened),
                "open with a cache failed");
  printf("CACHE_REJECTED=%d\n", (int)opened.cache_rejected);
  CHECK_OR_FAIL(opened.bytecode != NULL, "no bytecode");
  CHECK_OR_FAIL(opened.cache_rejected == (limited ? 1 : 0), "code cache policy");
  NAPI_CALL(env, unofficial_napi_bytecode_release(env, opened.bytecode));
  if (limited) free(cache);

  // A sparse parameter list of 2^32 - 1 entries fails without the host
  // reserving room for all of them.
  napi_value params = Eval(env, "new Array(2 ** 32 - 1)");
  CHECK_OR_FAIL(params != NULL, "could not make the params array");
  napi_value body;
  NAPI_CALL(env, napi_create_string_utf8(env, "return 1", NAPI_AUTO_LENGTH, &body));
  const unofficial_napi_js_source function_source = unofficial_napi_js_source_from_text(body);
  napi_value undefined_value;
  NAPI_CALL(env, napi_get_undefined(env, &undefined_value));
  napi_value compiled = NULL;
  status = unofficial_napi_compile_function(env, NULL, &function_source, filename, 0, 0,
                                            undefined_value, params, undefined_value, &compiled);
  printf("SPARSE_PARAMS=%d\n", (int)status);
  CHECK_OR_FAIL(status != napi_ok, "a sparse params array was accepted");

  // More parameters than V8 compiles: refused before any is read.
  params = Eval(env, "Array.from({ length: 65526 }, function (_, i) { return 'p' + i; })");
  CHECK_OR_FAIL(params != NULL, "could not make the long params array");
  status = unofficial_napi_compile_function(env, NULL, &function_source, filename, 0, 0,
                                            undefined_value, params, undefined_value, &compiled);
  printf("TOO_MANY_PARAMS=%d\n", (int)status);
  CHECK_OR_FAIL(status == napi_invalid_arg, "too many params were accepted");
  // A getter that keeps growing the array cannot extend the conversion.
  params = Eval(env,
      "(function () { var a = ['a']; Object.defineProperty(a, 0, { get: function () {"
      " a.push('b'); return 'a'; } }); return a; })()");
  CHECK_OR_FAIL(params != NULL, "could not make the growing params array");
  status = unofficial_napi_compile_function(env, NULL, &function_source, filename, 0, 0,
                                            undefined_value, params, undefined_value, &compiled);
  printf("GROWING_PARAMS=%d\n", (int)status);
  CHECK_OR_FAIL(status == napi_ok, "a one-element params array failed");

  // WebAssembly: out by default under limits.
  napi_value kind = Eval(env, "typeof WebAssembly");
  CHECK_OR_FAIL(kind != NULL, "typeof WebAssembly failed");
  char text[32] = {0};
  NAPI_CALL(env, napi_get_value_string_utf8(env, kind, text, sizeof text, &length));
  printf("WEBASSEMBLY=%s\n", text);
  CHECK_OR_FAIL(strcmp(text, limited ? "undefined" : "object") == 0, "WebAssembly default");

  puts("RESOURCE_LIMIT_RULES_OK");
  return 0;
}
