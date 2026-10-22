#include "test_env.h"

#include "node_api.h"
#include "unofficial_napi.h"

#include <vector>

#if defined(NAPI_TEST_ENGINE_V8)
#include <v8.h>
#endif

class Test65UnofficialContextify : public FixtureTestBase {};

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

TEST_F(Test65UnofficialContextify, MakeRunRoundTrip) {
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
TEST_F(Test65UnofficialContextify, MakeContextPreservesThrownProxyException) {
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

TEST_F(Test65UnofficialContextify, SandboxGlobalThisIsNotEnumerableForDeepFreeze) {
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

TEST_F(Test65UnofficialContextify, CompileFunctionAndCachedData) {
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

TEST_F(Test65UnofficialContextify, ModuleStateIsOneAtomicSnapshot) {
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

TEST_F(Test65UnofficialContextify, SyncModuleErrorDoesNotLeaveUnhandledRejection) {
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

TEST_F(Test65UnofficialContextify, ModuleHooksUseOneVersionedConfiguration) {
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
TEST_F(Test65UnofficialContextify,
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

TEST_F(Test65UnofficialContextify, CompileFunctionDoesNotUseGlobalFunctionConstructor) {
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

TEST_F(Test65UnofficialContextify, CompileFunctionAcceptsHashbangBody) {
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

TEST_F(Test65UnofficialContextify, CompileFunctionRejectsBomBeforeHashbang) {
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

TEST_F(Test65UnofficialContextify, FunctionPrototypeSourceLocationsDoNotBlockStaticAssignments) {
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

TEST_F(Test65UnofficialContextify, CjsCompileAndSyntaxDetection) {
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

TEST_F(Test65UnofficialContextify, PrivateSymbolAcceptsAutoLength) {
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
TEST_F(Test65UnofficialContextify, PrepareStackTraceResultIsNotRewritten) {
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

// unofficial_napi_contextify_enter / unofficial_napi_contextify_leave select
// the realm used by ordinary N-API operations. V8 implements them; QuickJS
// reports them as unsupported.
#if defined(NAPI_TEST_ENGINE_V8)
namespace {

struct ContextScopeFixture {
  napi_value host_defined_option = nullptr;
  napi_value child = nullptr;
  unofficial_napi_context_scope leaked = 0;
};

napi_status RunInContext(napi_env env,
                         napi_value sandbox,
                         napi_value host_defined_option,
                         const char* code,
                         napi_value* result) {
  const unofficial_napi_js_source source = unofficial_napi_js_source_from_text(Str(env, code));
  return unofficial_napi_contextify_run_script(env,
                                               sandbox,
                                               &source,
                                               Str(env, "context-scope.js"),
                                               0,
                                               0,
                                               -1,
                                               true,
                                               false,
                                               false,
                                               host_defined_option,
                                               result);
}

// Runs a boolean expression in a context. A thrown exception counts as false
// and is cleared, so one failed expectation does not cascade.
bool ScriptIsTrue(napi_env env,
                  napi_value sandbox,
                  napi_value host_defined_option,
                  const char* code) {
  napi_value result = nullptr;
  bool value = false;
  if (RunInContext(env, sandbox, host_defined_option, code, &result) != napi_ok ||
      napi_get_value_bool(env, result, &value) != napi_ok) {
    bool pending = false;
    napi_value exception = nullptr;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_get_and_clear_last_exception(env, &exception);
    }
    return false;
  }
  return value;
}

bool StrictEquals(napi_env env, napi_value left, napi_value right) {
  bool same = false;
  return napi_strict_equals(env, left, right, &same) == napi_ok && same;
}

napi_value CurrentGlobal(napi_env env) {
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok) return nullptr;
  return global;
}

napi_value NativeGlobal(napi_env env, napi_callback_info /*info*/) {
  return CurrentGlobal(env);
}

napi_value NativeObject(napi_env env, napi_callback_info /*info*/) {
  napi_value value = nullptr;
  napi_create_object(env, &value);
  return value;
}

napi_value NativeArray(napi_env env, napi_callback_info /*info*/) {
  napi_value value = nullptr;
  napi_create_array(env, &value);
  return value;
}

napi_value NativeFunction(napi_env env, napi_callback_info /*info*/) {
  napi_value value = nullptr;
  napi_create_function(env, "nested", NAPI_AUTO_LENGTH, NativeObject, nullptr, &value);
  return value;
}

ContextScopeFixture* FixtureFrom(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data) != napi_ok) return nullptr;
  return static_cast<ContextScopeFixture*>(data);
}

// Re-enters the child context from a callback created in the parent context.
napi_value NativeReenter(napi_env env, napi_callback_info info) {
  ContextScopeFixture* fixture = FixtureFrom(env, info);
  napi_value child_passed = nullptr;
  napi_value object = nullptr;
  if (fixture == nullptr ||
      RunInContext(env,
                   fixture->child,
                   fixture->host_defined_option,
                   "realmName === 'child' && "
                   "Object.getPrototypeOf(nativeObject()) === Object.prototype",
                   &child_passed) != napi_ok ||
      napi_create_object(env, &object) != napi_ok ||
      napi_set_named_property(env, object, "childPassed", child_passed) != napi_ok) {
    return nullptr;
  }
  return object;
}

// Enters the child context and returns without leaving.
napi_value LeakScope(napi_env env, napi_callback_info info) {
  ContextScopeFixture* fixture = FixtureFrom(env, info);
  if (fixture == nullptr ||
      unofficial_napi_contextify_enter(env, fixture->child, &fixture->leaked) != napi_ok) {
    return nullptr;
  }
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

// Enters the child context, then throws without leaving.
napi_value ThrowInScope(napi_env env, napi_callback_info info) {
  ContextScopeFixture* fixture = FixtureFrom(env, info);
  napi_value value = nullptr;
  if (fixture == nullptr ||
      unofficial_napi_contextify_enter(env, fixture->child, &fixture->leaked) != napi_ok ||
      napi_create_int32(env, 73, &value) != napi_ok) {
    return nullptr;
  }
  napi_throw(env, value);
  return nullptr;
}

bool Install(napi_env env,
             napi_value target,
             const char* name,
             napi_callback callback,
             ContextScopeFixture* fixture) {
  napi_value function = nullptr;
  return napi_create_function(env, name, NAPI_AUTO_LENGTH, callback, fixture, &function) ==
             napi_ok &&
         napi_set_named_property(env, target, name, function) == napi_ok;
}

// Creates a context whose native functions are created while it is entered.
napi_value MakeRealm(napi_env env, const char* name, ContextScopeFixture* fixture) {
  napi_value sandbox = nullptr;
  napi_value ignored = nullptr;
  unofficial_napi_context_scope scope = 0;
  if (napi_create_object(env, &sandbox) != napi_ok ||
      napi_set_named_property(env, sandbox, "realmName", Str(env, name)) != napi_ok ||
      unofficial_napi_contextify_make_context(env,
                                              sandbox,
                                              Str(env, name),
                                              Str(env, "test://context-scope"),
                                              true,
                                              true,
                                              false,
                                              fixture->host_defined_option,
                                              &ignored) != napi_ok ||
      unofficial_napi_contextify_enter(env, sandbox, &scope) != napi_ok) {
    return nullptr;
  }
  bool installed = Install(env, sandbox, "nativeGlobal", NativeGlobal, fixture) &&
                   Install(env, sandbox, "nativeObject", NativeObject, fixture) &&
                   Install(env, sandbox, "nativeArray", NativeArray, fixture) &&
                   Install(env, sandbox, "nativeFunction", NativeFunction, fixture) &&
                   Install(env, sandbox, "nativeReenter", NativeReenter, fixture);
  if (unofficial_napi_contextify_leave(env, scope) != napi_ok || !installed) return nullptr;
  return sandbox;
}

napi_value ContextGlobal(napi_env env, napi_value sandbox, ContextScopeFixture* fixture) {
  napi_value global = nullptr;
  if (RunInContext(env, sandbox, fixture->host_defined_option, "globalThis", &global) !=
      napi_ok) {
    return nullptr;
  }
  return global;
}

struct TeardownCounters {
  unsigned cleanup_calls = 0;
  unsigned finalizer_calls = 0;
  unsigned instance_calls = 0;
  unsigned gc_calls = 0;
  unsigned rejected_entries = 0;
  unofficial_napi_context_scope gc_scope = 0;
};

TeardownCounters* g_teardown = nullptr;

void ExpectEntryRejected(napi_env env) {
  unofficial_napi_context_scope token = 123;
  if (unofficial_napi_contextify_enter(env, nullptr, &token) == napi_generic_failure &&
      token == 0) {
    ++g_teardown->rejected_entries;
  }
}

void CleanupHook(void* data) {
  ++g_teardown->cleanup_calls;
  ExpectEntryRejected(static_cast<napi_env>(data));
}

void TeardownFinalizer(napi_env env, void* /*data*/, void* /*hint*/) {
  ++g_teardown->finalizer_calls;
  ExpectEntryRejected(env);
}

void InstanceDataFinalizer(napi_env env, void* /*data*/, void* /*hint*/) {
  ++g_teardown->instance_calls;
  ExpectEntryRejected(env);
}

// Ordinary finalization may enter a context. Returning without leaving must
// unwind the scope before the next VM operation.
void GcFinalizer(napi_env env, void* /*data*/, void* /*hint*/) {
  ++g_teardown->gc_calls;
  if (unofficial_napi_contextify_enter(env, nullptr, &g_teardown->gc_scope) != napi_ok) {
    g_teardown->gc_scope = 0;
  }
}

}  // namespace

TEST_F(Test65UnofficialContextify, ContextScopeSelectsRealmForOrdinaryOperations) {
  EnvScope s(runtime_.get());
  ContextScopeFixture fixture;
  fixture.host_defined_option = Sym(s.env, "context-scope");
  napi_value root = CurrentGlobal(s.env);
  napi_value parent = MakeRealm(s.env, "parent", &fixture);
  fixture.child = MakeRealm(s.env, "child", &fixture);
  ASSERT_NE(parent, nullptr);
  ASSERT_NE(fixture.child, nullptr);
  napi_value parent_global = ContextGlobal(s.env, parent, &fixture);
  napi_value child_global = ContextGlobal(s.env, fixture.child, &fixture);
  ASSERT_NE(parent_global, nullptr);
  ASSERT_NE(child_global, nullptr);
  EXPECT_FALSE(StrictEquals(s.env, parent_global, child_global));
  EXPECT_FALSE(StrictEquals(s.env, parent_global, root));

  unofficial_napi_context_scope outer = 0;
  ASSERT_EQ(unofficial_napi_contextify_enter(s.env, parent, &outer), napi_ok);
  EXPECT_NE(outer, 0u);
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), parent_global));
  napi_value script_global = nullptr;
  ASSERT_EQ(napi_run_script(s.env, Str(s.env, "globalThis"), &script_global), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, script_global, parent_global));

  napi_value object = nullptr;
  napi_value array = nullptr;
  napi_value error = nullptr;
  napi_value promise = nullptr;
  napi_value constructor = nullptr;
  napi_deferred deferred = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);
  ASSERT_EQ(napi_create_array(s.env, &array), napi_ok);
  ASSERT_EQ(napi_create_error(s.env, nullptr, Str(s.env, "native error"), &error), napi_ok);
  ASSERT_EQ(napi_create_promise(s.env, &deferred, &promise), napi_ok);
  ASSERT_EQ(napi_resolve_deferred(s.env, deferred, object), napi_ok);
  ASSERT_EQ(napi_define_class(s.env,
                              "NativeClass",
                              NAPI_AUTO_LENGTH,
                              NativeObject,
                              nullptr,
                              0,
                              nullptr,
                              &constructor),
            napi_ok);
  napi_property_descriptor owner = {};
  owner.utf8name = "owner";
  owner.getter = NativeGlobal;
  ASSERT_EQ(napi_define_properties(s.env, object, 1, &owner), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, parent_global, "nativeAllocatedObject", object),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, parent_global, "nativeAllocatedArray", array),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, parent_global, "nativeAllocatedError", error),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, parent_global, "nativeAllocatedPromise", promise),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, parent_global, "NativeClass", constructor), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_leave(s.env, outer), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), root));

  EXPECT_TRUE(ScriptIsTrue(
      s.env, parent, fixture.host_defined_option,
      "Object.getPrototypeOf(nativeAllocatedObject) === Object.prototype && "
      "Object.getPrototypeOf(nativeAllocatedArray) === Array.prototype && "
      "Object.getPrototypeOf(nativeAllocatedError) === Error.prototype && "
      "Object.getPrototypeOf(nativeAllocatedPromise) === Promise.prototype && "
      "Object.getPrototypeOf(NativeClass) === Function.prototype && "
      "Object.getPrototypeOf(NativeClass.prototype) === Object.prototype && "
      "nativeAllocatedObject.owner === globalThis"));

  // Values retained past their handle scope keep their creation realm.
  napi_handle_scope handles = nullptr;
  napi_ref reference = nullptr;
  napi_value retained = nullptr;
  ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_enter(s.env, parent, &outer), napi_ok);
  ASSERT_EQ(napi_create_object(s.env, &retained), napi_ok);
  ASSERT_EQ(napi_create_reference(s.env, retained, 1, &reference), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_leave(s.env, outer), napi_ok);
  ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  ASSERT_EQ(napi_get_reference_value(s.env, reference, &retained), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, parent_global, "retainedNativeObject", retained),
            napi_ok);
  EXPECT_TRUE(ScriptIsTrue(s.env, parent, fixture.host_defined_option,
                           "Object.getPrototypeOf(retainedNativeObject) === Object.prototype"));
  ASSERT_EQ(napi_delete_reference(s.env, reference), napi_ok);
}

