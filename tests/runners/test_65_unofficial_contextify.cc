#include "test_env.h"

#include "unofficial_napi.h"

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

namespace {

napi_value RunIn(napi_env env, napi_value sandbox, const char* code) {
  const unofficial_napi_js_source source =
      unofficial_napi_js_source_from_text(Str(env, code));
  napi_value result = nullptr;
  if (unofficial_napi_contextify_run_script(env, sandbox, &source, Str(env, "ctx.js"), 0, 0, -1,
                                            true, false, false, Sym(env, "hdo"),
                                            &result) != napi_ok) {
    return nullptr;
  }
  return result;
}

bool IsCollected(napi_env env, napi_ref ref) {
  napi_value value = nullptr;
  return napi_get_reference_value(env, ref, &value) == napi_ok && value == nullptr;
}

}  // namespace

// A context made for a sandbox is collected with it: the provider must not
// keep every context alive until the environment is released.
TEST_F(Test65UnofficialContextify, ContextIsCollectedWithItsSandbox) {
  EnvScope s(runtime_.get());

  // Object sandboxes, with and without their own microtask queue, and
  // vanilla contexts (a symbol), whose key is the context's global.
  constexpr int kContexts = 9;
  napi_ref keys[kContexts] = {};
  for (int i = 0; i < kContexts; ++i) {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
#if defined(NAPI_TEST_ENGINE_V8)
    const bool vanilla = i % 3 == 2;
#else
    const bool vanilla = false;  // QuickJS has no vanilla contexts.
#endif
    napi_value sandbox = nullptr;
    if (vanilla) {
      sandbox = Sym(s.env, "vanilla");
    } else {
      ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    }
    napi_value key = nullptr;
    ASSERT_EQ(unofficial_napi_contextify_make_context(s.env, sandbox, Str(s.env, "ctx"),
                                                      Str(s.env, "test://origin"), true, true,
                                                      i % 3 == 0, Sym(s.env, "hdo"), &key),
              napi_ok);
    ASSERT_NE(RunIn(s.env, key, "globalThis.payload = new Array(4096).fill(1); 1"), nullptr);
    ASSERT_EQ(napi_create_reference(s.env, key, 0, &keys[i]), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  // A sandbox still referenced keeps its context.
  napi_ref kept = nullptr;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value sandbox = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    napi_value key = nullptr;
    ASSERT_EQ(unofficial_napi_contextify_make_context(s.env, sandbox, Str(s.env, "kept"),
                                                      Str(s.env, "test://origin"), true, true,
                                                      true, Sym(s.env, "hdo"), &key),
              napi_ok);
    ASSERT_NE(RunIn(s.env, sandbox, "globalThis.marker = 42; 1"), nullptr);
    ASSERT_EQ(napi_create_reference(s.env, sandbox, 1, &kept), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  for (napi_ref key : keys) {
    EXPECT_TRUE(IsCollected(s.env, key));
    ASSERT_EQ(napi_delete_reference(s.env, key), napi_ok);
  }

  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_get_reference_value(s.env, kept, &sandbox), napi_ok);
  ASSERT_NE(sandbox, nullptr);
  napi_value marker = RunIn(s.env, sandbox, "marker");
  ASSERT_NE(marker, nullptr);
  int32_t value = 0;
  ASSERT_EQ(napi_get_value_int32(s.env, marker, &value), napi_ok);
  EXPECT_EQ(value, 42);
  ASSERT_EQ(napi_delete_reference(s.env, kept), napi_ok);
}

// A context stays usable, with its own microtask queue, while anything made
// in it is reachable, even after its sandbox handle is dropped and the
// provider releases collected contexts.
TEST_F(Test65UnofficialContextify, ContextOutlivesSandboxHandleWhileReachable) {
  EnvScope s(runtime_.get());

  napi_ref functions = nullptr;
  napi_ref weak_sandbox = nullptr;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value sandbox = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    napi_value key = nullptr;
    ASSERT_EQ(unofficial_napi_contextify_make_context(s.env, sandbox, Str(s.env, "ctx"),
                                                      Str(s.env, "test://origin"), true, true,
                                                      true, Sym(s.env, "hdo"), &key),
              napi_ok);
    napi_value made = RunIn(s.env, sandbox,
                            "globalThis.box = { n: 0 };"
                            "[() => { Promise.resolve().then(() => { box.n++; }); return 7; }]");
    ASSERT_NE(made, nullptr);
    ASSERT_EQ(napi_create_reference(s.env, made, 1, &functions), napi_ok);
    ASSERT_EQ(napi_create_reference(s.env, sandbox, 0, &weak_sandbox), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  // Collect, then make more contexts, which lets the provider release
  // records of collected contexts.
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  for (int i = 0; i < 4; ++i) {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value other = nullptr;
    napi_value key = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &other), napi_ok);
    ASSERT_EQ(unofficial_napi_contextify_make_context(s.env, other, Str(s.env, "other"),
                                                      Str(s.env, "test://origin"), true, true,
                                                      true, Sym(s.env, "hdo"), &key),
              napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);

  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    // The function's context keeps its global, the sandbox, alive.
    napi_value sandbox = nullptr;
    ASSERT_EQ(napi_get_reference_value(s.env, weak_sandbox, &sandbox), napi_ok);
    ASSERT_NE(sandbox, nullptr);

    napi_value array = nullptr, function = nullptr, undefined = nullptr, result = nullptr;
    ASSERT_EQ(napi_get_reference_value(s.env, functions, &array), napi_ok);
    ASSERT_EQ(napi_get_element(s.env, array, 0, &function), napi_ok);
    ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
    // Queues a microtask (on V8, on the context's own queue).
    ASSERT_EQ(napi_call_function(s.env, undefined, function, 0, nullptr, &result), napi_ok);
    int32_t value = 0;
    ASSERT_EQ(napi_get_value_int32(s.env, result, &value), napi_ok);
    EXPECT_EQ(value, 7);

#if defined(NAPI_TEST_ENGINE_V8)
    // Running a script in the context drains its own queue afterwards.
    ASSERT_NE(RunIn(s.env, sandbox, "0"), nullptr);
#else
    ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                  s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
              napi_ok);
#endif
    napi_value count = RunIn(s.env, sandbox, "box.n");
    ASSERT_NE(count, nullptr);
    ASSERT_EQ(napi_get_value_int32(s.env, count, &value), napi_ok);
    EXPECT_EQ(value, 1);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  // Once nothing made in the context is reachable, it is collected.
  ASSERT_EQ(napi_delete_reference(s.env, functions), napi_ok);
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_sandbox));
  ASSERT_EQ(napi_delete_reference(s.env, weak_sandbox), napi_ok);
}

