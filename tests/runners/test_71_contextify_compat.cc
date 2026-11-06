// The deprecated API, kept for compatibility: the contextify calls, the
// context-token hooks and version 1 of the option structs, on top of realms.
// The first half is main's test_65_unofficial_contextify.cc, unchanged but
// for the fixture name, so code written against main keeps passing it.
#define UNOFFICIAL_NAPI_NO_DEPRECATION_WARNINGS
#ifdef NAPI_MODULE
#undef NAPI_MODULE
#endif
#include "node_api.h"

#include "test_env.h"

#include "napi_testing.h"
#include "unofficial_napi.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

class Test71ContextifyCompat : public FixtureTestBase {};

namespace {

napi_value Str(napi_env env, const char* value) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out) != napi_ok) return nullptr;
  return out;
}

napi_value Sym(napi_env env, const char* value) {
  napi_value desc = Str(env, value);
  if (desc == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_create_symbol(env, desc, &out) != napi_ok) return nullptr;
  return out;
}

#if defined(NAPI_TEST_ENGINE_V8)
constexpr char kPreparedStack[] =
    "Error: sentinel\n"
    "    at process.processTicksAndRejections (node:internal/process/task_queues:85:11)\n"
    "    at triggerUncaughtException (node:internal/process/promises:251:13)";

napi_value ReturnPreparedStack(napi_env env, napi_callback_info /*info*/) {
  return Str(env, kPreparedStack);
}

napi_value CaptureDynamicImportId(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
      argc < 1 || argv[0] == nullptr) {
    return nullptr;
  }

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr ||
      napi_set_named_property(env, global, "__captured_dynamic_import_id", argv[0]) != napi_ok) {
    return nullptr;
  }

  napi_deferred deferred = nullptr;
  napi_value promise = nullptr;
  napi_value undefined = nullptr;
  if (napi_create_promise(env, &deferred, &promise) != napi_ok ||
      napi_get_undefined(env, &undefined) != napi_ok ||
      napi_resolve_deferred(env, deferred, undefined) != napi_ok) {
    return nullptr;
  }
  return promise;
}
#endif

}  // namespace

TEST_F(Test71ContextifyCompat, MakeRunRoundTrip) {
  EnvScope s(runtime_.get());

  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);

  napi_value result = nullptr;
  ASSERT_EQ(unofficial_napi_contextify_make_context(s.env,
                                                    sandbox,
                                                    Str(s.env, "ctx"),
                                                    Str(s.env, "test://origin"),
                                                    true,
                                                    true,
                                                    true,
                                                    Sym(s.env, "hdo"),
                                                    &result),
            napi_ok);
  ASSERT_NE(result, nullptr);

  napi_value eval_result = nullptr;
  const unofficial_napi_js_source run_source =
      unofficial_napi_js_source_from_text(Str(s.env, "globalThis.answer = 42; answer"));
  ASSERT_EQ(unofficial_napi_contextify_run_script(s.env,
                                                  sandbox,
                                                  &run_source,
                                                  Str(s.env, "ctx.js"),
                                                  0,
                                                  0,
                                                  -1,
                                                  true,
                                                  false,
                                                  false,
                                                  Sym(s.env, "hdo"),
                                                  &eval_result),
            napi_ok);
  ASSERT_NE(eval_result, nullptr);

  int32_t answer = 0;
  ASSERT_EQ(napi_get_value_int32(s.env, eval_result, &answer), napi_ok);
  EXPECT_EQ(answer, 42);

  napi_value answer_value = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, sandbox, "answer", &answer_value), napi_ok);
  ASSERT_EQ(napi_get_value_int32(s.env, answer_value, &answer), napi_ok);
  EXPECT_EQ(answer, 42);

}

#if defined(NAPI_TEST_ENGINE_V8)
TEST_F(Test71ContextifyCompat, MakeContextPreservesThrownProxyException) {
  EnvScope s(runtime_.get());

  napi_value source = Str(
      s.env,
      "(() => { const sentinel = {}; return { sentinel, sandbox: new Proxy({}, { ownKeys() { throw sentinel; } }) }; })()");
  ASSERT_NE(source, nullptr);
  napi_value fixture = nullptr;
  ASSERT_EQ(napi_run_script(s.env, source, &fixture), napi_ok);
  ASSERT_NE(fixture, nullptr);

  napi_value sentinel = nullptr;
  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, fixture, "sentinel", &sentinel), napi_ok);
  ASSERT_EQ(napi_get_named_property(s.env, fixture, "sandbox", &sandbox), napi_ok);

  napi_value result = nullptr;
  EXPECT_EQ(unofficial_napi_contextify_make_context(s.env,
                                                    sandbox,
                                                    Str(s.env, "ctx"),
                                                    Str(s.env, "test://origin"),
                                                    true,
                                                    true,
                                                    false,
                                                    Sym(s.env, "hdo"),
                                                    &result),
            napi_pending_exception);
  EXPECT_EQ(result, nullptr);

  bool pending = false;
  ASSERT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
  EXPECT_TRUE(pending);
  napi_value caught = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &caught), napi_ok);
  bool same = false;
  ASSERT_EQ(napi_strict_equals(s.env, caught, sentinel, &same), napi_ok);
  EXPECT_TRUE(same);
}
#endif

TEST_F(Test71ContextifyCompat, SandboxGlobalThisIsNotEnumerableForDeepFreeze) {
  EnvScope s(runtime_.get());

  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);

  napi_value result = nullptr;
  ASSERT_EQ(unofficial_napi_contextify_make_context(s.env,
                                                    sandbox,
                                                    Str(s.env, "ctx"),
                                                    Str(s.env, "test://origin"),
                                                    true,
                                                    true,
                                                    true,
                                                    Sym(s.env, "hdo"),
                                                    &result),
            napi_ok);
  ASSERT_NE(result, nullptr);

#if defined(NAPI_TEST_ENGINE_QUICKJS)
  // QuickJS keeps its context marker on the sandbox rather than copying it
  // into the context global. Pin the host-visible property contract directly.
  napi_value marker_key = Str(s.env, "__quickjs_contextified");
  bool has_marker = false;
  ASSERT_EQ(napi_has_own_property(s.env, sandbox, marker_key, &has_marker), napi_ok);
  EXPECT_TRUE(has_marker);

  napi_value enumerable_keys = nullptr;
  ASSERT_EQ(napi_get_all_property_names(s.env,
                                        sandbox,
                                        napi_key_own_only,
                                        napi_key_enumerable,
                                        napi_key_numbers_to_strings,
                                        &enumerable_keys),
            napi_ok);
  uint32_t key_count = 0;
  ASSERT_EQ(napi_get_array_length(s.env, enumerable_keys, &key_count), napi_ok);
  for (uint32_t index = 0; index < key_count; ++index) {
    napi_value key = nullptr;
    ASSERT_EQ(napi_get_element(s.env, enumerable_keys, index, &key), napi_ok);
    char text[64] = {};
    size_t copied = 0;
    ASSERT_EQ(napi_get_value_string_utf8(s.env, key, text, sizeof(text), &copied), napi_ok);
    EXPECT_STRNE(text, "__quickjs_contextified");
  }

  napi_value marker_false = nullptr;
  napi_value marker_true = nullptr;
  ASSERT_EQ(napi_get_boolean(s.env, false, &marker_false), napi_ok);
  ASSERT_EQ(napi_get_boolean(s.env, true, &marker_true), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, sandbox, "__quickjs_contextified", marker_false), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, sandbox, "__quickjs_contextified", marker_true), napi_ok);
#endif

  napi_value eval_result = nullptr;
  std::string freeze_script = R"JS(
const globalThisDescriptor = Object.getOwnPropertyDescriptor(globalThis, "globalThis");
if (!globalThisDescriptor || globalThisDescriptor.enumerable ||
    !globalThisDescriptor.writable || !globalThisDescriptor.configurable) {
  throw new Error("globalThis should be writable/configurable but non-enumerable");
}
const keys = Object.keys(globalThis);
if (keys.includes("globalThis")) {
  throw new Error("contextify internals should not be enumerable");
}
)JS";
  freeze_script += R"JS(
globalThis.__RSC_MANIFEST = {};
globalThis.__RSC_MANIFEST["/x"] = { ok: true };
function deepFreeze(obj) {
  if (obj === null || typeof obj !== "object" || Object.isFrozen(obj)) {
    return obj;
  }
  for (const value of Object.values(obj)) {
    deepFreeze(value);
  }
  return Object.freeze(obj);
}
deepFreeze(globalThis);
globalThis.__RSC_MANIFEST["/x"].ok;
)JS";
  const unofficial_napi_js_source freeze_source = unofficial_napi_js_source_from_text(
      Str(s.env, freeze_script.c_str()));
  ASSERT_EQ(unofficial_napi_contextify_run_script(s.env,
                                                  sandbox,
                                                  &freeze_source,
                                                  Str(s.env, "deep_freeze.js"),
                                                  0,
                                                  0,
                                                  -1,
                                                  true,
                                                  false,
                                                  false,
                                                  Sym(s.env, "hdo"),
                                                  &eval_result),
            napi_ok);
  ASSERT_NE(eval_result, nullptr);

  bool ok = false;
  ASSERT_EQ(napi_get_value_bool(s.env, eval_result, &ok), napi_ok);
  EXPECT_TRUE(ok);
}

TEST_F(Test71ContextifyCompat, CompileFunctionAndCachedData) {
  EnvScope s(runtime_.get());

  napi_value params = nullptr;
  ASSERT_EQ(napi_create_array_with_length(s.env, 2, &params), napi_ok);
  ASSERT_EQ(napi_set_element(s.env, params, 0, Str(s.env, "a")), napi_ok);
  ASSERT_EQ(napi_set_element(s.env, params, 1, Str(s.env, "b")), napi_ok);

  napi_value context_extensions = nullptr;
  ASSERT_EQ(napi_create_array_with_length(s.env, 0, &context_extensions), napi_ok);

  napi_value undef = nullptr;
  ASSERT_EQ(napi_get_undefined(s.env, &undef), napi_ok);

  napi_value out = nullptr;
  const unofficial_napi_js_source fn_source =
      unofficial_napi_js_source_from_text(Str(s.env, "return a + b;"));
  ASSERT_EQ(unofficial_napi_contextify_compile_function(s.env,
                                                        &fn_source,
                                                        Str(s.env, "fn.js"),
                                                        0,
                                                        0,
                                                        undef,
                                                        context_extensions,
                                                        params,
                                                        Sym(s.env, "hdo"),
                                                        &out),
            napi_ok);
  ASSERT_NE(out, nullptr);

  napi_value fn = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, out, "function", &fn), napi_ok);
  ASSERT_NE(fn, nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  napi_value argv[2] = {nullptr, nullptr};
  ASSERT_EQ(napi_create_int32(s.env, 2, &argv[0]), napi_ok);
  ASSERT_EQ(napi_create_int32(s.env, 3, &argv[1]), napi_ok);

  napi_value fn_result = nullptr;
  ASSERT_EQ(napi_call_function(s.env, global, fn, 2, argv, &fn_result), napi_ok);
  int32_t sum = 0;
  ASSERT_EQ(napi_get_value_int32(s.env, fn_result, &sum), napi_ok);
  EXPECT_EQ(sum, 5);

  // Cached data now flows through the bytecode handle APIs: compile eagerly,
  // serialize the engine bytes, and restore a live artifact from them.
  unofficial_napi_bytecode_open_options open_options{};
  open_options.size = sizeof(open_options);
  open_options.version = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION;
  open_options.source_text = Str(s.env, "1 + 1");
  open_options.filename = Str(s.env, "script.js");
  open_options.shape = unofficial_napi_bytecode_shape_script;
  open_options.params_or_undefined = undef;
  open_options.host_defined_option_id = Sym(s.env, "hdo");
  unofficial_napi_bytecode_open_result open_result{};
  ASSERT_EQ(unofficial_napi_bytecode_open(s.env, &open_options, &open_result), napi_ok);
  unofficial_napi_bytecode bytecode = open_result.bytecode;
  ASSERT_NE(bytecode, nullptr);

  napi_value cached_data = nullptr;
  ASSERT_EQ(unofficial_napi_bytecode_serialize(s.env, bytecode, &cached_data), napi_ok);
  ASSERT_NE(cached_data, nullptr);
  bool is_typedarray = false;
  ASSERT_EQ(napi_is_typedarray(s.env, cached_data, &is_typedarray), napi_ok);
  EXPECT_TRUE(is_typedarray);
  ASSERT_EQ(unofficial_napi_bytecode_release(s.env, bytecode), napi_ok);

  const uint8_t* bytes = nullptr;
  size_t byte_length = 0;
  napi_typedarray_type array_type = napi_uint8_array;
  napi_value arraybuffer = nullptr;
  size_t byte_offset = 0;
  void* data = nullptr;
  ASSERT_EQ(napi_get_typedarray_info(s.env, cached_data, &array_type, &byte_length, &data,
                                     &arraybuffer, &byte_offset),
            napi_ok);
  ASSERT_GT(byte_length, 0u);
  bytes = static_cast<const uint8_t*>(data);

  open_options.cache_bytes = bytes;
  open_options.cache_byte_length = byte_length;
  open_options.has_cache = 1;
  open_result = {};
  ASSERT_EQ(unofficial_napi_bytecode_open(s.env, &open_options, &open_result), napi_ok);
  EXPECT_EQ(open_result.cache_rejected, 0);
  unofficial_napi_bytecode restored = open_result.bytecode;
  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(unofficial_napi_bytecode_release(s.env, restored), napi_ok);

  open_options.source_text = Str(s.env, "1000 + 2000 + 3000");
  open_options.cache_policy = unofficial_napi_bytecode_cache_validate_only;
  open_result = {};
  ASSERT_EQ(unofficial_napi_bytecode_open(s.env, &open_options, &open_result), napi_ok);
  EXPECT_EQ(open_result.cache_rejected, 1);
  EXPECT_EQ(open_result.bytecode, nullptr);

  // A present-but-empty cache is a cache miss, not a malformed transaction.
  // The provider reports the rejection and atomically prepares the fallback
  // artifact from source so callers never need a deserialize/compile branch.
  open_options.cache_bytes = nullptr;
  open_options.cache_byte_length = 0;
  open_options.has_cache = 1;
  open_options.cache_policy = unofficial_napi_bytecode_cache_compile_on_reject;
  open_result = {};
  ASSERT_EQ(unofficial_napi_bytecode_open(s.env, &open_options, &open_result), napi_ok);
  EXPECT_EQ(open_result.cache_rejected, 1);
  ASSERT_NE(open_result.bytecode, nullptr);
  ASSERT_EQ(unofficial_napi_bytecode_release(s.env, open_result.bytecode), napi_ok);
}