TEST_F(Test65UnofficialContextify, ContextScopeNativeCallbacksUseCreationRealm) {
  EnvScope s(runtime_.get());
  ContextScopeFixture fixture;
  fixture.host_defined_option = Sym(s.env, "context-scope");
  napi_value root = CurrentGlobal(s.env);
  napi_value parent = MakeRealm(s.env, "parent", &fixture);
  fixture.child = MakeRealm(s.env, "child", &fixture);
  ASSERT_NE(parent, nullptr);
  ASSERT_NE(fixture.child, nullptr);
  napi_value parent_global = ContextGlobal(s.env, parent, &fixture);
  napi_value child_global = ContextGlobal(s.env, fixture.child, &fixture);
  ASSERT_NE(parent_global, nullptr);
  ASSERT_NE(child_global, nullptr);

  const napi_value host = fixture.host_defined_option;
  EXPECT_TRUE(ScriptIsTrue(s.env, parent, host, "realmName === 'parent'"));
  EXPECT_TRUE(ScriptIsTrue(s.env, parent, host, "nativeGlobal() === globalThis"));
  EXPECT_TRUE(ScriptIsTrue(
      s.env, parent, host, "Object.getPrototypeOf(nativeObject()) === Object.prototype"));
  EXPECT_TRUE(ScriptIsTrue(
      s.env, parent, host, "Object.getPrototypeOf(nativeArray()) === Array.prototype"));
  EXPECT_TRUE(ScriptIsTrue(
      s.env, parent, host, "Object.getPrototypeOf(nativeFunction()) === Function.prototype"));
  EXPECT_TRUE(ScriptIsTrue(s.env, parent, host,
                           "(() => { const result = nativeReenter(); return result.childPassed && "
                           "Object.getPrototypeOf(result) === Object.prototype; })()"));

  // A callback keeps its creation realm when another realm calls it.
  napi_value parent_native_global = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, parent_global, "nativeGlobal", &parent_native_global),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, child_global, "parentNativeGlobal",
                                    parent_native_global),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, child_global, "parentGlobal", parent_global), napi_ok);
  EXPECT_TRUE(ScriptIsTrue(s.env, fixture.child, host, "parentNativeGlobal() === parentGlobal"));

  // Microtask jobs dispatch native callbacks in their creation realm too.
  napi_value ignored = nullptr;
  ASSERT_EQ(RunInContext(s.env, fixture.child, host,
                         "globalThis.jobPassed = false; Promise.resolve().then(() => { "
                         "globalThis.jobPassed = nativeGlobal() === globalThis; });",
                         &ignored),
            napi_ok);
  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                  s.env, unofficial_napi_event_loop_checkpoint_microtasks, false, nullptr),
              napi_ok);
  }
  EXPECT_TRUE(ScriptIsTrue(s.env, fixture.child, host, "jobPassed"));
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), root));
}