// A value made in a context stays usable while it is in a handle scope, after
// its sandbox handle is gone; once released, the context is collected. A
// string, unlike an object, does not keep its realm alive by itself.
TEST_F(Test65UnofficialContextify, ContextValueOutlivesSandboxHandle) {
  EnvScope s(runtime_.get());

  napi_ref weak_sandbox = nullptr;
  napi_handle_scope outer = nullptr;
  ASSERT_EQ(napi_open_handle_scope(s.env, &outer), napi_ok);
  napi_value made = nullptr;
  {
    napi_escapable_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_escapable_handle_scope(s.env, &scope), napi_ok);
    napi_value sandbox = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    napi_value key = nullptr;
    ASSERT_EQ(unofficial_napi_contextify_make_context(s.env, sandbox, Str(s.env, "ctx"),
                                                      Str(s.env, "test://origin"), true, true,
                                                      false, Sym(s.env, "hdo"), &key),
              napi_ok);
    napi_value value = RunIn(s.env, sandbox, "'x'.repeat(40) + 'y'");
    ASSERT_NE(value, nullptr);
    ASSERT_EQ(napi_escape_handle(s.env, scope, value, &made), napi_ok);
    ASSERT_EQ(napi_create_reference(s.env, sandbox, 0, &weak_sandbox), napi_ok);
    ASSERT_EQ(napi_close_escapable_handle_scope(s.env, scope), napi_ok);
  }

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  size_t length = 0;
  ASSERT_EQ(napi_get_value_string_utf8(s.env, made, nullptr, 0, &length), napi_ok);
  EXPECT_EQ(length, 41u);
  ASSERT_EQ(napi_close_handle_scope(s.env, outer), napi_ok);

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_sandbox));
  ASSERT_EQ(napi_delete_reference(s.env, weak_sandbox), napi_ok);
}