TEST_F(Test71ContextifyCompat, ModuleStateIsOneAtomicSnapshot) {
  EnvScope s(runtime_.get());

  napi_value wrapper = nullptr;
  napi_value undefined = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &wrapper), napi_ok);
  ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
  const unofficial_napi_js_source source =
      unofficial_napi_js_source_from_text(Str(s.env, "export const value = 42;"));
  unofficial_napi_module_create_options create_options{};
  create_options.size = sizeof(create_options);
  create_options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  create_options.kind = unofficial_napi_module_source_text;
  create_options.wrapper = wrapper;
  create_options.url = Str(s.env, "state.mjs");
  create_options.context_or_undefined = undefined;
  create_options.payload.source_text.source = &source;
  create_options.payload.source_text.host_defined_option_id = undefined;
  unofficial_napi_module_create_result create_result{};

  auto invalid_options = create_options;
  invalid_options.size = sizeof(invalid_options) - 1;
  EXPECT_EQ(unofficial_napi_module_wrap_create(s.env, &invalid_options, &create_result),
            napi_invalid_arg);
  invalid_options = create_options;
  invalid_options.version++;
  EXPECT_EQ(unofficial_napi_module_wrap_create(s.env, &invalid_options, &create_result),
            napi_invalid_arg);
  invalid_options = create_options;
  invalid_options.kind = static_cast<unofficial_napi_module_kind>(99);
  EXPECT_EQ(unofficial_napi_module_wrap_create(s.env, &invalid_options, &create_result),
            napi_invalid_arg);

  ASSERT_EQ(unofficial_napi_module_wrap_create(s.env, &create_options, &create_result),
            napi_ok);
  unofficial_napi_module module = create_result.module;
  ASSERT_NE(module, nullptr);
  ASSERT_NE(create_result.module_requests, nullptr);
  uint32_t request_count = 1;
  ASSERT_EQ(napi_get_array_length(s.env, create_result.module_requests, &request_count),
            napi_ok);
  EXPECT_EQ(request_count, 0u);
  EXPECT_FALSE(create_result.has_top_level_await);

  EXPECT_EQ(unofficial_napi_module_wrap_get_state(
                s.env, module, nullptr, nullptr, nullptr),
            napi_invalid_arg);
  int32_t status = -1;
  napi_value error = nullptr;
  bool has_async_graph = true;
  ASSERT_EQ(unofficial_napi_module_wrap_get_state(
                s.env, module, &status, &error, &has_async_graph),
            napi_ok);
  EXPECT_EQ(status, 0);
  EXPECT_FALSE(has_async_graph);
  ASSERT_NE(error, nullptr);
  napi_valuetype error_type = napi_object;
  ASSERT_EQ(napi_typeof(s.env, error, &error_type), napi_ok);
  EXPECT_EQ(error_type, napi_undefined);

  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, module, 0, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, module), napi_ok);
  status = -1;
  has_async_graph = true;
  ASSERT_EQ(unofficial_napi_module_wrap_get_state(
                s.env, module, &status, nullptr, &has_async_graph),
            napi_ok);
  EXPECT_EQ(status, 2);
  EXPECT_FALSE(has_async_graph);

  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);
}

// Node's ESM loader rejects JSON modules whose request lost its
// `with { type: 'json' }` clause (ERR_IMPORT_ATTRIBUTE_MISSING), so every
// provider must surface parsed import attributes on the created requests.
TEST_F(Test71ContextifyCompat, ModuleRequestsPreserveImportAttributes) {
  EnvScope s(runtime_.get());

  napi_value wrapper = nullptr;
  napi_value undefined = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &wrapper), napi_ok);
  ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
  const unofficial_napi_js_source source =
      unofficial_napi_js_source_from_text(Str(s.env,
          "import data from './data.json' with { type: 'json' };\n"
          "export const ok = data;"));
  unofficial_napi_module_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  options.kind = unofficial_napi_module_source_text;
  options.wrapper = wrapper;
  options.url = Str(s.env, "attributed.mjs");
  options.context_or_undefined = undefined;
  options.payload.source_text.source = &source;
  options.payload.source_text.host_defined_option_id = undefined;
  unofficial_napi_module_create_result created{};
  ASSERT_EQ(unofficial_napi_module_wrap_create(s.env, &options, &created), napi_ok);
  ASSERT_NE(created.module, nullptr);
  ASSERT_NE(created.module_requests, nullptr);

  uint32_t request_count = 0;
  ASSERT_EQ(napi_get_array_length(s.env, created.module_requests, &request_count),
            napi_ok);
  ASSERT_EQ(request_count, 1u);
  napi_value request = nullptr;
  ASSERT_EQ(napi_get_element(s.env, created.module_requests, 0, &request), napi_ok);

  napi_value specifier = nullptr;
  char text[64] = {0};
  size_t text_length = 0;
  ASSERT_EQ(napi_get_named_property(s.env, request, "specifier", &specifier),
            napi_ok);
  ASSERT_EQ(napi_get_value_string_utf8(s.env, specifier, text, sizeof(text),
                                       &text_length),
            napi_ok);
  EXPECT_STREQ(text, "./data.json");

  napi_value attributes = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, request, "attributes", &attributes),
            napi_ok);
  napi_valuetype attributes_type = napi_undefined;
  ASSERT_EQ(napi_typeof(s.env, attributes, &attributes_type), napi_ok);
  ASSERT_EQ(attributes_type, napi_object);
  napi_value type_value = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, attributes, "type", &type_value),
            napi_ok);
  ASSERT_EQ(napi_get_value_string_utf8(s.env, type_value, text, sizeof(text),
                                       &text_length),
            napi_ok);
  EXPECT_STREQ(text, "json");

  napi_value phase = nullptr;
  int32_t phase_number = 0;
  ASSERT_EQ(napi_get_named_property(s.env, request, "phase", &phase), napi_ok);
  ASSERT_EQ(napi_get_value_int32(s.env, phase, &phase_number), napi_ok);
  EXPECT_EQ(phase_number, 2);

  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, created.module), napi_ok);
}

TEST_F(Test71ContextifyCompat, SyncModuleErrorDoesNotLeaveUnhandledRejection) {
  EnvScope s(runtime_.get());

  napi_value callback = nullptr;
  ASSERT_EQ(napi_run_script(s.env, Str(s.env,
      "globalThis.unhandledModulePromises = new Set();"
      "globalThis.moduleError = new Error('module failure');"
      "(event, promise) => {"
      "  if (event === 0) unhandledModulePromises.add(promise);"
      "  if (event === 1) unhandledModulePromises.delete(promise);"
      "}"), &callback), napi_ok);
  ASSERT_EQ(unofficial_napi_set_promise_reject_callback(s.env, callback), napi_ok);

  napi_value wrapper = nullptr;
  napi_value undefined = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &wrapper), napi_ok);
  ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
  const unofficial_napi_js_source source =
      unofficial_napi_js_source_from_text(Str(s.env, "throw globalThis.moduleError;"));
  unofficial_napi_module_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  options.kind = unofficial_napi_module_source_text;
  options.wrapper = wrapper;
  options.url = Str(s.env, "throwing.mjs");
  options.context_or_undefined = undefined;
  options.payload.source_text.source = &source;
  options.payload.source_text.host_defined_option_id = undefined;
  unofficial_napi_module_create_result created{};
  ASSERT_EQ(unofficial_napi_module_wrap_create(s.env, &options, &created), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, created.module, 0, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, created.module), napi_ok);

  napi_value result = nullptr;
  EXPECT_EQ(unofficial_napi_module_wrap_evaluate_sync(
                s.env, created.module, options.url, undefined, &result),
            napi_pending_exception);
  napi_value exception = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &exception), napi_ok);
  napi_value expected = nullptr;
  ASSERT_EQ(napi_run_script(s.env, Str(s.env, "globalThis.moduleError"), &expected), napi_ok);
  bool same_error = false;
  ASSERT_EQ(napi_strict_equals(s.env, exception, expected, &same_error), napi_ok);
  EXPECT_TRUE(same_error);

  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
            napi_ok);
  ASSERT_EQ(napi_run_script(s.env, Str(s.env, "unhandledModulePromises.size"), &result), napi_ok);
  uint32_t unhandled_count = 0;
  ASSERT_EQ(napi_get_value_uint32(s.env, result, &unhandled_count), napi_ok);
  EXPECT_EQ(unhandled_count, 0u);
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, created.module), napi_ok);
}

TEST_F(Test71ContextifyCompat, ModuleHooksUseOneVersionedConfiguration) {
  EnvScope s(runtime_.get());

  unofficial_napi_module_hooks hooks{};
  hooks.size = sizeof(hooks) - 1;
  hooks.version = UNOFFICIAL_NAPI_MODULE_HOOKS_VERSION;
  EXPECT_EQ(unofficial_napi_module_wrap_set_hooks(s.env, &hooks), napi_invalid_arg);

  hooks.size = sizeof(hooks);
  hooks.version = 0;
  EXPECT_EQ(unofficial_napi_module_wrap_set_hooks(s.env, &hooks), napi_invalid_arg);

  hooks.version = UNOFFICIAL_NAPI_MODULE_HOOKS_VERSION;
  EXPECT_EQ(unofficial_napi_module_wrap_set_hooks(s.env, &hooks), napi_ok);
}

#if defined(NAPI_TEST_ENGINE_V8)
// V8 stores the host-defined-options block in each compiled function's
// ScriptOrigin. This regression covers the V8 bytecode fast path changed in
// v8/src/unofficial_napi_contextify.cc. QuickJS resolves dynamic imports by
// referrer name instead and has no equivalent per-function ScriptOrigin.
TEST_F(Test71ContextifyCompat,
       FunctionBytecodePreservesExplicitHostDefinedOptionIdentity) {
  EnvScope s(runtime_.get());

  napi_value callback = nullptr;
  ASSERT_EQ(napi_create_function(s.env,
                                 "captureDynamicImportId",
                                 NAPI_AUTO_LENGTH,
                                 CaptureDynamicImportId,
                                 nullptr,
                                 &callback),
            napi_ok);
  const unofficial_napi_module_hooks hooks = {
      sizeof(unofficial_napi_module_hooks),
      UNOFFICIAL_NAPI_MODULE_HOOKS_VERSION,
      callback,
      nullptr,
  };
  ASSERT_EQ(unofficial_napi_module_wrap_set_hooks(s.env, &hooks), napi_ok);

  napi_value undefined = nullptr;
  ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
  napi_value explicit_id = Sym(s.env, "explicit-cjs-host-id");
  ASSERT_NE(explicit_id, nullptr);

  auto compile_invoke_and_capture = [&](napi_value host_id) -> napi_value {
    unofficial_napi_bytecode_open_options options{};
    options.size = sizeof(options);
    options.version = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION;
    options.source_text = Str(s.env, "return import('node:test');");
    options.filename = Str(s.env, "host-id.js");
    options.shape = unofficial_napi_bytecode_shape_cjs_function;
    options.params_or_undefined = undefined;
    options.host_defined_option_id = host_id;
    unofficial_napi_bytecode_open_result result{};
    EXPECT_EQ(unofficial_napi_bytecode_open(s.env, &options, &result), napi_ok);
    unofficial_napi_bytecode bytecode = result.bytecode;
    EXPECT_NE(bytecode, nullptr);
    if (bytecode == nullptr) return nullptr;

    const unofficial_napi_js_source source =
        unofficial_napi_js_source_from_bytecode(bytecode);
    napi_value compiled = nullptr;
    EXPECT_EQ(unofficial_napi_contextify_compile_function(s.env,
                                                          &source,
                                                          Str(s.env, "host-id.js"),
                                                          0,
                                                          0,
                                                          undefined,
                                                          undefined,
                                                          undefined,
                                                          host_id,
                                                          &compiled),
              napi_ok);
    EXPECT_NE(compiled, nullptr);

    napi_value fn = nullptr;
    napi_value global = nullptr;
    napi_value import_result = nullptr;
    if (compiled != nullptr) {
      EXPECT_EQ(napi_get_named_property(s.env, compiled, "function", &fn), napi_ok);
      EXPECT_EQ(napi_get_global(s.env, &global), napi_ok);
      EXPECT_EQ(napi_call_function(s.env, global, fn, 0, nullptr, &import_result), napi_ok);
    }

    napi_value captured = nullptr;
    if (global != nullptr) {
      EXPECT_EQ(napi_get_named_property(
                    s.env, global, "__captured_dynamic_import_id", &captured),
                napi_ok);
      if (import_result != nullptr) {
        EXPECT_EQ(napi_set_named_property(s.env, global, "__host_id_import_result", import_result),
                  napi_ok);
        napi_value ignored = nullptr;
        EXPECT_EQ(napi_run_script(
                      s.env,
                      Str(s.env, "__host_id_import_result.catch(() => {});"),
                      &ignored),
                  napi_ok);
      }
    }
    EXPECT_EQ(unofficial_napi_bytecode_release(s.env, bytecode), napi_ok);
    return captured;
  };

  napi_value captured_without_id = compile_invoke_and_capture(undefined);
  ASSERT_NE(captured_without_id, nullptr);
  bool equal = false;
  ASSERT_EQ(napi_strict_equals(s.env, captured_without_id, undefined, &equal), napi_ok);
  EXPECT_TRUE(equal);

  napi_value captured_with_id = compile_invoke_and_capture(explicit_id);
  ASSERT_NE(captured_with_id, nullptr);
  ASSERT_EQ(napi_strict_equals(s.env, captured_with_id, explicit_id, &equal), napi_ok);
  EXPECT_TRUE(equal);

  uint32_t checkpoint_state = unofficial_napi_event_loop_checkpoint_state_none;
  EXPECT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env,
                unofficial_napi_event_loop_checkpoint_microtasks,
                true,
                &checkpoint_state),
            napi_ok);
}
#endif