TEST_F(Test65UnofficialContextify, ContextScopeTokensAreStrictlyLifo) {
  EnvScope s(runtime_.get());
  ContextScopeFixture fixture;
  fixture.host_defined_option = Sym(s.env, "context-scope");
  napi_value root = CurrentGlobal(s.env);
  napi_value parent = MakeRealm(s.env, "parent", &fixture);
  napi_value child = MakeRealm(s.env, "child", &fixture);
  ASSERT_NE(parent, nullptr);
  ASSERT_NE(child, nullptr);
  napi_value parent_global = ContextGlobal(s.env, parent, &fixture);
  napi_value child_global = ContextGlobal(s.env, child, &fixture);

  unofficial_napi_context_scope outer = 0;
  unofficial_napi_context_scope inner = 0;
  unofficial_napi_context_scope newer = 0;
  ASSERT_EQ(unofficial_napi_contextify_enter(s.env, parent, &outer), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_enter(s.env, child, &inner), napi_ok);
  EXPECT_NE(outer, inner);

  // Leaving out of order fails and leaves the selection unchanged.
  EXPECT_EQ(unofficial_napi_contextify_leave(s.env, outer), napi_invalid_arg);
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), child_global));

  ASSERT_EQ(unofficial_napi_contextify_leave(s.env, inner), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), parent_global));

  // Tokens are never reused, so a closed token cannot alias a newer scope.
  ASSERT_EQ(unofficial_napi_contextify_enter(s.env, child, &newer), napi_ok);
  EXPECT_NE(newer, inner);
  EXPECT_EQ(unofficial_napi_contextify_leave(s.env, inner), napi_invalid_arg);
  ASSERT_EQ(unofficial_napi_contextify_leave(s.env, newer), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_leave(s.env, outer), napi_ok);
  EXPECT_EQ(unofficial_napi_contextify_leave(s.env, outer), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_contextify_leave(s.env, 0), napi_invalid_arg);
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), root));
}