// A weak reference to a context's object does not keep the context alive and
// reads back as empty once it is collected.
TEST_F(Test65UnofficialContextify, WeakReferenceToContextValue) {
  EnvScope s(runtime_.get());

  napi_ref weak_sandbox = nullptr;
  napi_ref weak_value = nullptr;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value sandbox = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    napi_value key = nullptr;
    ASSERT_EQ(unofficial_napi_contextify_make_context(s.env, sandbox, Str(s.env, "ctx"),
                                                      Str(s.env, "test://origin"), true, true,
                                                      false, Sym(s.env, "hdo"), &key),
              napi_ok);
    napi_value value = RunIn(s.env, sandbox, "globalThis.kept = {}; kept");
    ASSERT_NE(value, nullptr);
    ASSERT_EQ(napi_create_reference(s.env, value, 0, &weak_value), napi_ok);
    ASSERT_EQ(napi_create_reference(s.env, sandbox, 0, &weak_sandbox), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_sandbox));
  EXPECT_TRUE(IsCollected(s.env, weak_value));
  // A collected reference can still be made strong and deleted.
  uint32_t count = 0;
  ASSERT_EQ(napi_reference_ref(s.env, weak_value, &count), napi_ok);
  ASSERT_EQ(napi_delete_reference(s.env, weak_value), napi_ok);
  ASSERT_EQ(napi_delete_reference(s.env, weak_sandbox), napi_ok);
}

namespace {

napi_value MakeContext(napi_env env, napi_value sandbox) {
  napi_value key = nullptr;
  if (unofficial_napi_contextify_make_context(env, sandbox, Str(env, "ctx"),
                                              Str(env, "test://origin"), true, true, false,
                                              Sym(env, "hdo"), &key) != napi_ok) {
    return nullptr;
  }
  return key;
}

napi_value EnqueueArgument(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value callback = nullptr;
  if (napi_get_cb_info(env, info, &argc, &callback, nullptr, nullptr) != napi_ok ||
      unofficial_napi_enqueue_microtask(env, callback) != napi_ok) {
    return nullptr;
  }
  return nullptr;
}

int32_t MainInt(napi_env env, const char* code) {
  int32_t value = -1;
  napi_value result = RunIn(env, nullptr, code);
  if (result == nullptr || napi_get_value_int32(env, result, &value) != napi_ok) return -1;
  return value;
}

}  // namespace

// Code in a context resolves a promise of the main realm and returns. The
// reaction job runs after the context is otherwise unreachable and collected,
// and the context is collected once the job has run.
TEST_F(Test65UnofficialContextify, JobQueuedFromContextRunsAfterCollection) {
  EnvScope s(runtime_.get());

  napi_ref weak_sandbox = nullptr;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    ASSERT_NE(RunIn(s.env, nullptr,
                    "globalThis.hits = 0;"
                    "globalThis.pending = new Promise(r => { globalThis.resolvePending = r; });"
                    "pending.then(() => { hits++; }); 0"),
              nullptr);
    napi_value sandbox = nullptr, global = nullptr, resolve = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
    ASSERT_EQ(napi_get_named_property(s.env, global, "resolvePending", &resolve), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, sandbox, "resolvePending", resolve), napi_ok);
    ASSERT_NE(MakeContext(s.env, sandbox), nullptr);
    ASSERT_NE(RunIn(s.env, sandbox, "resolvePending(1); 0"), nullptr);
    ASSERT_NE(RunIn(s.env, nullptr, "delete globalThis.resolvePending; 0"), nullptr);
    ASSERT_EQ(napi_create_reference(s.env, sandbox, 0, &weak_sandbox), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
            napi_ok);
  EXPECT_EQ(MainInt(s.env, "hits"), 1);
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_sandbox));
  ASSERT_EQ(napi_delete_reference(s.env, weak_sandbox), napi_ok);
}