TEST_F(Test71ContextifyCompat, CompileFunctionDoesNotUseGlobalFunctionConstructor) {
  EnvScope s(runtime_.get());

  napi_value patch_result = nullptr;
  ASSERT_EQ(napi_run_script(
                s.env,
                Str(s.env,
                    R"JS(
globalThis.Function = function Function() {
  throw new Error("global Function constructor should not be called");
};
1;
)JS"),
                &patch_result),
            napi_ok);

  napi_value undef = nullptr;
  ASSERT_EQ(napi_get_undefined(s.env, &undef), napi_ok);

  napi_value params = nullptr;
  ASSERT_EQ(napi_create_array_with_length(s.env, 1, &params), napi_ok);
  ASSERT_EQ(napi_set_element(s.env, params, 0, Str(s.env, "value")), napi_ok);

  napi_value out = nullptr;
  const unofficial_napi_js_source fn_source =
      unofficial_napi_js_source_from_text(Str(s.env, "return value + 1;"));
  ASSERT_EQ(unofficial_napi_contextify_compile_function(s.env,
                                                        &fn_source,
                                                        Str(s.env, "no-global-function.js"),
                                                        0,
                                                        0,
                                                        undef,
                                                        undef,
                                                        params,
                                                        undef,
                                                        &out),
            napi_ok);
  ASSERT_NE(out, nullptr);

  napi_value fn = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, out, "function", &fn), napi_ok);
  ASSERT_NE(fn, nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  napi_value argv[1] = {nullptr};
  ASSERT_EQ(napi_create_int32(s.env, 41, &argv[0]), napi_ok);

  napi_value fn_result = nullptr;
  ASSERT_EQ(napi_call_function(s.env, global, fn, 1, argv, &fn_result), napi_ok);
  int32_t answer = 0;
  ASSERT_EQ(napi_get_value_int32(s.env, fn_result, &answer), napi_ok);
  EXPECT_EQ(answer, 42);
}

TEST_F(Test71ContextifyCompat, CompileFunctionAcceptsHashbangBody) {
  EnvScope s(runtime_.get());

  napi_value undef = nullptr;
  ASSERT_EQ(napi_get_undefined(s.env, &undef), napi_ok);

  napi_value out = nullptr;
  const unofficial_napi_js_source hashbang_source = unofficial_napi_js_source_from_text(
      Str(s.env, "#!/usr/bin/env node\nreturn 42;"));
  ASSERT_EQ(unofficial_napi_contextify_compile_function(s.env,
                                                        &hashbang_source,
                                                        Str(s.env, ""),
                                                        0,
                                                        0,
                                                        undef,
                                                        undef,
                                                        undef,
                                                        undef,
                                                        &out),
            napi_ok);
  ASSERT_NE(out, nullptr);

  napi_value fn = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, out, "function", &fn), napi_ok);
  ASSERT_NE(fn, nullptr);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  napi_value fn_result = nullptr;
  ASSERT_EQ(napi_call_function(s.env, global, fn, 0, nullptr, &fn_result), napi_ok);

  int32_t answer = 0;
  ASSERT_EQ(napi_get_value_int32(s.env, fn_result, &answer), napi_ok);
  EXPECT_EQ(answer, 42);
}

TEST_F(Test71ContextifyCompat, CompileFunctionRejectsBomBeforeHashbang) {
  EnvScope s(runtime_.get());

  napi_value undef = nullptr;
  ASSERT_EQ(napi_get_undefined(s.env, &undef), napi_ok);

  napi_value out = nullptr;
  const unofficial_napi_js_source bom_source = unofficial_napi_js_source_from_text(
      Str(s.env, "\xEF\xBB\xBF#!/usr/bin/env node\nreturn 42;"));
  EXPECT_EQ(unofficial_napi_contextify_compile_function(s.env,
                                                        &bom_source,
                                                        Str(s.env, "bom_hashbang.js"),
                                                        0,
                                                        0,
                                                        undef,
                                                        undef,
                                                        undef,
                                                        undef,
                                                        &out),
            napi_pending_exception);

  bool pending = false;
  ASSERT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
  EXPECT_TRUE(pending);
  if (pending) {
    napi_value error = nullptr;
    ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &error), napi_ok);
    ASSERT_NE(error, nullptr);
  }
}

TEST_F(Test71ContextifyCompat, FunctionPrototypeSourceLocationsDoNotBlockStaticAssignments) {
  EnvScope s(runtime_.get());

  napi_value result = nullptr;
  ASSERT_EQ(napi_run_script(
                s.env,
                Str(s.env,
                    R"JS(
"use strict";
for (const key of ["fileName", "lineNumber", "columnNumber"]) {
  if (Object.getOwnPropertyDescriptor(Function.prototype, key) !== undefined) {
    throw new Error(`${key} should not be inherited from Function.prototype`);
  }
}
class Manifest {}
Manifest.fileName = "package.json";
Manifest.lineNumber = 1;
Manifest.columnNumber = 2;
const fileName = Object.getOwnPropertyDescriptor(Manifest, "fileName");
const lineNumber = Object.getOwnPropertyDescriptor(Manifest, "lineNumber");
const columnNumber = Object.getOwnPropertyDescriptor(Manifest, "columnNumber");
fileName.value === "package.json" && fileName.writable &&
  lineNumber.value === 1 && lineNumber.writable &&
  columnNumber.value === 2 && columnNumber.writable ? 1 : 0;
)JS"),
                &result),
            napi_ok);
  ASSERT_NE(result, nullptr);

  int32_t ok = 0;
  ASSERT_EQ(napi_get_value_int32(s.env, result, &ok), napi_ok);
  EXPECT_EQ(ok, 1);
}

TEST_F(Test71ContextifyCompat, CjsCompileAndSyntaxDetection) {
  EnvScope s(runtime_.get());

  napi_value params = nullptr;
  ASSERT_EQ(napi_create_array_with_length(s.env, 5, &params), napi_ok);
  ASSERT_EQ(napi_set_element(s.env, params, 0, Str(s.env, "exports")), napi_ok);
  ASSERT_EQ(napi_set_element(s.env, params, 1, Str(s.env, "require")), napi_ok);
  ASSERT_EQ(napi_set_element(s.env, params, 2, Str(s.env, "module")), napi_ok);
  ASSERT_EQ(napi_set_element(s.env, params, 3, Str(s.env, "__filename")), napi_ok);
  ASSERT_EQ(napi_set_element(s.env, params, 4, Str(s.env, "__dirname")), napi_ok);

  napi_value undef = nullptr;
  ASSERT_EQ(napi_get_undefined(s.env, &undef), napi_ok);

  napi_value out = nullptr;
  const unofficial_napi_js_source cjs_source =
      unofficial_napi_js_source_from_text(Str(s.env, "module.exports = 1;"));
  ASSERT_EQ(unofficial_napi_contextify_compile_function(s.env,
                                                        &cjs_source,
                                                        Str(s.env, "cjs.js"),
                                                        0,
                                                        0,
                                                        undef,
                                                        undef,
                                                        params,
                                                        undef,
                                                        &out),
            napi_ok);
  ASSERT_NE(out, nullptr);
  napi_value fn = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, out, "function", &fn), napi_ok);
  ASSERT_NE(fn, nullptr);

  bool contains = false;
  ASSERT_EQ(unofficial_napi_contextify_contains_module_syntax(s.env,
                                                              Str(s.env, "export const x = 1;"),
                                                              Str(s.env, "esmish.js"),
                                                              Str(s.env, "file:///esmish.js"),
                                                              true,
                                                              &contains),
            napi_ok);
  EXPECT_TRUE(contains);

  ASSERT_EQ(unofficial_napi_contextify_contains_module_syntax(s.env,
                                                              Str(s.env, "module.exports = 1;"),
                                                              Str(s.env, "cjs.js"),
                                                              Str(s.env, "file:///cjs.js"),
                                                              true,
                                                              &contains),
            napi_ok);
  EXPECT_FALSE(contains);

  ASSERT_EQ(unofficial_napi_contextify_contains_module_syntax(
                s.env,
                Str(s.env,
                    "var __export = (target, all) => target;\n"
                    "// Annotate the CommonJS export names for ESM import in node:\n"
                    "0 && (module.exports = { build });\n"),
                Str(s.env, "esbuild-ish.js"),
                Str(s.env, "file:///esbuild-ish.js"),
                true,
                &contains),
            napi_ok);
  EXPECT_FALSE(contains);

  ASSERT_EQ(unofficial_napi_contextify_contains_module_syntax(s.env,
                                                              Str(s.env, "import('node:fs');"),
                                                              Str(s.env, "dynamic-import.cjs"),
                                                              Str(s.env, "file:///dynamic-import.cjs"),
                                                              true,
                                                              &contains),
            napi_ok);
  EXPECT_FALSE(contains);

  ASSERT_EQ(unofficial_napi_contextify_contains_module_syntax(s.env,
                                                              Str(s.env, "await 1;"),
                                                              Str(s.env, "tla.js"),
                                                              Str(s.env, "file:///tla.js"),
                                                              true,
                                                              &contains),
            napi_ok);
  EXPECT_TRUE(contains);
}

TEST_F(Test71ContextifyCompat, PrivateSymbolAcceptsAutoLength) {
  EnvScope s(runtime_.get());

  napi_value symbol = nullptr;
  ASSERT_EQ(unofficial_napi_create_private_symbol(
                s.env, "node:arrowMessage", NAPI_AUTO_LENGTH, &symbol),
            napi_ok);
  ASSERT_NE(symbol, nullptr);

  napi_valuetype type = napi_undefined;
  ASSERT_EQ(napi_typeof(s.env, symbol, &type), napi_ok);
  EXPECT_TRUE(type == napi_symbol || type == napi_object);
}

#if defined(NAPI_TEST_ENGINE_V8)
TEST_F(Test71ContextifyCompat, PrepareStackTraceResultIsNotRewritten) {
  EnvScope s(runtime_.get());

  napi_value callback = nullptr;
  ASSERT_EQ(napi_create_function(s.env,
                                 "prepareStackTrace",
                                 NAPI_AUTO_LENGTH,
                                 ReturnPreparedStack,
                                 nullptr,
                                 &callback),
            napi_ok);
  ASSERT_NE(callback, nullptr);
  ASSERT_EQ(unofficial_napi_set_prepare_stack_trace_callback(s.env, callback),
            napi_ok);

  napi_value source = Str(s.env, "new Error('sentinel').stack");
  napi_value stack = nullptr;
  ASSERT_EQ(napi_run_script(s.env, source, &stack), napi_ok);
  ASSERT_NE(stack, nullptr);

  size_t length = 0;
  ASSERT_EQ(napi_get_value_string_utf8(s.env, stack, nullptr, 0, &length),
            napi_ok);
  std::string actual(length + 1, '\0');
  size_t written = 0;
  ASSERT_EQ(napi_get_value_string_utf8(
                s.env, stack, actual.data(), actual.size(), &written),
            napi_ok);
  actual.resize(written);
  EXPECT_EQ(actual, kPreparedStack);

  ASSERT_EQ(unofficial_napi_set_prepare_stack_trace_callback(s.env, nullptr),
            napi_ok);
}
#endif

// The rest pins how the deprecated API maps onto realms.

namespace {

napi_value Undefined(napi_env env) {
  napi_value out = nullptr;
  if (napi_get_undefined(env, &out) != napi_ok) return nullptr;
  return out;
}

napi_value Null(napi_env env) {
  napi_value out = nullptr;
  if (napi_get_null(env, &out) != napi_ok) return nullptr;
  return out;
}

napi_value Global(napi_env env) {
  napi_value out = nullptr;
  if (napi_get_global(env, &out) != napi_ok) return nullptr;
  return out;
}

napi_value Object(napi_env env) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok) return nullptr;
  return out;
}

napi_value Prototype(napi_env env, napi_value object) {
  napi_value out = nullptr;
  if (object == nullptr || napi_get_prototype(env, object, &out) != napi_ok) return nullptr;
  return out;
}

bool StrictEquals(napi_env env, napi_value a, napi_value b) {
  bool same = false;
  return a != nullptr && b != nullptr && napi_strict_equals(env, a, b, &same) == napi_ok &&
         same;
}

// Runs `source` with the realm API (NULL: the env's own realm).
napi_value RunIn(napi_env env, unofficial_napi_realm realm, const char* source) {
  const unofficial_napi_js_source script = unofficial_napi_js_source_from_text(Str(env, source));
  napi_value result = nullptr;
  if (unofficial_napi_run_script(env, realm, &script, Str(env, "realm.js"), 0, 0, -1, true, false,
                                 false, nullptr, &result) != napi_ok) {
    return nullptr;
  }
  return result;
}