TEST_F(Test65UnofficialContextify, ContextScopeSelectsRootAndValidatesArguments) {
  EnvScope s(runtime_.get());
  ContextScopeFixture fixture;
  fixture.host_defined_option = Sym(s.env, "context-scope");
  napi_value root = CurrentGlobal(s.env);
  napi_value parent = MakeRealm(s.env, "parent", &fixture);
  ASSERT_NE(parent, nullptr);

  // A null or undefined sandbox selects the environment's root context.
  unofficial_napi_context_scope outer = 0;
  unofficial_napi_context_scope inner = 0;
  napi_value undefined = nullptr;
  napi_value null_value = nullptr;
  ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
  ASSERT_EQ(napi_get_null(s.env, &null_value), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_enter(s.env, parent, &outer), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_enter(s.env, nullptr, &inner), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), root));
  ASSERT_EQ(unofficial_napi_contextify_leave(s.env, inner), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_enter(s.env, undefined, &inner), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), root));
  ASSERT_EQ(unofficial_napi_contextify_leave(s.env, inner), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_enter(s.env, null_value, &inner), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), root));
  ASSERT_EQ(unofficial_napi_contextify_leave(s.env, inner), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_leave(s.env, outer), napi_ok);

  // An object that was never contextified does not name a context.
  napi_value plain = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &plain), napi_ok);
  unofficial_napi_context_scope token = 123;
  EXPECT_EQ(unofficial_napi_contextify_enter(s.env, plain, &token), napi_invalid_arg);
  EXPECT_EQ(token, 0u);
  EXPECT_EQ(unofficial_napi_contextify_enter(s.env, parent, nullptr), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_contextify_enter(nullptr, parent, &token), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_contextify_leave(nullptr, 1), napi_invalid_arg);
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), root));
}