// A native function called from a context queues a main-realm callback; the
// callback runs after the context is collected.
TEST_F(Test65UnofficialContextify, MicrotaskQueuedFromContextRunsAfterCollection) {
  EnvScope s(runtime_.get());

  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    ASSERT_NE(RunIn(s.env, nullptr, "globalThis.calls = 0; globalThis.count = () => { calls++; }; 0"),
              nullptr);
    napi_value sandbox = nullptr, enqueue = nullptr, global = nullptr, count = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    ASSERT_EQ(napi_create_function(s.env, "enqueue", NAPI_AUTO_LENGTH, EnqueueArgument, nullptr,
                                   &enqueue),
              napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, sandbox, "enqueue", enqueue), napi_ok);
    ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
    ASSERT_EQ(napi_get_named_property(s.env, global, "count", &count), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, sandbox, "count", count), napi_ok);
    ASSERT_NE(MakeContext(s.env, sandbox), nullptr);
    ASSERT_NE(RunIn(s.env, sandbox, "enqueue(count); 0"), nullptr);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
            napi_ok);
  EXPECT_EQ(MainInt(s.env, "calls"), 1);
}

namespace {

// A queued job: has the context queue a throwing callback, then drains the
// queue itself (a nested checkpoint).
napi_value DrainFromJob(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  size_t argc = 0;
  napi_value sandbox = nullptr;
  if (napi_get_cb_info(env, info, &argc, nullptr, nullptr, &data) != napi_ok ||
      napi_get_reference_value(env, *static_cast<napi_ref*>(data), &sandbox) != napi_ok ||
      sandbox == nullptr) {
    return nullptr;
  }
  RunIn(env, sandbox, "enqueue(thrower); 0");
  unofficial_napi_event_loop_checkpoint(env, unofficial_napi_event_loop_checkpoint_microtasks, true,
                                        nullptr);
  return nullptr;
}

void ClearPendingException(napi_env env) {
  bool pending = false;
  napi_value exception = nullptr;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    napi_get_and_clear_last_exception(env, &exception);
  }
}

}  // namespace

// A job queued from a context throws inside a nested checkpoint; the context
// is still released once the queue is drained.
TEST_F(Test65UnofficialContextify, ContextOfJobThrowingInNestedCheckpointIsReleased) {
  EnvScope s(runtime_.get());

  napi_ref weak_sandbox = nullptr;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value sandbox = nullptr, enqueue = nullptr, drain = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    ASSERT_EQ(napi_create_function(s.env, "enqueue", NAPI_AUTO_LENGTH, EnqueueArgument, nullptr,
                                   &enqueue),
              napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, sandbox, "enqueue", enqueue), napi_ok);
    napi_value thrower = RunIn(s.env, nullptr, "() => { throw new Error('inner'); }");
    ASSERT_NE(thrower, nullptr);
    ASSERT_EQ(napi_set_named_property(s.env, sandbox, "thrower", thrower), napi_ok);
    ASSERT_NE(MakeContext(s.env, sandbox), nullptr);
    ASSERT_EQ(napi_create_reference(s.env, sandbox, 0, &weak_sandbox), napi_ok);
    ASSERT_EQ(napi_create_function(s.env, "drain", NAPI_AUTO_LENGTH, DrainFromJob, &weak_sandbox,
                                   &drain),
              napi_ok);
    ASSERT_EQ(unofficial_napi_enqueue_microtask(s.env, drain), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  for (int i = 0; i < 2; ++i) {
    unofficial_napi_event_loop_checkpoint(s.env, unofficial_napi_event_loop_checkpoint_microtasks,
                                          true, nullptr);
    ClearPendingException(s.env);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_sandbox));
  ASSERT_EQ(napi_delete_reference(s.env, weak_sandbox), napi_ok);
}