// Runs `source` with the deprecated call.
napi_status RunInContext(napi_env env,
                         napi_value sandbox_or_null,
                         const char* source,
                         napi_value* result_out) {
  const unofficial_napi_js_source script = unofficial_napi_js_source_from_text(Str(env, source));
  return unofficial_napi_contextify_run_script(env, sandbox_or_null, &script, Str(env, "ctx.js"), 0,
                                               0, -1, true, false, false, nullptr, result_out);
}

// Compiles and calls `body` with the deprecated call.
napi_status CallCompiled(napi_env env,
                         napi_value parsing_context,
                         const char* body,
                         napi_value* result_out) {
  const unofficial_napi_js_source source = unofficial_napi_js_source_from_text(Str(env, body));
  napi_value compiled = nullptr;
  napi_status status = unofficial_napi_contextify_compile_function(
      env, &source, Str(env, "fn.js"), 0, 0, parsing_context, nullptr, nullptr, nullptr,
      &compiled);
  if (status != napi_ok) return status;
  napi_value fn = nullptr;
  status = napi_get_named_property(env, compiled, "function", &fn);
  if (status != napi_ok) return status;
  return napi_call_function(env, Undefined(env), fn, 0, nullptr, result_out);
}

napi_status MakeContext(napi_env env, napi_value sandbox_or_symbol, napi_value* result_out) {
  return unofficial_napi_contextify_make_context(env, sandbox_or_symbol, Str(env, "ctx"),
                                                 Str(env, "test://origin"), true, true, false,
                                                 Sym(env, "hdo"), result_out);
}

unofficial_napi_realm FreshRealm(napi_env env, napi_value* global_out) {
  unofficial_napi_realm realm = nullptr;
  if (unofficial_napi_create_realm(env, nullptr, &realm, global_out) != napi_ok) return nullptr;
  return realm;
}

// main's version-1 layouts, which binaries built against main pass. The
// providers must read no further than they end.
struct MainBytecodeOpenOptionsV1 {
  uint32_t size;
  uint32_t version;
  napi_value source_text;
  napi_value filename;
  int32_t shape;
  napi_value params_or_undefined;
  napi_value host_defined_option_id;
  int32_t line_offset;
  int32_t column_offset;
  const uint8_t* cache_bytes;
  size_t cache_byte_length;
  uint8_t has_cache;
  uint8_t cache_policy;
};

struct MainModuleCreateOptionsV1 {
  size_t size;
  uint32_t version;
  unofficial_napi_module_kind kind;
  napi_value wrapper;
  napi_value url;
  napi_value context_or_undefined;
  union {
    unofficial_napi_source_text_module_options source_text;
    unofficial_napi_synthetic_module_options synthetic;
  } payload;
};

struct MainEnvHooksV1 {
  uint32_t size;
  uint32_t version;
  void* data;
  unofficial_napi_context_token_callback context_token_assign_callback;
  unofficial_napi_context_token_callback context_token_unassign_callback;
  unofficial_napi_enqueue_foreground_task_callback enqueue_foreground_task_callback;
  unofficial_napi_fatal_error_callback fatal_error_callback;
  unofficial_napi_oom_error_callback oom_error_callback;
};

static_assert(sizeof(MainBytecodeOpenOptionsV1) == UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_V1_SIZE);
static_assert(offsetof(unofficial_napi_bytecode_open_options, cache_policy) ==
              offsetof(MainBytecodeOpenOptionsV1, cache_policy));
static_assert(sizeof(MainModuleCreateOptionsV1) == UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_V1_SIZE);
static_assert(offsetof(unofficial_napi_module_create_options, context_or_undefined) ==
              offsetof(MainModuleCreateOptionsV1, context_or_undefined));
static_assert(offsetof(unofficial_napi_module_create_options, payload) ==
              offsetof(MainModuleCreateOptionsV1, payload));
static_assert(sizeof(MainEnvHooksV1) == sizeof(unofficial_napi_env_hooks));
static_assert(offsetof(unofficial_napi_env_hooks, context_token_unassign_callback) ==
              offsetof(MainEnvHooksV1, context_token_unassign_callback));
static_assert(offsetof(unofficial_napi_env_hooks, oom_error_callback) ==
              offsetof(MainEnvHooksV1, oom_error_callback));
static_assert(UNOFFICIAL_NAPI_ENV_HOOKS_VERSION == 1);

MainBytecodeOpenOptionsV1 MainBytecodeOptions(napi_env env, const char* source) {
  MainBytecodeOpenOptionsV1 options{};
  options.size = sizeof(options);
  options.version = 1;
  options.source_text = Str(env, source);
  options.filename = Str(env, "bytecode.js");
  options.shape = unofficial_napi_bytecode_shape_script;
  options.params_or_undefined = Undefined(env);
  options.host_defined_option_id = Undefined(env);
  return options;
}

// Opens a script that does not parse and returns the prototype of the
// SyntaxError, which shows the realm it was compiled for.
napi_value SyntaxErrorPrototype(napi_env env, const void* options) {
  unofficial_napi_bytecode_open_result opened{};
  if (unofficial_napi_bytecode_open(
          env, static_cast<const unofficial_napi_bytecode_open_options*>(options), &opened) !=
      napi_pending_exception) {
    return nullptr;
  }
  napi_value error = nullptr;
  if (napi_get_and_clear_last_exception(env, &error) != napi_ok) return nullptr;
  return Prototype(env, error);
}

// Creates `code` ("export const g = globalThis;" unless given) from `options`
// (main's version-1 layout or the current one), evaluates it and returns g.
// `status_out` receives module_wrap_create's status.
template <typename Options>
napi_value ModuleGlobal(napi_env env, Options options, napi_status* status_out,
                        const char* code = "export const g = globalThis;") {
  const unofficial_napi_js_source source = unofficial_napi_js_source_from_text(Str(env, code));
  options.kind = unofficial_napi_module_source_text;
  options.wrapper = Object(env);
  options.url = Str(env, "compat.mjs");
  options.payload.source_text.source = &source;
  options.payload.source_text.host_defined_option_id = Undefined(env);
  unofficial_napi_module_create_result created{};
  *status_out = unofficial_napi_module_wrap_create(
      env, reinterpret_cast<const unofficial_napi_module_create_options*>(&options), &created);
  if (*status_out != napi_ok) return nullptr;
  napi_value evaluated = nullptr;
  napi_value ns = nullptr;
  napi_value g = nullptr;
  if (unofficial_napi_module_wrap_link(env, created.module, 0, nullptr) != napi_ok ||
      unofficial_napi_module_wrap_instantiate(env, created.module) != napi_ok ||
      unofficial_napi_module_wrap_evaluate(env, created.module, -1, false, &evaluated) !=
          napi_ok ||
      unofficial_napi_module_wrap_get_namespace(env, created.module, &ns) != napi_ok ||
      napi_get_named_property(env, ns, "g", &g) != napi_ok) {
    g = nullptr;
  }
  unofficial_napi_module_wrap_destroy(env, created.module);
  return g;
}

MainModuleCreateOptionsV1 MainModuleOptions(napi_value context) {
  MainModuleCreateOptionsV1 options{};
  options.size = sizeof(options);
  options.version = 1;
  options.context_or_undefined = context;
  return options;
}

unofficial_napi_module_create_options ModuleOptions(unofficial_napi_realm realm,
                                                    napi_value context) {
  unofficial_napi_module_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  options.realm_or_null = realm;
  options.context_or_undefined = context;
  return options;
}

// Opens a realm scope for the enclosing block.
class EnteredRealm {
 public:
  EnteredRealm(napi_env env, unofficial_napi_realm realm) : env_(env) {
    EXPECT_EQ(unofficial_napi_enter_realm(env, realm, &scope_), napi_ok);
  }
  ~EnteredRealm() { EXPECT_EQ(unofficial_napi_leave_realm(env_, scope_), napi_ok); }
  EnteredRealm(const EnteredRealm&) = delete;
  EnteredRealm& operator=(const EnteredRealm&) = delete;

 private:
  napi_env env_;
  unofficial_napi_realm_scope scope_ = 0;
};

struct CallbackRealm {
  napi_ref seen = nullptr;
};

// Records the global a deprecated run with no sandbox sees.
napi_value SeeNullishContext(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data) != napi_ok) return nullptr;
  napi_value seen = nullptr;
  if (RunInContext(env, nullptr, "globalThis", &seen) != napi_ok) return nullptr;
  napi_create_reference(env, seen, 1, &static_cast<CallbackRealm*>(data)->seen);
  return nullptr;
}

}  // namespace

// vm.constants.DONT_CONTEXTIFY: a symbol makes a realm with a fresh global.
TEST_F(Test71ContextifyCompat, MakeContextWithASymbolMakesAFreshGlobal) {
  EnvScope s(runtime_.get());

  napi_value global = nullptr;
  ASSERT_EQ(MakeContext(s.env, Sym(s.env, "DONT_CONTEXTIFY"), &global), napi_ok);
  ASSERT_NE(global, nullptr);
  EXPECT_FALSE(StrictEquals(s.env, global, Global(s.env)));

  unofficial_napi_realm realm = nullptr;
  ASSERT_EQ(unofficial_napi_get_realm(s.env, global, &realm), napi_ok);
  ASSERT_NE(realm, nullptr);
  EXPECT_TRUE(StrictEquals(s.env, RunIn(s.env, realm, "globalThis"), global));
  napi_value seen = nullptr;
  ASSERT_EQ(RunInContext(s.env, global, "globalThis", &seen), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, seen, global));
  EXPECT_FALSE(StrictEquals(s.env, RunIn(s.env, realm, "Object.prototype"),
                            RunIn(s.env, nullptr, "Object.prototype")));
}

// main's V8 statuses. (main's QuickJS took a NULL name, and returned an
// already contextified sandbox again; both providers now refuse.)
TEST_F(Test71ContextifyCompat, MakeContextRefusesWhatMainRefused) {
  EnvScope s(runtime_.get());

  napi_value sandbox = Object(s.env);
  napi_value number = nullptr;
  ASSERT_EQ(napi_create_int32(s.env, 7, &number), napi_ok);
  napi_value result = nullptr;
  EXPECT_EQ(unofficial_napi_contextify_make_context(s.env, sandbox, nullptr, nullptr, true, true,
                                                    false, nullptr, &result),
            napi_invalid_arg);
  EXPECT_EQ(MakeContext(s.env, nullptr, &result), napi_invalid_arg);
  EXPECT_EQ(MakeContext(s.env, number, &result), napi_invalid_arg);
  EXPECT_EQ(MakeContext(s.env, Undefined(s.env), &result), napi_invalid_arg);
  EXPECT_EQ(MakeContext(s.env, sandbox, nullptr), napi_invalid_arg);
  EXPECT_EQ(result, nullptr);

  ASSERT_EQ(MakeContext(s.env, sandbox, &result), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, result, sandbox));
  // A sandbox belongs to one realm, whichever call made it.
  napi_value again = nullptr;
  EXPECT_EQ(MakeContext(s.env, sandbox, &again), napi_invalid_arg);
  napi_value realm_sandbox = Object(s.env);
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = realm_sandbox;
  unofficial_napi_realm realm = nullptr;
  ASSERT_EQ(unofficial_napi_create_realm(s.env, &options, &realm, nullptr), napi_ok);
  EXPECT_EQ(MakeContext(s.env, realm_sandbox, &again), napi_invalid_arg);
  EXPECT_EQ(again, nullptr);
}

// The objects and realms of both APIs mix.
TEST_F(Test71ContextifyCompat, ContextifiedObjectsAreRealms) {
  EnvScope s(runtime_.get());

  napi_value sandbox = Object(s.env);
  napi_value result = nullptr;
  ASSERT_EQ(MakeContext(s.env, sandbox, &result), napi_ok);
  unofficial_napi_realm realm = nullptr;
  ASSERT_EQ(unofficial_napi_get_realm(s.env, sandbox, &realm), napi_ok);
  ASSERT_NE(realm, nullptr);
  ASSERT_NE(RunIn(s.env, realm, "globalThis.answer = 42; 1"), nullptr);
  napi_value answer = nullptr;
  ASSERT_EQ(RunInContext(s.env, sandbox, "answer", &answer), napi_ok);
  int32_t value = 0;
  ASSERT_EQ(napi_get_value_int32(s.env, answer, &value), napi_ok);
  EXPECT_EQ(value, 42);

  // (QuickJS copies a sandbox into its realm's own global, so a sandbox
  // realm is told apart by its properties, not by globalThis.)
  napi_value realm_sandbox = Object(s.env);
  napi_value marker = Object(s.env);
  ASSERT_EQ(napi_set_named_property(s.env, realm_sandbox, "marker", marker), napi_ok);
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = realm_sandbox;
  ASSERT_EQ(unofficial_napi_create_realm(s.env, &options, &realm, nullptr), napi_ok);
  napi_value seen = nullptr;
  ASSERT_EQ(RunInContext(s.env, realm_sandbox, "marker", &seen), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, seen, marker));
  ASSERT_EQ(CallCompiled(s.env, realm_sandbox, "return marker;", &seen), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, seen, marker));
}