TEST_F(Test65UnofficialContextify, ContextScopeLeavePreservesPendingException) {
  EnvScope s(runtime_.get());
  ContextScopeFixture fixture;
  fixture.host_defined_option = Sym(s.env, "context-scope");
  napi_value root = CurrentGlobal(s.env);
  napi_value parent = MakeRealm(s.env, "parent", &fixture);
  napi_value child = MakeRealm(s.env, "child", &fixture);
  ASSERT_NE(parent, nullptr);
  ASSERT_NE(child, nullptr);

  unofficial_napi_context_scope outer = 0;
  napi_value ignored = nullptr;
  ASSERT_EQ(unofficial_napi_contextify_enter(s.env, parent, &outer), napi_ok);
  EXPECT_EQ(RunInContext(s.env, child, fixture.host_defined_option, "throw 91", &ignored),
            napi_pending_exception);
  ASSERT_EQ(unofficial_napi_contextify_leave(s.env, outer), napi_ok);

  bool pending = false;
  ASSERT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
  EXPECT_TRUE(pending);
  napi_value exception = nullptr;
  int32_t thrown = 0;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &exception), napi_ok);
  ASSERT_EQ(napi_get_value_int32(s.env, exception, &thrown), napi_ok);
  EXPECT_EQ(thrown, 91);
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), root));
}