// A frozen sandbox cannot hold anything new; its context stays usable while
// the sandbox lives and is collected with it.
TEST_F(Test65UnofficialContextify, FrozenSandboxContext) {
  EnvScope s(runtime_.get());

  napi_ref weak_sandbox = nullptr;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value sandbox = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    ASSERT_EQ(napi_object_freeze(s.env, sandbox), napi_ok);
    ASSERT_NE(MakeContext(s.env, sandbox), nullptr);
    ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);

    const unofficial_napi_js_source source =
        unofficial_napi_js_source_from_text(Str(s.env, "return 40 + 2"));
    napi_value compiled = nullptr, function = nullptr, undefined = nullptr, result = nullptr;
    ASSERT_EQ(unofficial_napi_contextify_compile_function(s.env, &source, Str(s.env, "f.js"), 0, 0,
                                                          sandbox, nullptr, nullptr,
                                                          Sym(s.env, "hdo"), &compiled),
              napi_ok);
    ASSERT_EQ(napi_get_named_property(s.env, compiled, "function", &function), napi_ok);
    ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
    ASSERT_EQ(napi_call_function(s.env, undefined, function, 0, nullptr, &result), napi_ok);
    int32_t value = 0;
    ASSERT_EQ(napi_get_value_int32(s.env, result, &value), napi_ok);
    EXPECT_EQ(value, 42);
    ASSERT_EQ(napi_create_reference(s.env, sandbox, 0, &weak_sandbox), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_sandbox));
  ASSERT_EQ(napi_delete_reference(s.env, weak_sandbox), napi_ok);
}

// A Proxy sandbox's traps never see the provider's own bookkeeping, and its
// context stays usable.
TEST_F(Test65UnofficialContextify, ProxySandboxTrapsSeeNoInternals) {
  EnvScope s(runtime_.get());

  napi_handle_scope scope = nullptr;
  ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
  napi_value sandbox = RunIn(s.env, nullptr,
                             "globalThis.seen = [];"
                             "new Proxy({}, {"
                             "  defineProperty(target, key, descriptor) {"
                             "    if (typeof key === 'symbol') { seen.push(String(key)); return true; }"
                             "    return Reflect.defineProperty(target, key, descriptor);"
                             "  },"
                             "  deleteProperty(target, key) {"
                             "    if (typeof key === 'symbol') { seen.push(String(key)); return true; }"
                             "    return Reflect.deleteProperty(target, key);"
                             "  },"
                             "})");
  ASSERT_NE(sandbox, nullptr);
  ASSERT_NE(MakeContext(s.env, sandbox), nullptr);
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);

  napi_value sum = RunIn(s.env, sandbox, "1 + 1");
  ASSERT_NE(sum, nullptr);
  int32_t value = 0;
  ASSERT_EQ(napi_get_value_int32(s.env, sum, &value), napi_ok);
  EXPECT_EQ(value, 2);
  EXPECT_EQ(MainInt(s.env, "seen.length"), 0);
  ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
}

#if defined(NAPI_TEST_ENGINE_V8)
// The provider releases the records of collected contexts (their references
// and microtask queues) when it makes the next context.
TEST_F(Test65UnofficialContextify, CollectedContextRecordsAreReleased) {
  struct TokenCounts {
    int assigned = 0;
    int unassigned = 0;
  } counts;
  constexpr int kContexts = 6;
  {
    EnvScope s(runtime_.get());
    unofficial_napi_env_hooks hooks{};
    hooks.size = sizeof(hooks);
    hooks.version = UNOFFICIAL_NAPI_ENV_HOOKS_VERSION;
    hooks.data = &counts;
    hooks.context_token_assign_callback = [](napi_env, void*, void* data) {
      ++static_cast<TokenCounts*>(data)->assigned;
    };
    hooks.context_token_unassign_callback = [](napi_env, void*, void* data) {
      ++static_cast<TokenCounts*>(data)->unassigned;
    };
    uint64_t accepted = 0;
    ASSERT_EQ(unofficial_napi_attach_env(s.env, &hooks, &accepted), napi_ok);

    auto make = [&](const char* name) {
      napi_handle_scope scope = nullptr;
      ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
      napi_value sandbox = nullptr;
      napi_value key = nullptr;
      ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
      ASSERT_EQ(unofficial_napi_contextify_make_context(s.env, sandbox, Str(s.env, name),
                                                        Str(s.env, "test://origin"), true, true,
                                                        true, Sym(s.env, "hdo"), &key),
                napi_ok);
      ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
    };
    for (int i = 0; i < kContexts; ++i) make("ctx");
    ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
    EXPECT_EQ(counts.assigned, kContexts);
    EXPECT_EQ(counts.unassigned, 0);

    make("next");
    EXPECT_EQ(counts.assigned, kContexts + 1);
    EXPECT_EQ(counts.unassigned, kContexts);
  }
  // Releasing the environment releases the rest.
  EXPECT_EQ(counts.unassigned, kContexts + 1);
}
#endif