// A sandbox or parsing context is a contextified object, the env's own global
// or nothing; anything else fails as in main's V8. (main's QuickJS compiled in
// the env's context for an object that was not contextified.)
TEST_F(Test71ContextifyCompat, ContextArgumentsNameARealm) {
  EnvScope s(runtime_.get());

  napi_value env_global = Global(s.env);
  for (napi_value nothing : {static_cast<napi_value>(nullptr), Undefined(s.env), Null(s.env)}) {
    napi_value seen = nullptr;
    ASSERT_EQ(RunInContext(s.env, nothing, "globalThis", &seen), napi_ok);
    EXPECT_TRUE(StrictEquals(s.env, seen, env_global));
    seen = nullptr;
    ASSERT_EQ(CallCompiled(s.env, nothing, "return globalThis;", &seen), napi_ok);
    EXPECT_TRUE(StrictEquals(s.env, seen, env_global));
  }
  napi_value seen = nullptr;
  ASSERT_EQ(RunInContext(s.env, env_global, "globalThis", &seen), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, seen, env_global));
  ASSERT_EQ(CallCompiled(s.env, env_global, "return globalThis;", &seen), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, seen, env_global));

  napi_value number = nullptr;
  ASSERT_EQ(napi_create_int32(s.env, 7, &number), napi_ok);
  for (napi_value other : {Object(s.env), number}) {
    EXPECT_EQ(RunInContext(s.env, other, "1", &seen), napi_invalid_arg);
    EXPECT_EQ(CallCompiled(s.env, other, "return 1;", &seen), napi_invalid_arg);
  }
}

// No sandbox means the current realm, as the env's context did in main: the
// realm entered, or the realm of the running callback. The realm API's NULL
// is always the env's own realm.
TEST_F(Test71ContextifyCompat, NoContextMeansTheCurrentRealm) {
  EnvScope s(runtime_.get());

  napi_value env_global = Global(s.env);
  napi_value realm_global = nullptr;
  unofficial_napi_realm realm = FreshRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);

  CallbackRealm callback;
  napi_value fn = nullptr;
  {
    EnteredRealm entered(s.env, realm);
    napi_value seen = nullptr;
    ASSERT_EQ(RunInContext(s.env, Undefined(s.env), "globalThis", &seen), napi_ok);
    EXPECT_TRUE(StrictEquals(s.env, seen, realm_global));
    ASSERT_EQ(CallCompiled(s.env, nullptr, "return globalThis;", &seen), napi_ok);
    EXPECT_TRUE(StrictEquals(s.env, seen, realm_global));
    EXPECT_TRUE(StrictEquals(s.env, RunIn(s.env, nullptr, "globalThis"), env_global));
    // A native function made here belongs to the realm.
    ASSERT_EQ(napi_create_function(s.env, "see", NAPI_AUTO_LENGTH, SeeNullishContext, &callback,
                                   &fn),
              napi_ok);
  }

  napi_value seen = nullptr;
  ASSERT_EQ(RunInContext(s.env, nullptr, "globalThis", &seen), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, seen, env_global));

  ASSERT_EQ(napi_call_function(s.env, env_global, fn, 0, nullptr, &seen), napi_ok);
  ASSERT_NE(callback.seen, nullptr);
  ASSERT_EQ(napi_get_reference_value(s.env, callback.seen, &seen), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, seen, realm_global));
  ASSERT_EQ(napi_delete_reference(s.env, callback.seen), napi_ok);
}

// The env's own global may be a realm's sandbox (vm.createContext with
// globalThis). Named as a context it names that realm, as on main's V8, and
// nothing still means the env's realm while that is current. While the
// realm is current, nothing means the realm on V8, but the env's realm on
// QuickJS, whose napi_get_global returns the sandbox there: the env's own
// global. (An array literal shows the realm a script ran in; a sandbox realm
// resolves global names such as Array through the sandbox.)
TEST_F(Test71ContextifyCompat, TheEnvsGlobalAsASandbox) {
  EnvScope s(runtime_.get());

  napi_value env_global = Global(s.env);
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = env_global;
  unofficial_napi_realm realm = nullptr;
  ASSERT_EQ(unofficial_napi_create_realm(s.env, &options, &realm, nullptr), napi_ok);
  napi_value env_array = Prototype(s.env, RunIn(s.env, nullptr, "[]"));
  napi_value realm_array = Prototype(s.env, RunIn(s.env, realm, "[]"));
  ASSERT_NE(env_array, nullptr);
  ASSERT_NE(realm_array, nullptr);
  ASSERT_FALSE(StrictEquals(s.env, env_array, realm_array));
  auto array_in = [&](napi_value context) {
    napi_value array = nullptr;
    EXPECT_EQ(RunInContext(s.env, context, "[]", &array), napi_ok);
    return Prototype(s.env, array);
  };

  EXPECT_TRUE(StrictEquals(s.env, array_in(env_global), realm_array));
  EXPECT_TRUE(StrictEquals(s.env, array_in(nullptr), env_array));
  EnteredRealm entered(s.env, realm);
  EXPECT_TRUE(StrictEquals(s.env, array_in(env_global), realm_array));
#if defined(NAPI_TEST_ENGINE_QUICKJS)
  EXPECT_TRUE(StrictEquals(s.env, array_in(nullptr), env_array));
#else
  EXPECT_TRUE(StrictEquals(s.env, array_in(nullptr), realm_array));
#endif
}

// Version 1 of the bytecode and module options takes the current realm as
// unofficial_napi_contextify_run_script finds it for nothing: while the realm
// whose sandbox is the env's own global is current, that realm on V8, but the
// env's realm on QuickJS (see TheEnvsGlobalAsASandbox).
TEST_F(Test71ContextifyCompat, VersionOneOptionsInTheEnvsGlobalAsASandbox) {
  EnvScope s(runtime_.get());

  napi_value env_global = Global(s.env);
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = env_global;
  unofficial_napi_realm realm = nullptr;
  ASSERT_EQ(unofficial_napi_create_realm(s.env, &options, &realm, nullptr), napi_ok);

  MainBytecodeOpenOptionsV1 bytecode_v1 = MainBytecodeOptions(s.env, "(");
  unofficial_napi_bytecode_open_options bytecode_v2{};
  std::memcpy(&bytecode_v2, &bytecode_v1, sizeof(bytecode_v1));
  bytecode_v2.size = sizeof(bytecode_v2);
  bytecode_v2.version = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION;
  napi_value env_syntax = SyntaxErrorPrototype(s.env, &bytecode_v2);
  bytecode_v2.realm_or_null = realm;
  napi_value realm_syntax = SyntaxErrorPrototype(s.env, &bytecode_v2);
  ASSERT_NE(env_syntax, nullptr);
  ASSERT_NE(realm_syntax, nullptr);
  ASSERT_FALSE(StrictEquals(s.env, env_syntax, realm_syntax));

  napi_status status = napi_generic_failure;
  const char* const array_module = "export const g = [];";
  napi_value env_array =
      Prototype(s.env, ModuleGlobal(s.env, ModuleOptions(nullptr, nullptr), &status, array_module));
  napi_value realm_array =
      Prototype(s.env, ModuleGlobal(s.env, ModuleOptions(realm, nullptr), &status, array_module));
  ASSERT_NE(env_array, nullptr);
  ASSERT_NE(realm_array, nullptr);
  ASSERT_FALSE(StrictEquals(s.env, env_array, realm_array));
  auto module_array = [&](napi_value context) {
    return Prototype(s.env, ModuleGlobal(s.env, MainModuleOptions(context), &status, array_module));
  };

  EXPECT_TRUE(StrictEquals(s.env, SyntaxErrorPrototype(s.env, &bytecode_v1), env_syntax));
  EXPECT_TRUE(StrictEquals(s.env, module_array(nullptr), env_array));
  EXPECT_TRUE(StrictEquals(s.env, module_array(env_global), realm_array));
  EnteredRealm entered(s.env, realm);
  EXPECT_TRUE(StrictEquals(s.env, module_array(env_global), realm_array));
#if defined(NAPI_TEST_ENGINE_QUICKJS)
  EXPECT_TRUE(StrictEquals(s.env, SyntaxErrorPrototype(s.env, &bytecode_v1), env_syntax));
  EXPECT_TRUE(StrictEquals(s.env, module_array(nullptr), env_array));
#else
  EXPECT_TRUE(StrictEquals(s.env, SyntaxErrorPrototype(s.env, &bytecode_v1), realm_syntax));
  EXPECT_TRUE(StrictEquals(s.env, module_array(nullptr), realm_array));
#endif
}

// Version 1 of the bytecode options (main's layout) compiles for the current
// realm; version 2 for its `realm_or_null`, NULL being the env's own.
TEST_F(Test71ContextifyCompat, BytecodeOptionsOfBothVersions) {
  EnvScope s(runtime_.get());

  napi_value a_global = nullptr;
  napi_value b_global = nullptr;
  unofficial_napi_realm a = FreshRealm(s.env, &a_global);
  unofficial_napi_realm b = FreshRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  napi_value env_syntax = RunIn(s.env, nullptr, "SyntaxError.prototype");
  napi_value a_syntax = RunIn(s.env, a, "SyntaxError.prototype");
  napi_value b_syntax = RunIn(s.env, b, "SyntaxError.prototype");
  ASSERT_FALSE(StrictEquals(s.env, a_syntax, env_syntax));

  MainBytecodeOpenOptionsV1 v1 = MainBytecodeOptions(s.env, "(");
  EXPECT_TRUE(StrictEquals(s.env, SyntaxErrorPrototype(s.env, &v1), env_syntax));
  {
    EnteredRealm entered(s.env, a);
    EXPECT_TRUE(StrictEquals(s.env, SyntaxErrorPrototype(s.env, &v1), a_syntax));
  }
  MainBytecodeOpenOptionsV1 ok_v1 = MainBytecodeOptions(s.env, "1 + 1");
  unofficial_napi_bytecode_open_result opened{};
  ASSERT_EQ(unofficial_napi_bytecode_open(
                s.env, reinterpret_cast<const unofficial_napi_bytecode_open_options*>(&ok_v1),
                &opened),
            napi_ok);
  ASSERT_NE(opened.bytecode, nullptr);
  EXPECT_EQ(unofficial_napi_bytecode_release(s.env, opened.bytecode), napi_ok);
  MainBytecodeOpenOptionsV1 short_v1 = v1;
  short_v1.size -= 1;
  EXPECT_EQ(unofficial_napi_bytecode_open(
                s.env, reinterpret_cast<const unofficial_napi_bytecode_open_options*>(&short_v1),
                &opened),
            napi_invalid_arg);

  unofficial_napi_bytecode_open_options v2{};
  std::memcpy(&v2, &v1, sizeof(v1));
  v2.size = sizeof(v2);
  v2.version = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION;
  v2.realm_or_null = b;
  EXPECT_TRUE(StrictEquals(s.env, SyntaxErrorPrototype(s.env, &v2), b_syntax));
  v2.realm_or_null = nullptr;
  {
    EnteredRealm entered(s.env, a);
    EXPECT_TRUE(StrictEquals(s.env, SyntaxErrorPrototype(s.env, &v2), env_syntax));
  }
  // A version 1 struct ends before the realm, which is not read even when the
  // struct is long enough to have one.
  unofficial_napi_bytecode_open_options long_v1 = v2;
  long_v1.version = 1;
  long_v1.realm_or_null = b;
  EXPECT_TRUE(StrictEquals(s.env, SyntaxErrorPrototype(s.env, &long_v1), env_syntax));

  unofficial_napi_bytecode_open_options invalid = v2;
  invalid.size = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_V1_SIZE;
  EXPECT_EQ(unofficial_napi_bytecode_open(s.env, &invalid, &opened), napi_invalid_arg);
  for (uint32_t version : {0u, 3u}) {
    invalid = v2;
    invalid.version = version;
    EXPECT_EQ(unofficial_napi_bytecode_open(s.env, &invalid, &opened), napi_invalid_arg);
  }
}

// Version 1 of the module options (main's layout) takes a context, nothing
// being the current realm. Version 2 takes `realm_or_null`, or still the
// deprecated context, nothing being the env's realm then.
TEST_F(Test71ContextifyCompat, ModuleOptionsOfBothVersions) {
  EnvScope s(runtime_.get());

  napi_value env_global = Global(s.env);
  napi_value a_global = nullptr;
  napi_value b_global = nullptr;
  unofficial_napi_realm a = FreshRealm(s.env, &a_global);
  unofficial_napi_realm b = FreshRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  napi_status status = napi_generic_failure;

  EXPECT_TRUE(StrictEquals(s.env, ModuleGlobal(s.env, MainModuleOptions(a_global), &status),
                           a_global));
  EXPECT_TRUE(StrictEquals(s.env, ModuleGlobal(s.env, MainModuleOptions(env_global), &status),
                           env_global));
  for (napi_value nothing : {static_cast<napi_value>(nullptr), Undefined(s.env)}) {
    EXPECT_TRUE(StrictEquals(s.env, ModuleGlobal(s.env, MainModuleOptions(nothing), &status),
                             env_global));
    EnteredRealm entered(s.env, a);
    EXPECT_TRUE(StrictEquals(s.env, ModuleGlobal(s.env, MainModuleOptions(nothing), &status),
                             a_global));
  }
  EXPECT_EQ(ModuleGlobal(s.env, MainModuleOptions(Object(s.env)), &status), nullptr);
  EXPECT_EQ(status, napi_invalid_arg);
  MainModuleCreateOptionsV1 short_v1 = MainModuleOptions(nullptr);
  short_v1.size -= 1;
  EXPECT_EQ(ModuleGlobal(s.env, short_v1, &status), nullptr);
  EXPECT_EQ(status, napi_invalid_arg);
  {
    // A version 1 struct ends before the realm, which is not read even when
    // the struct is long enough to have one.
    unofficial_napi_module_create_options long_v1 = ModuleOptions(b, Undefined(s.env));
    long_v1.version = 1;
    EnteredRealm entered(s.env, a);
    EXPECT_TRUE(StrictEquals(s.env, ModuleGlobal(s.env, long_v1, &status), a_global));
  }

  EXPECT_TRUE(StrictEquals(s.env, ModuleGlobal(s.env, ModuleOptions(a, nullptr), &status),
                           a_global));
  EXPECT_TRUE(StrictEquals(s.env, ModuleGlobal(s.env, ModuleOptions(a, Undefined(s.env)), &status),
                           a_global));
  // Old source built against this header: the version macro with a context.
  EXPECT_TRUE(StrictEquals(s.env, ModuleGlobal(s.env, ModuleOptions(nullptr, a_global), &status),
                           a_global));
  {
    EnteredRealm entered(s.env, a);
    EXPECT_TRUE(StrictEquals(
        s.env, ModuleGlobal(s.env, ModuleOptions(nullptr, Undefined(s.env)), &status),
        env_global));
  }
  // A realm and a context together are ambiguous.
  EXPECT_EQ(ModuleGlobal(s.env, ModuleOptions(a, b_global), &status), nullptr);
  EXPECT_EQ(status, napi_invalid_arg);
  EXPECT_EQ(ModuleGlobal(s.env, ModuleOptions(nullptr, Object(s.env)), &status), nullptr);
  EXPECT_EQ(status, napi_invalid_arg);
  unofficial_napi_module_create_options short_v2 = ModuleOptions(a, nullptr);
  short_v2.size = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_V1_SIZE;
  EXPECT_EQ(ModuleGlobal(s.env, short_v2, &status), nullptr);
  EXPECT_EQ(status, napi_invalid_arg);
}