TEST_F(Test65UnofficialContextify, ContextScopeCallbackReclaimsUnbalancedScopes) {
  EnvScope s(runtime_.get());
  ContextScopeFixture fixture;
  fixture.host_defined_option = Sym(s.env, "context-scope");
  napi_value root = CurrentGlobal(s.env);
  napi_value parent = MakeRealm(s.env, "parent", &fixture);
  fixture.child = MakeRealm(s.env, "child", &fixture);
  ASSERT_NE(parent, nullptr);
  ASSERT_NE(fixture.child, nullptr);
  napi_value parent_global = ContextGlobal(s.env, parent, &fixture);
  ASSERT_NE(parent_global, nullptr);

  unofficial_napi_context_scope scope = 0;
  ASSERT_EQ(unofficial_napi_contextify_enter(s.env, parent, &scope), napi_ok);
  ASSERT_TRUE(Install(s.env, parent_global, "leakScope", LeakScope, &fixture));
  ASSERT_TRUE(Install(s.env, parent_global, "throwInScope", ThrowInScope, &fixture));
  ASSERT_EQ(unofficial_napi_contextify_leave(s.env, scope), napi_ok);

  // A scope a callback leaves open is closed when the callback returns.
  EXPECT_TRUE(ScriptIsTrue(s.env, parent, fixture.host_defined_option,
                           "leakScope(); nativeGlobal() === globalThis"));
  EXPECT_NE(fixture.leaked, 0u);
  EXPECT_EQ(unofficial_napi_contextify_leave(s.env, fixture.leaked), napi_invalid_arg);

  // The same holds when the callback throws.
  fixture.leaked = 0;
  EXPECT_TRUE(ScriptIsTrue(s.env, parent, fixture.host_defined_option,
                           "(() => { try { throwInScope(); } catch (error) { "
                           "return error === 73 && nativeGlobal() === globalThis; } "
                           "return false; })()"));
  EXPECT_NE(fixture.leaked, 0u);
  EXPECT_EQ(unofficial_napi_contextify_leave(s.env, fixture.leaked), napi_invalid_arg);
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), root));
}

TEST_F(Test65UnofficialContextify, ContextScopeTokensBelongToOneEnvironment) {
  EnvScope s(runtime_.get());
  ContextScopeFixture fixture;
  fixture.host_defined_option = Sym(s.env, "context-scope");
  napi_value parent = MakeRealm(s.env, "parent", &fixture);
  ASSERT_NE(parent, nullptr);
  napi_value parent_global = ContextGlobal(s.env, parent, &fixture);

  unofficial_napi_context_scope outer = 0;
  unofficial_napi_context_scope inner = 0;
  ASSERT_EQ(unofficial_napi_contextify_enter(s.env, parent, &outer), napi_ok);
  {
    EnvScope foreign(runtime_.get());
    ASSERT_EQ(unofficial_napi_contextify_enter(foreign.env, nullptr, &inner), napi_ok);
    EXPECT_NE(inner, outer);
    EXPECT_EQ(unofficial_napi_contextify_leave(foreign.env, outer), napi_invalid_arg);
    ASSERT_EQ(unofficial_napi_contextify_leave(foreign.env, inner), napi_ok);
  }
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), parent_global));
  EXPECT_EQ(unofficial_napi_contextify_leave(s.env, inner), napi_invalid_arg);
  ASSERT_EQ(unofficial_napi_contextify_leave(s.env, outer), napi_ok);
}

// Scopes left outside any handle scope must not grow the environment's root
// handle scope, which stays open for the environment's lifetime.
TEST_F(Test65UnofficialContextify, ContextScopeLeaveDoesNotRetainHandles) {
  EnvScope s(runtime_.get());
  ContextScopeFixture fixture;
  fixture.host_defined_option = Sym(s.env, "context-scope");
  napi_value parent = MakeRealm(s.env, "parent", &fixture);
  ASSERT_NE(parent, nullptr);
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  ASSERT_NE(isolate, nullptr);

  const int before = v8::HandleScope::NumberOfHandles(isolate);
  for (int i = 0; i < 10000; ++i) {
    unofficial_napi_context_scope outer = 0;
    unofficial_napi_context_scope inner = 0;
    ASSERT_EQ(unofficial_napi_contextify_enter(s.env, parent, &outer), napi_ok);
    ASSERT_EQ(unofficial_napi_contextify_enter(s.env, nullptr, &inner), napi_ok);
    ASSERT_EQ(unofficial_napi_contextify_leave(s.env, inner), napi_ok);
    ASSERT_EQ(unofficial_napi_contextify_leave(s.env, outer), napi_ok);
  }
  EXPECT_LT(v8::HandleScope::NumberOfHandles(isolate) - before, 16);
}

TEST_F(Test65UnofficialContextify, ContextScopeDepthIsBounded) {
  EnvScope s(runtime_.get());
  napi_value root = CurrentGlobal(s.env);
  constexpr unsigned kMaxDepth = 1024;
  std::vector<unofficial_napi_context_scope> scopes(kMaxDepth);
  for (unsigned i = 0; i < kMaxDepth; ++i) {
    ASSERT_EQ(unofficial_napi_contextify_enter(s.env, nullptr, &scopes[i]), napi_ok) << i;
  }
  unofficial_napi_context_scope overflow = 123;
  EXPECT_EQ(unofficial_napi_contextify_enter(s.env, nullptr, &overflow), napi_generic_failure);
  EXPECT_EQ(overflow, 0u);
  for (unsigned i = kMaxDepth; i > 0; --i) {
    ASSERT_EQ(unofficial_napi_contextify_leave(s.env, scopes[i - 1]), napi_ok) << i;
  }
  EXPECT_TRUE(StrictEquals(s.env, CurrentGlobal(s.env), root));
}