namespace {

// How many napi_values `n` calls of `call` leave in the handle scope they
// run in. Whatever `call` uses is made before.
template <typename Call>
size_t ValuesLeft(napi_env env, Call call) {
  constexpr int kCalls = 16;
  napi_handle_scope scope = nullptr;
  EXPECT_EQ(napi_open_handle_scope(env, &scope), napi_ok);
  const size_t before = napi::testing::open_value_count(env);
  for (int i = 0; i < kCalls; ++i) call();
  const size_t after = napi::testing::open_value_count(env);
  EXPECT_EQ(napi_close_handle_scope(env, scope), napi_ok);
  return after - before;
}

}  // namespace

// Finding the realm a deprecated call names leaves no value behind: each
// call leaves in the caller's handle scope only what the realm API call it
// stands for leaves there (its result).
TEST_F(Test71ContextifyCompat, DeprecatedCallsLeaveOnlyTheirResults) {
  EnvScope s(runtime_.get());

  napi_value env_global = Global(s.env);
  napi_value undefined = Undefined(s.env);
  napi_value a_global = nullptr;
  unofficial_napi_realm a = FreshRealm(s.env, &a_global);
  ASSERT_NE(a, nullptr);
  napi_value filename = Str(s.env, "count.js");
  const unofficial_napi_js_source script =
      unofficial_napi_js_source_from_text(Str(s.env, "1"));
  const unofficial_napi_js_source body =
      unofficial_napi_js_source_from_text(Str(s.env, "return 1;"));
  auto run_in_context = [&](napi_value context) {
    return [&s, &script, filename, context] {
      napi_value result = nullptr;
      EXPECT_EQ(unofficial_napi_contextify_run_script(s.env, context, &script, filename, 0, 0, -1,
                                                      true, false, false, nullptr, &result),
                napi_ok);
    };
  };
  auto run_in_realm = [&](unofficial_napi_realm realm) {
    return [&s, &script, filename, realm] {
      napi_value result = nullptr;
      EXPECT_EQ(unofficial_napi_run_script(s.env, realm, &script, filename, 0, 0, -1, true, false,
                                           false, nullptr, &result),
                napi_ok);
    };
  };
  auto compile_in_context = [&](napi_value context) {
    return [&s, &body, filename, context] {
      napi_value result = nullptr;
      EXPECT_EQ(unofficial_napi_contextify_compile_function(s.env, &body, filename, 0, 0, context,
                                                            nullptr, nullptr, nullptr, &result),
                napi_ok);
    };
  };
  auto compile_in_realm = [&](unofficial_napi_realm realm) {
    return [&s, &body, filename, realm] {
      napi_value result = nullptr;
      EXPECT_EQ(unofficial_napi_compile_function(s.env, realm, &body, filename, 0, 0, nullptr,
                                                 nullptr, nullptr, &result),
                napi_ok);
    };
  };
  MainBytecodeOpenOptionsV1 bytecode_v1 = MainBytecodeOptions(s.env, "1 + 1");
  unofficial_napi_bytecode_open_options bytecode_v2{};
  std::memcpy(&bytecode_v2, &bytecode_v1, sizeof(bytecode_v1));
  bytecode_v2.size = sizeof(bytecode_v2);
  bytecode_v2.version = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION;
  auto open_bytecode = [&](const void* options) {
    return [&s, options] {
      unofficial_napi_bytecode_open_result opened{};
      EXPECT_EQ(unofficial_napi_bytecode_open(
                    s.env, static_cast<const unofficial_napi_bytecode_open_options*>(options),
                    &opened),
                napi_ok);
      EXPECT_EQ(unofficial_napi_bytecode_release(s.env, opened.bytecode), napi_ok);
    };
  };
  const unofficial_napi_js_source module_source =
      unofficial_napi_js_source_from_text(Str(s.env, "export const one = 1;"));
  napi_value wrapper = Object(s.env);
  napi_value url = Str(s.env, "count.mjs");
  auto create_module = [&](auto options) {
    options.kind = unofficial_napi_module_source_text;
    options.wrapper = wrapper;
    options.url = url;
    options.payload.source_text.source = &module_source;
    options.payload.source_text.host_defined_option_id = undefined;
    return [&s, options] {
      unofficial_napi_module_create_result created{};
      EXPECT_EQ(unofficial_napi_module_wrap_create(
                    s.env, reinterpret_cast<const unofficial_napi_module_create_options*>(&options),
                    &created),
                napi_ok);
      EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, created.module), napi_ok);
    };
  };

  // The env's realm current: no context, the env's global, a realm's global.
  for (napi_value context : {static_cast<napi_value>(nullptr), undefined, env_global}) {
    EXPECT_EQ(ValuesLeft(s.env, run_in_context(context)), ValuesLeft(s.env, run_in_realm(nullptr)));
    EXPECT_EQ(ValuesLeft(s.env, compile_in_context(context)),
              ValuesLeft(s.env, compile_in_realm(nullptr)));
  }
  EXPECT_EQ(ValuesLeft(s.env, run_in_context(a_global)), ValuesLeft(s.env, run_in_realm(a)));
  EXPECT_EQ(ValuesLeft(s.env, compile_in_context(a_global)), ValuesLeft(s.env, compile_in_realm(a)));
  EXPECT_EQ(ValuesLeft(s.env, open_bytecode(&bytecode_v1)),
            ValuesLeft(s.env, open_bytecode(&bytecode_v2)));
  EXPECT_EQ(ValuesLeft(s.env, create_module(MainModuleOptions(nullptr))),
            ValuesLeft(s.env, create_module(ModuleOptions(nullptr, nullptr))));
  EXPECT_EQ(ValuesLeft(s.env, create_module(MainModuleOptions(env_global))),
            ValuesLeft(s.env, create_module(ModuleOptions(nullptr, nullptr))));
  EXPECT_EQ(ValuesLeft(s.env, create_module(MainModuleOptions(a_global))),
            ValuesLeft(s.env, create_module(ModuleOptions(a, nullptr))));

  // Another realm current.
  EnteredRealm entered(s.env, a);
  bytecode_v2.realm_or_null = a;
  EXPECT_EQ(ValuesLeft(s.env, run_in_context(nullptr)), ValuesLeft(s.env, run_in_realm(a)));
  EXPECT_EQ(ValuesLeft(s.env, compile_in_context(nullptr)), ValuesLeft(s.env, compile_in_realm(a)));
  EXPECT_EQ(ValuesLeft(s.env, open_bytecode(&bytecode_v1)),
            ValuesLeft(s.env, open_bytecode(&bytecode_v2)));
  EXPECT_EQ(ValuesLeft(s.env, create_module(MainModuleOptions(nullptr))),
            ValuesLeft(s.env, create_module(ModuleOptions(a, nullptr))));
}

namespace {

struct TokenLog {
  napi_env env = nullptr;
  // 'a' assign, 'u' unassign, 'c' the cleanup hook.
  std::vector<std::pair<char, void*>> events;
  bool wrong_env = false;
};

void AssignToken(napi_env env, void* token, void* data) {
  auto* log = static_cast<TokenLog*>(data);
  log->wrong_env |= env != log->env;
  log->events.emplace_back('a', token);
}

void UnassignToken(napi_env env, void* token, void* data) {
  auto* log = static_cast<TokenLog*>(data);
  log->wrong_env |= env != log->env;
  log->events.emplace_back('u', token);
}

void RecordCleanup(void* data) {
  static_cast<TokenLog*>(data)->events.emplace_back('c', nullptr);
}

void MakeRealmInCleanup(void* data) {
  napi_env env = static_cast<TokenLog*>(data)->env;
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) return;
  napi_value global = nullptr;
  FreshRealm(env, &global);
  napi_close_handle_scope(env, scope);
}

}  // namespace

// The deprecated context-token hooks: V8 assigns a token when a realm is
// created and unassigns it once, when the collected realm's record is freed or
// when the env is released. QuickJS does not accept them, as in main.
TEST_F(Test71ContextifyCompat, ContextTokensFollowRealms) {
  napi_env env = nullptr;
  unofficial_napi_env_owner owner = nullptr;
  unofficial_napi_env_create_options create_options{};
  InitializeTestEnvCreateOptions(&create_options);
  ASSERT_EQ(unofficial_napi_create_env(NAPI_TEST_MODULE_API_VERSION, &create_options, &env, &owner),
            napi_ok);
  TokenLog log;
  log.env = env;

  // A realm made before the hooks are attached gets no token.
  napi_ref early = nullptr;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(env, &scope), napi_ok);
    napi_value global = nullptr;
    ASSERT_NE(FreshRealm(env, &global), nullptr);
    ASSERT_EQ(napi_create_reference(env, global, 1, &early), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(env, scope), napi_ok);
  }

  unofficial_napi_env_hooks hooks{};
  hooks.size = sizeof(hooks);
  hooks.version = UNOFFICIAL_NAPI_ENV_HOOKS_VERSION;
  hooks.data = &log;
  hooks.context_token_assign_callback = AssignToken;
  hooks.context_token_unassign_callback = UnassignToken;
  uint64_t accepted = 0;
  unofficial_napi_env_hooks invalid = hooks;
  invalid.size -= 1;
  EXPECT_EQ(unofficial_napi_attach_env(env, &invalid, &accepted), napi_invalid_arg);
  invalid = hooks;
  invalid.version = 2;
  EXPECT_EQ(unofficial_napi_attach_env(env, &invalid, &accepted), napi_invalid_arg);
  ASSERT_EQ(unofficial_napi_attach_env(env, &hooks, &accepted), napi_ok);
#if defined(NAPI_TEST_ENGINE_V8)
  EXPECT_EQ(accepted, static_cast<uint64_t>(unofficial_napi_env_hook_context_token_assign |
                                            unofficial_napi_env_hook_context_token_unassign));
#else
  EXPECT_EQ(accepted, 0u);
#endif
  ASSERT_EQ(napi_add_env_cleanup_hook(env, RecordCleanup, &log), napi_ok);
  ASSERT_EQ(napi_add_env_cleanup_hook(env, MakeRealmInCleanup, &log), napi_ok);

  // A realm that nothing keeps, and one from make_context kept until release.
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(env, &scope), napi_ok);
    napi_value global = nullptr;
    ASSERT_NE(FreshRealm(env, &global), nullptr);
    ASSERT_EQ(napi_close_handle_scope(env, scope), napi_ok);
  }
  napi_ref kept = nullptr;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(env, &scope), napi_ok);
    napi_value global = nullptr;
    ASSERT_EQ(MakeContext(env, Sym(env, "DONT_CONTEXTIFY"), &global), napi_ok);
    ASSERT_EQ(napi_create_reference(env, global, 1, &kept), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(env, scope), napi_ok);
  }

#if defined(NAPI_TEST_ENGINE_V8)
  ASSERT_EQ(log.events.size(), 2u);
  EXPECT_EQ(log.events[0].first, 'a');
  EXPECT_EQ(log.events[1].first, 'a');
  void* collected_token = log.events[0].second;
  void* kept_token = log.events[1].second;
  EXPECT_NE(collected_token, nullptr);
  EXPECT_NE(kept_token, nullptr);
  EXPECT_NE(collected_token, kept_token);

  // Collection frees the first realm's record, and with it its token.
  ASSERT_EQ(unofficial_napi_collect_garbage(env), napi_ok);
  ASSERT_EQ(log.events.size(), 3u);
  EXPECT_EQ(log.events[2], std::make_pair('u', collected_token));
#else
  ASSERT_EQ(unofficial_napi_collect_garbage(env), napi_ok);
  EXPECT_TRUE(log.events.empty());
#endif

  // Release unassigns the realm still alive before the cleanup hooks run, and
  // a realm a cleanup hook makes after them.
  ASSERT_EQ(unofficial_napi_release_env(owner, nullptr), napi_ok);
#if defined(NAPI_TEST_ENGINE_V8)
  ASSERT_EQ(log.events.size(), 7u);
  EXPECT_EQ(log.events[3], std::make_pair('u', kept_token));
  EXPECT_EQ(log.events[4].first, 'a');
  EXPECT_EQ(log.events[5].first, 'c');
  EXPECT_EQ(log.events[6], std::make_pair('u', log.events[4].second));
#else
  ASSERT_EQ(log.events.size(), 1u);
  EXPECT_EQ(log.events[0].first, 'c');
#endif
  EXPECT_FALSE(log.wrong_env);
}

// unofficial_napi_memory_pressure frees no realm record, so it runs no token
// callback: a realm its collection takes keeps its token until
// unofficial_napi_collect_garbage frees the record. Token hooks are V8's
// only.
TEST_F(Test71ContextifyCompat, MemoryPressureLeavesACollectedRealmsTokenToCollectGarbage) {
  EnvScope s(runtime_.get());
  TokenLog log;
  log.env = s.env;
  unofficial_napi_env_hooks hooks{};
  hooks.size = sizeof(hooks);
  hooks.version = UNOFFICIAL_NAPI_ENV_HOOKS_VERSION;
  hooks.data = &log;
  hooks.context_token_assign_callback = AssignToken;
  hooks.context_token_unassign_callback = UnassignToken;
  uint64_t accepted = 0;
  ASSERT_EQ(unofficial_napi_attach_env(s.env, &hooks, &accepted), napi_ok);

  // A realm that nothing keeps, and a weak reference that tells when its
  // global is collected.
  napi_ref weak = nullptr;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value global = nullptr;
    ASSERT_NE(FreshRealm(s.env, &global), nullptr);
    ASSERT_EQ(napi_create_reference(s.env, global, 0, &weak), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }
  const size_t assigned = log.events.size();

  ASSERT_EQ(unofficial_napi_memory_pressure(s.env, unofficial_napi_memory_pressure_critical),
            napi_ok);
  bool collected = false;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value global = nullptr;
    ASSERT_EQ(napi_get_reference_value(s.env, weak, &global), napi_ok);
    collected = global == nullptr;
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }
  EXPECT_TRUE(collected) << "critical pressure did not collect the realm's global";
  EXPECT_EQ(log.events.size(), assigned) << "memory pressure ran a token callback";

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
#if defined(NAPI_TEST_ENGINE_V8)
  ASSERT_EQ(assigned, 1u);
  ASSERT_EQ(log.events.size(), 2u);
  EXPECT_EQ(log.events[1], std::make_pair('u', log.events[0].second));
#else
  EXPECT_TRUE(log.events.empty());
#endif
  ASSERT_EQ(napi_delete_reference(s.env, weak), napi_ok);
  EXPECT_FALSE(log.wrong_env);
}

namespace {

// The context-token hooks at release, for realms made while the env is
// released. Each assign must have exactly one unassign, before the env is
// freed, and both run in the env's realm. A token stays the same for its realm
// and is not reused while assigned.
struct ReleaseTokens {
  napi_env env = nullptr;
  // The env's own global: a hook runs in the env's realm if it is the global.
  napi_ref env_global = nullptr;
  // 'a' assign, 'u' unassign, in order.
  std::vector<std::pair<char, void*>> events;
  // Tokens assigned and not yet unassigned.
  std::vector<void*> live;
  // Realms the test made, with the token each was given (its handle).
  std::vector<void*> made;
  int failed_makes = 0;
  bool wrong_env = false;
  bool wrong_realm = false;
  bool reused = false;
  bool unassigned_unknown = false;
  // Hooks that found an exception pending when they began.
  int pending_at_hook = 0;
  // How many more realms the unassign hook makes, one per call.
  int unassign_makes = 0;
  // How many more unassign hooks throw, one per call.
  int unassign_throws = 0;
  // How many more unassign hooks import() from a script, one per call, and
  // what became of each import(): "returned, <how its promise settled>".
  int unassign_imports = 0;
  std::vector<std::string> import_outcomes;
};

void CheckHookRealm(napi_env env, ReleaseTokens* log) {
  log->wrong_env |= env != log->env;
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) ++log->pending_at_hook;
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) {
    log->wrong_realm = true;
    return;
  }
  napi_value global = nullptr;
  napi_value env_global = nullptr;
  bool same = false;
  log->wrong_realm |= napi_get_global(env, &global) != napi_ok ||
                      napi_get_reference_value(env, log->env_global, &env_global) != napi_ok ||
                      env_global == nullptr ||
                      napi_strict_equals(env, global, env_global, &same) != napi_ok || !same;
  napi_close_handle_scope(env, scope);
}

// Imports from a script in the env's realm, then runs the microtasks: says
// what became of the import().
std::string ImportFromAScript(napi_env env) {
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) return "no handle scope";
  napi_value box = nullptr;
  std::string outcome;
  if (napi_run_script(env,
                      Str(env,
                          "(() => {"
                          "  const box = { sync: 'none', outcome: 'pending' };"
                          "  try {"
                          "    const promise = import('missing');"
                          "    box.sync = 'returned';"
                          "    promise.then((ns) => { box.outcome = 'resolved:' + (ns && ns.from); },"
                          "                 () => { box.outcome = 'rejected'; });"
                          "  } catch (e) { box.sync = 'threw: ' + e; }"
                          "  return box;"
                          "})()"),
                      &box) != napi_ok) {
    outcome = "script failed";
  } else {
    unofficial_napi_event_loop_checkpoint(env, unofficial_napi_event_loop_checkpoint_microtasks,
                                          true, nullptr);
    for (const char* name : {"sync", "outcome"}) {
      napi_value value = nullptr;
      char text[128] = {};
      size_t length = 0;
      if (napi_get_named_property(env, box, name, &value) != napi_ok ||
          napi_get_value_string_utf8(env, value, text, sizeof(text), &length) != napi_ok) {
        std::snprintf(text, sizeof(text), "<unreadable>");
        length = std::strlen(text);
      }
      if (!outcome.empty()) outcome += ", ";
      outcome.append(text, length);
    }
  }
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    napi_value ignored = nullptr;
    napi_get_and_clear_last_exception(env, &ignored);
    outcome += " (exception pending)";
  }
  napi_close_handle_scope(env, scope);
  return outcome;
}

void MakeReleaseRealm(ReleaseTokens* log) {
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(log->env, &scope) != napi_ok) {
    ++log->failed_makes;
    return;
  }
  unofficial_napi_realm realm = nullptr;
  napi_value global = nullptr;
  if (unofficial_napi_create_realm(log->env, nullptr, &realm, &global) == napi_ok &&
      realm != nullptr) {
    log->made.push_back(realm);
  } else {
    ++log->failed_makes;
  }
  napi_close_handle_scope(log->env, scope);
}

void AssignReleaseToken(napi_env env, void* token, void* data) {
  auto* log = static_cast<ReleaseTokens*>(data);
  CheckHookRealm(env, log);
  log->reused |= std::find(log->live.begin(), log->live.end(), token) != log->live.end();
  log->live.push_back(token);
  log->events.emplace_back('a', token);
}

void UnassignReleaseToken(napi_env env, void* token, void* data) {
  auto* log = static_cast<ReleaseTokens*>(data);
  CheckHookRealm(env, log);
  auto it = std::find(log->live.begin(), log->live.end(), token);
  if (it == log->live.end()) {
    log->unassigned_unknown = true;
  } else {
    log->live.erase(it);
  }
  log->events.emplace_back('u', token);
  if (log->unassign_makes > 0) {
    --log->unassign_makes;
    MakeReleaseRealm(log);
  }
  if (log->unassign_throws > 0) {
    --log->unassign_throws;
    napi_throw_error(env, nullptr, "unassign");
  }
  if (log->unassign_imports > 0) {
    --log->unassign_imports;
    log->import_outcomes.push_back(ImportFromAScript(env));
  }
}

void MakeRealmHook(void* data) {
  MakeReleaseRealm(static_cast<ReleaseTokens*>(data));
}

void FinalizeMakingRealm(node_api_basic_env, void*, void* hint) {
  MakeReleaseRealm(static_cast<ReleaseTokens*>(hint));
}

void FinalizeAddingRealmHook(node_api_basic_env basic_env, void*, void* hint) {
  napi_add_env_cleanup_hook(const_cast<napi_env>(basic_env), MakeRealmHook, hint);
}

void FinalizeInstanceDataMakingRealm(napi_env, void* data, void*) {
  MakeReleaseRealm(static_cast<ReleaseTokens*>(data));
}

// Makes an env whose token hooks record into `log`.
void AttachReleaseTokens(EnvScope& s, ReleaseTokens* log) {
  log->env = s.env;
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_create_reference(s.env, global, 1, &log->env_global), napi_ok);
  unofficial_napi_env_hooks hooks{};
  hooks.size = sizeof(hooks);
  hooks.version = UNOFFICIAL_NAPI_ENV_HOOKS_VERSION;
  hooks.data = log;
  hooks.context_token_assign_callback = AssignReleaseToken;
  hooks.context_token_unassign_callback = UnassignReleaseToken;
  uint64_t accepted = 0;
  ASSERT_EQ(unofficial_napi_attach_env(s.env, &hooks, &accepted), napi_ok);
#if defined(NAPI_TEST_ENGINE_V8)
  EXPECT_EQ(accepted, static_cast<uint64_t>(unofficial_napi_env_hook_context_token_assign |
                                            unofficial_napi_env_hook_context_token_unassign));
#else
  EXPECT_EQ(accepted, 0u);
#endif
}

// Sets `name` on the env's global, which keeps `value` until the release.
void KeepOnGlobal(napi_env env, const char* name, napi_value value) {
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(env, global, name, value), napi_ok);
}

void KeepFinalizedObject(napi_env env, napi_finalize finalize, ReleaseTokens* log) {
  napi_value object = nullptr;
  ASSERT_EQ(napi_create_object(env, &object), napi_ok);
  ASSERT_EQ(napi_add_finalizer(env, object, nullptr, finalize, log, nullptr), napi_ok);
  KeepOnGlobal(env, "finalized", object);
}

void KeepWrappedObject(napi_env env, napi_finalize finalize, ReleaseTokens* log) {
  static int native = 0;
  napi_value object = nullptr;
  ASSERT_EQ(napi_create_object(env, &object), napi_ok);
  ASSERT_EQ(napi_wrap(env, object, &native, finalize, log, nullptr), napi_ok);
  KeepOnGlobal(env, "wrapped", object);
}

void KeepExternalArrayBuffer(napi_env env, napi_finalize finalize, ReleaseTokens* log) {
  static char bytes[16] = {};
  napi_value array_buffer = nullptr;
  ASSERT_EQ(napi_create_external_arraybuffer(env, bytes, sizeof(bytes), finalize, log,
                                             &array_buffer),
            napi_ok);
  KeepOnGlobal(env, "external", array_buffer);
}

void ReleaseTokensEnv(EnvScope& s) {
  ASSERT_EQ(unofficial_napi_release_env(s.scope, nullptr), napi_ok);
  s.scope = nullptr;
  s.env = nullptr;
}

// V8: every token assigned was unassigned once, before the release returned,
// in the env's realm, and was the handle of a realm the test made. QuickJS
// calls neither hook.
void ExpectEveryTokenUnassignedOnce(const ReleaseTokens& log, size_t realms) {
  EXPECT_EQ(log.failed_makes, 0);
  EXPECT_EQ(log.made.size(), realms);
#if defined(NAPI_TEST_ENGINE_V8)
  size_t assigned = 0;
  size_t unassigned = 0;
  std::vector<void*> live;
  for (const auto& event : log.events) {
    if (event.first == 'a') {
      ++assigned;
      EXPECT_EQ(std::count(live.begin(), live.end(), event.second), 0);
      live.push_back(event.second);
    } else {
      ++unassigned;
      auto it = std::find(live.begin(), live.end(), event.second);
      ASSERT_NE(it, live.end());
      live.erase(it);
    }
  }
  EXPECT_EQ(assigned, realms);
  EXPECT_EQ(unassigned, realms);
  EXPECT_TRUE(live.empty());
  EXPECT_TRUE(log.live.empty());
  for (void* token : log.made) {
    EXPECT_NE(std::find_if(log.events.begin(), log.events.end(),
                           [&](const auto& event) {
                             return event.first == 'a' && event.second == token;
                           }),
              log.events.end());
  }
  EXPECT_FALSE(log.reused);
  EXPECT_FALSE(log.unassigned_unknown);
  EXPECT_FALSE(log.wrong_env);
  EXPECT_FALSE(log.wrong_realm);
#else
  EXPECT_TRUE(log.events.empty());
#endif
}

}  // namespace

TEST_F(Test71ContextifyCompat, TokenOfARealmARefFinalizerMakesAtReleaseIsUnassigned) {
  EnvScope s(runtime_.get());
  ReleaseTokens log;
  AttachReleaseTokens(s, &log);
  KeepFinalizedObject(s.env, FinalizeMakingRealm, &log);
  ReleaseTokensEnv(s);
  ExpectEveryTokenUnassignedOnce(log, 1);
}

TEST_F(Test71ContextifyCompat, TokenOfARealmAWrapFinalizerMakesAtReleaseIsUnassigned) {
  EnvScope s(runtime_.get());
  ReleaseTokens log;
  AttachReleaseTokens(s, &log);
  KeepWrappedObject(s.env, FinalizeMakingRealm, &log);
  ReleaseTokensEnv(s);
  ExpectEveryTokenUnassignedOnce(log, 1);
}

TEST_F(Test71ContextifyCompat, TokenOfARealmAnExternalBufferFinalizerMakesAtReleaseIsUnassigned) {
  EnvScope s(runtime_.get());
  ReleaseTokens log;
  AttachReleaseTokens(s, &log);
  KeepExternalArrayBuffer(s.env, FinalizeMakingRealm, &log);
  ReleaseTokensEnv(s);
  ExpectEveryTokenUnassignedOnce(log, 1);
}

TEST_F(Test71ContextifyCompat, TokenOfARealmTheInstanceDataFinalizerMakesAtReleaseIsUnassigned) {
  EnvScope s(runtime_.get());
  ReleaseTokens log;
  AttachReleaseTokens(s, &log);
  ASSERT_EQ(napi_set_instance_data(s.env, &log, FinalizeInstanceDataMakingRealm, nullptr),
            napi_ok);
  ReleaseTokensEnv(s);
  ExpectEveryTokenUnassignedOnce(log, 1);
}