// Releasing an environment closes scopes left open, and cleanup hooks and
// teardown finalizers cannot enter a context. A finalizer that runs during
// ordinary garbage collection may, and its unbalanced scope is reclaimed.
TEST_F(Test65UnofficialContextify, ContextScopeTeardownReclaimsAndRejectsEntry) {
  TeardownCounters counters;
  g_teardown = &counters;
  for (unsigned run = 0; run < 2; ++run) {
    napi_env env = nullptr;
    unofficial_napi_env_owner owner = nullptr;
    unofficial_napi_env_create_options options{};
    InitializeTestEnvCreateOptions(&options);
    ASSERT_EQ(unofficial_napi_create_env(NAPI_TEST_MODULE_API_VERSION, &options, &env, &owner),
              napi_ok);

    napi_handle_scope handles = nullptr;
    napi_value transient = nullptr;
    ASSERT_EQ(napi_open_handle_scope(env, &handles), napi_ok);
    ASSERT_EQ(napi_create_object(env, &transient), napi_ok);
    ASSERT_EQ(napi_add_finalizer(env, transient, nullptr, GcFinalizer, nullptr, nullptr),
              napi_ok);
    ASSERT_EQ(napi_close_handle_scope(env, handles), napi_ok);
    for (unsigned attempt = 0; attempt < 3 && counters.gc_calls != run + 1; ++attempt) {
      ASSERT_EQ(unofficial_napi_collect_garbage(env), napi_ok);
      ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                    env, unofficial_napi_event_loop_checkpoint_host_tasks, false, nullptr),
                napi_ok);
    }
    ASSERT_EQ(counters.gc_calls, run + 1);
    EXPECT_NE(counters.gc_scope, 0u);
    EXPECT_EQ(unofficial_napi_contextify_leave(env, counters.gc_scope), napi_invalid_arg);

    ASSERT_EQ(napi_add_env_cleanup_hook(env, CleanupHook, env), napi_ok);
    ASSERT_EQ(napi_set_instance_data(env, nullptr, InstanceDataFinalizer, nullptr), napi_ok);
    napi_value object = nullptr;
    napi_ref reference = nullptr;
    uint32_t count = 0;
    ASSERT_EQ(napi_create_object(env, &object), napi_ok);
    ASSERT_EQ(napi_add_finalizer(env, object, nullptr, TeardownFinalizer, nullptr, &reference),
              napi_ok);
    ASSERT_EQ(napi_reference_ref(env, reference, &count), napi_ok);

    // Leave the root and, innermost, a contextified realm entered. Release
    // must close both before it exits the root context, or V8 aborts.
    napi_value sandbox = nullptr;
    napi_value ignored = nullptr;
    napi_value host_defined_option = nullptr;
    ASSERT_EQ(napi_create_object(env, &sandbox), napi_ok);
    ASSERT_EQ(napi_create_symbol(env, nullptr, &host_defined_option), napi_ok);
    ASSERT_EQ(unofficial_napi_contextify_make_context(env,
                                                      sandbox,
                                                      Str(env, "teardown"),
                                                      Str(env, "test://teardown"),
                                                      true,
                                                      true,
                                                      false,
                                                      host_defined_option,
                                                      &ignored),
              napi_ok);
    unofficial_napi_context_scope outer = 0;
    unofficial_napi_context_scope inner = 0;
    ASSERT_EQ(unofficial_napi_contextify_enter(env, nullptr, &outer), napi_ok);
    ASSERT_EQ(unofficial_napi_contextify_enter(env, sandbox, &inner), napi_ok);
    ASSERT_EQ(unofficial_napi_release_env(owner, nullptr), napi_ok);
  }
  g_teardown = nullptr;
  EXPECT_EQ(counters.cleanup_calls, 2u);
  EXPECT_EQ(counters.finalizer_calls, 2u);
  EXPECT_EQ(counters.instance_calls, 2u);
  EXPECT_EQ(counters.rejected_entries, 6u);
}
#endif

#if defined(NAPI_TEST_ENGINE_QUICKJS)
TEST_F(Test65UnofficialContextify, ContextScopeIsUnsupported) {
  EnvScope s(runtime_.get());

  napi_value sandbox = nullptr;
  napi_value ignored = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_make_context(s.env,
                                                    sandbox,
                                                    Str(s.env, "ctx"),
                                                    Str(s.env, "test://origin"),
                                                    true,
                                                    true,
                                                    false,
                                                    Sym(s.env, "hdo"),
                                                    &ignored),
            napi_ok);

  unofficial_napi_context_scope token = 123;
  EXPECT_EQ(unofficial_napi_contextify_enter(s.env, nullptr, &token), napi_generic_failure);
  EXPECT_EQ(token, 0u);
  token = 123;
  EXPECT_EQ(unofficial_napi_contextify_enter(s.env, sandbox, &token), napi_generic_failure);
  EXPECT_EQ(token, 0u);
  EXPECT_EQ(unofficial_napi_contextify_leave(s.env, 1), napi_generic_failure);
  EXPECT_EQ(unofficial_napi_contextify_enter(s.env, nullptr, nullptr), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_contextify_enter(nullptr, nullptr, &token), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_contextify_leave(nullptr, 1), napi_invalid_arg);
}
#endif