// A hook a finalizer adds at release runs in a later round of the release.
TEST_F(Test71ContextifyCompat, TokenOfARealmALateHookMakesAtReleaseIsUnassigned) {
  EnvScope s(runtime_.get());
  ReleaseTokens log;
  AttachReleaseTokens(s, &log);
  KeepFinalizedObject(s.env, FinalizeAddingRealmHook, &log);
  ReleaseTokensEnv(s);
  ExpectEveryTokenUnassignedOnce(log, 1);
}

// The unassign hook of a realm alive at release makes a realm, and so does
// the unassign hook of that one.
TEST_F(Test71ContextifyCompat, TokenOfARealmAnUnassignHookMakesAtReleaseIsUnassigned) {
  EnvScope s(runtime_.get());
  ReleaseTokens log;
  AttachReleaseTokens(s, &log);
  MakeReleaseRealm(&log);
  log.unassign_makes = 2;
  ReleaseTokensEnv(s);
#if defined(NAPI_TEST_ENGINE_V8)
  ExpectEveryTokenUnassignedOnce(log, 3);
  EXPECT_EQ(log.unassign_makes, 0);
#else
  ExpectEveryTokenUnassignedOnce(log, 1);
#endif
}

// All of them in one release, with what each makes handled in turn: a realm
// alive when the release begins, one a cleanup hook makes, and those values'
// finalizers, a late hook, the instance-data finalizer and unassign hooks
// make.
TEST_F(Test71ContextifyCompat, TokensOfRealmsMadeThroughoutTheReleaseAreUnassignedOnce) {
  EnvScope s(runtime_.get());
  ReleaseTokens log;
  AttachReleaseTokens(s, &log);
  MakeReleaseRealm(&log);
  ASSERT_EQ(napi_add_env_cleanup_hook(s.env, MakeRealmHook, &log), napi_ok);
  KeepFinalizedObject(s.env, FinalizeMakingRealm, &log);
  KeepWrappedObject(s.env, FinalizeMakingRealm, &log);
  KeepExternalArrayBuffer(s.env, FinalizeMakingRealm, &log);
  napi_value late = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &late), napi_ok);
  ASSERT_EQ(napi_add_finalizer(s.env, late, nullptr, FinalizeAddingRealmHook, &log, nullptr),
            napi_ok);
  KeepOnGlobal(s.env, "late", late);
  ASSERT_EQ(napi_set_instance_data(s.env, &log, FinalizeInstanceDataMakingRealm, nullptr),
            napi_ok);
  log.unassign_makes = 3;
  ReleaseTokensEnv(s);
#if defined(NAPI_TEST_ENGINE_V8)
  ExpectEveryTokenUnassignedOnce(log, 10);
  EXPECT_EQ(log.unassign_makes, 0);
#else
  ExpectEveryTokenUnassignedOnce(log, 7);
#endif
  // The realm alive when the release began gave up its token first, before
  // the cleanup hooks ran.
  if (!log.events.empty()) {
    EXPECT_EQ(log.events[1], std::make_pair('u', log.made[0]));
  }
}

namespace {

// How many token events had happened when each finalizer ran.
struct EventsAtFinalizers {
  ReleaseTokens* log = nullptr;
  std::vector<size_t> seen;
};

void RecordEventsAtFinalizer(node_api_basic_env, void*, void* hint) {
  auto* state = static_cast<EventsAtFinalizers*>(hint);
  state->seen.push_back(state->log->events.size());
}

// A cleanup hook of the release's second pass: makes a realm, and a value
// whose finalizer runs in that pass.
void HookMakingRealmAndFinalizedValue(void* data) {
  auto* state = static_cast<EventsAtFinalizers*>(data);
  MakeReleaseRealm(state->log);
  napi_env env = state->log->env;
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) return;
  napi_value object = nullptr;
  napi_value global = nullptr;
  if (napi_create_object(env, &object) == napi_ok &&
      napi_add_finalizer(env, object, nullptr, RecordEventsAtFinalizer, state, nullptr) ==
          napi_ok &&
      napi_get_global(env, &global) == napi_ok) {
    napi_set_named_property(env, global, "finalized_late", object);
  }
  napi_close_handle_scope(env, scope);
}

void RecordEventsThenAddHook(node_api_basic_env basic_env, void* data, void* hint) {
  RecordEventsAtFinalizer(basic_env, data, hint);
  napi_add_env_cleanup_hook(const_cast<napi_env>(basic_env), HookMakingRealmAndFinalizedValue,
                            hint);
}

}  // namespace

// A realm a cleanup hook makes at release gives up its token after the hooks
// of that pass, before the pass's finalizers run, as on main (whose contexts'
// own cleanup hook unassigned them), and in a later pass too.
TEST_F(Test71ContextifyCompat, TokenOfARealmACleanupHookMakesIsUnassignedBeforeTheFinalizers) {
  EnvScope s(runtime_.get());
  ReleaseTokens log;
  AttachReleaseTokens(s, &log);
  EventsAtFinalizers state;
  state.log = &log;
  ASSERT_EQ(napi_add_env_cleanup_hook(s.env, MakeRealmHook, &log), napi_ok);
  napi_value object = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);
  ASSERT_EQ(napi_add_finalizer(s.env, object, nullptr, RecordEventsThenAddHook, &state, nullptr),
            napi_ok);
  KeepOnGlobal(s.env, "finalized", object);
  ReleaseTokensEnv(s);
  ExpectEveryTokenUnassignedOnce(log, 2);
  ASSERT_EQ(state.seen.size(), 2u);
#if defined(NAPI_TEST_ENGINE_V8)
  // First pass: the hook's realm is assigned and unassigned, then the
  // finalizer runs. Second pass: the same for the late hook's realm.
  ASSERT_EQ(log.events.size(), 4u);
  EXPECT_EQ(log.events[0], std::make_pair('a', log.made[0]));
  EXPECT_EQ(log.events[1], std::make_pair('u', log.made[0]));
  EXPECT_EQ(log.events[2], std::make_pair('a', log.made[1]));
  EXPECT_EQ(log.events[3], std::make_pair('u', log.made[1]));
  EXPECT_EQ(state.seen[0], 2u);
  EXPECT_EQ(state.seen[1], 4u);
#else
  EXPECT_EQ(state.seen[0], 0u);
  EXPECT_EQ(state.seen[1], 0u);
#endif
}

namespace {

// A cleanup hook with an exception pending makes a realm the collector frees
// at once, so its unassign hook runs inside the cleanup hook.
struct ThrowThenFree {
  ReleaseTokens* log = nullptr;
  bool pending_after_free = false;
  size_t events_before = 0;
  size_t events_after = 0;
};

void HookThrowingThenFreeingARealm(void* data) {
  auto* state = static_cast<ThrowThenFree*>(data);
  napi_env env = state->log->env;
  MakeReleaseRealm(state->log);
  napi_throw_error(env, nullptr, "hook");
  state->events_before = state->log->events.size();
  unofficial_napi_collect_garbage(env);
  state->events_after = state->log->events.size();
  napi_is_exception_pending(env, &state->pending_after_free);
}

}  // namespace

// What a token hook throws at release is dropped, as what a cleanup hook or a
// finalizer throws: nothing can catch it. A hook that runs inside other code
// (here collect_garbage, freeing a collected realm, in a cleanup hook) runs
// with no exception pending, and the one that code has pending is kept.
TEST_F(Test71ContextifyCompat, TokenHookExceptionsAtReleaseAreDropped) {
  EnvScope s(runtime_.get());
  ReleaseTokens log;
  AttachReleaseTokens(s, &log);
  MakeReleaseRealm(&log);
  MakeReleaseRealm(&log);
  log.unassign_throws = 1;
  ThrowThenFree state;
  state.log = &log;
  ASSERT_EQ(napi_add_env_cleanup_hook(s.env, HookThrowingThenFreeingARealm, &state), napi_ok);
  ReleaseTokensEnv(s);
  ExpectEveryTokenUnassignedOnce(log, 3);
  EXPECT_TRUE(state.pending_after_free);
  // The second unassign hook did not find the first one's exception, and the
  // one inside the cleanup hook ran with none pending either.
  EXPECT_EQ(log.pending_at_hook, 0);
#if defined(NAPI_TEST_ENGINE_V8)
  EXPECT_EQ(log.unassign_throws, 0);
  EXPECT_EQ(state.events_after, state.events_before + 1);
#endif
}

// An unassign callback the release runs after the module loader's cleanup
// hook (for the realm a later cleanup hook made, in the sweep after the
// hooks) imports from a script: import() returns a promise, which rejects,
// since on V8 the loader's hook has let go of the module hooks. The import
// callback those hooks installed, still the isolate's, found no loader state
// there and returned no promise with nothing thrown, and V8 aborts the process
// on that. Token hooks are V8's only, so on QuickJS no unassign callback runs.
TEST_F(Test71ContextifyCompat, AnImportAnUnassignHookMakesAfterTheLoadersCleanupHookRejects) {
  EnvScope s(runtime_.get());
  ReleaseTokens log;
  AttachReleaseTokens(s, &log);
  napi_value loader = nullptr;
  ASSERT_EQ(napi_run_script(s.env,
                            Str(s.env, "(id, specifier) => Promise.resolve({ from: 'hooks' })"),
                            &loader),
            napi_ok);
  unofficial_napi_module_hooks hooks{};
  hooks.size = sizeof(hooks);
  hooks.version = UNOFFICIAL_NAPI_MODULE_HOOKS_VERSION;
  hooks.import_module_dynamically = loader;
  ASSERT_EQ(unofficial_napi_module_wrap_set_hooks(s.env, &hooks), napi_ok);
  EXPECT_EQ(ImportFromAScript(s.env), "returned, resolved:hooks");
  // Added after the loader's cleanup hook, so it runs before it.
  ASSERT_EQ(napi_add_env_cleanup_hook(s.env, MakeRealmHook, &log), napi_ok);
  log.unassign_imports = 1;
  ReleaseTokensEnv(s);
  ExpectEveryTokenUnassignedOnce(log, 1);
#if defined(NAPI_TEST_ENGINE_V8)
  ASSERT_EQ(log.import_outcomes.size(), 1u);
  EXPECT_EQ(log.import_outcomes[0], "returned, rejected");
#else
  EXPECT_TRUE(log.import_outcomes.empty());
#endif
}

namespace {

// A finalizer of the release makes a realm and lets the collector free it, so
// that realm's unassign callback runs inside unofficial_napi_collect_garbage,
// outside the release's sweeps, and makes another realm. A finalizer of the
// same pass that runs after it records whether that realm still has its token.
struct UnassignOutsideASweep {
  ReleaseTokens* log = nullptr;
  // Token events while collect_garbage ran (V8: 'u' for the collected realm,
  // 'a' for the one its unassign callback made).
  size_t events_inside = 0;
  void* made_by_unassign = nullptr;
  bool freeing_ran = false;
  bool later_ran_after = false;
  bool assigned_at_later = false;
};

void FinalizerFreeingARealm(node_api_basic_env, void*, void* hint) {
  auto* state = static_cast<UnassignOutsideASweep*>(hint);
  ReleaseTokens* log = state->log;
  MakeReleaseRealm(log);
  const size_t made = log->made.size();
  const size_t events = log->events.size();
  log->unassign_makes = 1;
  unofficial_napi_collect_garbage(log->env);
  log->unassign_makes = 0;
  state->events_inside = log->events.size() - events;
  if (log->made.size() > made) state->made_by_unassign = log->made.back();
  state->freeing_ran = true;
}

void FinalizerCheckingTheToken(node_api_basic_env, void*, void* hint) {
  auto* state = static_cast<UnassignOutsideASweep*>(hint);
  state->later_ran_after = state->freeing_ran;
  state->assigned_at_later =
      state->made_by_unassign != nullptr &&
      std::find(state->log->live.begin(), state->log->live.end(), state->made_by_unassign) !=
          state->log->live.end();
}

}  // namespace

// A realm an unassign callback makes is unassigned in the same sweep only
// when a sweep of the release runs that callback. One that
// unofficial_napi_collect_garbage runs (here inside a finalizer of the
// release) is in no sweep: the realm it makes keeps its token while the
// pass's later finalizers run, until the next pass begins, as a realm a
// finalizer makes itself. Token hooks are V8's only.
TEST_F(Test71ContextifyCompat, ARealmAnUnassignCallbackMakesOutsideASweepWaitsForTheNextOne) {
  EnvScope s(runtime_.get());
  ReleaseTokens log;
  AttachReleaseTokens(s, &log);
  UnassignOutsideASweep state;
  state.log = &log;
  // Made first, so finalized after the other (most recent first).
  napi_value later = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &later), napi_ok);
  ASSERT_EQ(napi_add_finalizer(s.env, later, nullptr, FinalizerCheckingTheToken, &state, nullptr),
            napi_ok);
  KeepOnGlobal(s.env, "later", later);
  napi_value first = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &first), napi_ok);
  ASSERT_EQ(napi_add_finalizer(s.env, first, nullptr, FinalizerFreeingARealm, &state, nullptr),
            napi_ok);
  KeepOnGlobal(s.env, "first", first);
  ReleaseTokensEnv(s);
  EXPECT_TRUE(state.freeing_ran);
  EXPECT_TRUE(state.later_ran_after) << "the finalizers ran in another order";
#if defined(NAPI_TEST_ENGINE_V8)
  ExpectEveryTokenUnassignedOnce(log, 2);
  EXPECT_EQ(state.events_inside, 2u) << "the realm was not freed inside collect_garbage";
  ASSERT_NE(state.made_by_unassign, nullptr);
  EXPECT_TRUE(state.assigned_at_later);
#else
  ExpectEveryTokenUnassignedOnce(log, 1);
  EXPECT_EQ(state.events_inside, 0u);
  EXPECT_EQ(state.made_by_unassign, nullptr);
#endif
}
