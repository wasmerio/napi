#include "test_env.h"

#ifdef NAPI_MODULE
#undef NAPI_MODULE
#endif
#include "node_api.h"
#include "unofficial_napi.h"

#include <string>
#include <vector>

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

// Creates a realm over `sandbox` with the options vm.createContext passes.
napi_status CreateSandboxRealm(napi_env env,
                               napi_value sandbox,
                               bool own_microtask_queue,
                               unofficial_napi_realm* realm_out) {
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = sandbox;
  options.name = Str(env, "ctx");
  options.origin = Str(env, "test://origin");
  options.host_defined_option_id = Sym(env, "hdo");
  options.own_microtask_queue = own_microtask_queue;
  return unofficial_napi_create_realm(env, &options, UNOFFICIAL_NAPI_REALM_MODE_FRESH, 0, nullptr, realm_out, nullptr, nullptr);
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

struct RealmProbe {
  napi_ref main_global = nullptr;
  napi_ref main_object_prototype = nullptr;
  bool called = false;
  bool sees_main_global = false;
  bool creates_in_main_realm = false;
};

// Records which realm a native callback runs in.
napi_value ProbeRealm(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data) != napi_ok ||
      data == nullptr) {
    return nullptr;
  }
  auto* probe = static_cast<RealmProbe*>(data);
  probe->called = true;

  napi_value global = nullptr;
  napi_value main_global = nullptr;
  if (napi_get_global(env, &global) != napi_ok ||
      napi_get_reference_value(env, probe->main_global, &main_global) != napi_ok ||
      napi_strict_equals(env, global, main_global, &probe->sees_main_global) != napi_ok) {
    return nullptr;
  }

  napi_value object = nullptr;
  napi_value prototype = nullptr;
  napi_value main_prototype = nullptr;
  if (napi_create_object(env, &object) != napi_ok ||
      napi_get_prototype(env, object, &prototype) != napi_ok ||
      napi_get_reference_value(env, probe->main_object_prototype, &main_prototype) != napi_ok ||
      napi_strict_equals(env, prototype, main_prototype, &probe->creates_in_main_realm) != napi_ok) {
    return nullptr;
  }
  return object;
}

}  // namespace

// A native function keeps the realm it was created in, also when a script in
// another context calls it (as in Node and V8).
TEST_F(Test65UnofficialContextify, NativeFunctionCalledFromContextKeepsItsRealm) {
  EnvScope s(runtime_.get());

  RealmProbe probe;
  napi_value global = nullptr;
  napi_value object = nullptr;
  napi_value object_prototype = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);
  ASSERT_EQ(napi_get_prototype(s.env, object, &object_prototype), napi_ok);
  ASSERT_EQ(napi_create_reference(s.env, global, 1, &probe.main_global), napi_ok);
  ASSERT_EQ(napi_create_reference(s.env, object_prototype, 1, &probe.main_object_prototype),
            napi_ok);

  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_function(s.env, "probe", NAPI_AUTO_LENGTH, ProbeRealm, &probe, &fn),
            napi_ok);
  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, sandbox, "probe", fn), napi_ok);

  unofficial_napi_realm realm = nullptr;
  ASSERT_EQ(CreateSandboxRealm(s.env, sandbox, false, &realm), napi_ok);

  napi_value eval_result = nullptr;
  const unofficial_napi_js_source source = unofficial_napi_js_source_from_text(
      Str(s.env, "Object.getPrototypeOf(probe()) === Object.prototype"));
  ASSERT_EQ(unofficial_napi_run_script(s.env,
                                       realm,
                                       &source,
                                       Str(s.env, "probe.js"),
                                       0,
                                       0,
                                       -1,
                                       true,
                                       false,
                                       false,
                                       Sym(s.env, "hdo"),
                                       &eval_result),
            napi_ok);

  EXPECT_TRUE(probe.called);
  EXPECT_TRUE(probe.sees_main_global);
  EXPECT_TRUE(probe.creates_in_main_realm);
  // The object comes from the main realm, not the context's.
  bool from_context_realm = true;
  ASSERT_EQ(napi_get_value_bool(s.env, eval_result, &from_context_realm), napi_ok);
  EXPECT_FALSE(from_context_realm);

  ASSERT_EQ(napi_delete_reference(s.env, probe.main_global), napi_ok);
  ASSERT_EQ(napi_delete_reference(s.env, probe.main_object_prototype), napi_ok);
}

TEST_F(Test65UnofficialContextify, CreateRealmRunRoundTrip) {
  EnvScope s(runtime_.get());

  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);

  unofficial_napi_realm realm = nullptr;
  ASSERT_EQ(CreateSandboxRealm(s.env, sandbox, true, &realm), napi_ok);
  ASSERT_NE(realm, nullptr);

  napi_value eval_result = nullptr;
  const unofficial_napi_js_source run_source =
      unofficial_napi_js_source_from_text(Str(s.env, "globalThis.answer = 42; answer"));
  ASSERT_EQ(unofficial_napi_run_script(s.env,
                                       realm,
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
TEST_F(Test65UnofficialContextify, CreateRealmPreservesThrownProxyException) {
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

  unofficial_napi_realm realm = nullptr;
  EXPECT_EQ(CreateSandboxRealm(s.env, sandbox, false, &realm), napi_pending_exception);
  EXPECT_EQ(realm, nullptr);

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

  unofficial_napi_realm realm = nullptr;
  ASSERT_EQ(CreateSandboxRealm(s.env, sandbox, true, &realm), napi_ok);
  ASSERT_NE(realm, nullptr);

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
  ASSERT_EQ(unofficial_napi_run_script(s.env,
                                       realm,
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
  ASSERT_EQ(unofficial_napi_compile_function(s.env,
                                             nullptr,
                                             &fn_source,
                                             Str(s.env, "fn.js"),
                                             0,
                                             0,
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
  create_options.payload.source_text.source = &source;
  create_options.payload.source_text.host_defined_option_id = undefined;
  unofficial_napi_module_create_result create_result{};

  auto invalid_options = create_options;
  invalid_options.size = sizeof(invalid_options) - 1;
  EXPECT_EQ(unofficial_napi_module_wrap_create(s.env, &invalid_options, &create_result),
            napi_invalid_arg);
  invalid_options = create_options;
  invalid_options.size = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_V1_SIZE;
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
TEST_F(Test65UnofficialContextify, ModuleRequestsPreserveImportAttributes) {
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
  options.realm_or_null = nullptr;
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
    EXPECT_EQ(unofficial_napi_compile_function(s.env,
                                               nullptr,
                                               &source,
                                               Str(s.env, "host-id.js"),
                                               0,
                                               0,
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
  ASSERT_EQ(unofficial_napi_compile_function(s.env,
                                             nullptr,
                                             &fn_source,
                                             Str(s.env, "no-global-function.js"),
                                             0,
                                             0,
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
  ASSERT_EQ(unofficial_napi_compile_function(s.env,
                                             nullptr,
                                             &hashbang_source,
                                             Str(s.env, ""),
                                             0,
                                             0,
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
  EXPECT_EQ(unofficial_napi_compile_function(s.env,
                                             nullptr,
                                             &bom_source,
                                             Str(s.env, "bom_hashbang.js"),
                                             0,
                                             0,
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
  ASSERT_EQ(unofficial_napi_compile_function(s.env,
                                             nullptr,
                                             &cjs_source,
                                             Str(s.env, "cjs.js"),
                                             0,
                                             0,
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

struct LoaderAtRelease {
  napi_env env = nullptr;
  unofficial_napi_module module = nullptr;
};

// A cleanup hook that runs after the module loader has let go of its modules:
// it asks about the entry module's top-level await, as Node does at exit, by
// a handle the loader no longer knows, then imports dynamically.
void UseTheLoaderAtRelease(void* arg) {
  auto* loader = static_cast<LoaderAtRelease*>(arg);
  bool settled = false;
  (void)unofficial_napi_module_wrap_check_unsettled_top_level_await(
      loader->env, loader->module, false, &settled);
  napi_value ignored = nullptr;
  (void)napi_run_script(loader->env,
                        Str(loader->env, "globalThis.process = {};\n"
                                         "import('gone').catch(() => {});"),
                        &ignored);
}

}  // namespace

// What the module loader keeps for an env, made while its release runs (here
// after the loader's own cleanup), goes with the env: envs made later, which
// may reuse its address, start without it.
TEST_F(Test65UnofficialContextify, ModuleStateMadeAtReleaseGoesWithTheEnv) {
  for (int i = 0; i < 32; ++i) {
    EnvScope s(runtime_.get());
    LoaderAtRelease loader;
    loader.env = s.env;
    // Added before the module below, so it runs after the loader's cleanup.
    ASSERT_EQ(napi_add_env_cleanup_hook(s.env, UseTheLoaderAtRelease, &loader), napi_ok);

    napi_value wrapper = nullptr;
    napi_value undefined = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &wrapper), napi_ok);
    ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
    const unofficial_napi_js_source source =
        unofficial_napi_js_source_from_text(Str(s.env, "export const value = 1;"));
    unofficial_napi_module_create_options options{};
    options.size = sizeof(options);
    options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
    options.kind = unofficial_napi_module_source_text;
    options.wrapper = wrapper;
    options.url = Str(s.env, "entry.mjs");
    options.payload.source_text.source = &source;
    options.payload.source_text.host_defined_option_id = undefined;
    unofficial_napi_module_create_result created{};
    ASSERT_EQ(unofficial_napi_module_wrap_create(s.env, &options, &created), napi_ok);
    loader.module = created.module;

    ASSERT_EQ(unofficial_napi_release_env(s.scope, nullptr), napi_ok);
    s.scope = nullptr;
    s.env = nullptr;
  }
}

// A script's import() in an env with no module loader and no `process`
// object, so nothing to load the module with, rejects.
TEST_F(Test65UnofficialContextify, DynamicImportWithNoLoaderRejects) {
  EnvScope s(runtime_.get());
  napi_value result = nullptr;
  ASSERT_EQ(napi_run_script(s.env,
                            Str(s.env,
                                "globalThis.outcome = 'pending';"
                                "import('missing').then(() => { outcome = 'resolved'; },"
                                "                       () => { outcome = 'rejected'; });"
                                "typeof process"),
                            &result),
            napi_ok);
  char text[32] = {};
  size_t length = 0;
  ASSERT_EQ(napi_get_value_string_utf8(s.env, result, text, sizeof(text), &length), napi_ok);
  EXPECT_STREQ(text, "undefined");
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
            napi_ok);
  ASSERT_EQ(napi_run_script(s.env, Str(s.env, "outcome"), &result), napi_ok);
  ASSERT_EQ(napi_get_value_string_utf8(s.env, result, text, sizeof(text), &length), napi_ok);
  EXPECT_STREQ(text, "rejected");
}

namespace {

// Calls import('missing') and returns a box that records what became of it:
// `sync` is "returned" once import() returned a promise, and `outcome` says
// how that promise settled.
constexpr char kImportBox[] =
    "(() => {"
    "  const box = { sync: 'none', outcome: 'pending' };"
    "  try {"
    "    const promise = import('missing');"
    "    box.sync = Object.prototype.toString.call(promise) === '[object Promise]'"
    "               ? 'returned' : 'returned no promise';"
    "    promise.then((ns) => { box.outcome = 'resolved:' + (ns && ns.from); },"
    "                 () => { box.outcome = 'rejected'; });"
    "  } catch (e) { box.sync = 'threw: ' + e; }"
    "  return box;"
    "})()";

std::string BoxText(napi_env env, napi_value box, const char* name) {
  napi_value value = nullptr;
  napi_value string = nullptr;
  char text[128] = {};
  size_t length = 0;
  if (box == nullptr || napi_get_named_property(env, box, name, &value) != napi_ok ||
      napi_coerce_to_string(env, value, &string) != napi_ok ||
      napi_get_value_string_utf8(env, string, text, sizeof(text), &length) != napi_ok) {
    return "<unreadable>";
  }
  return std::string(text, length);
}

// "returned, rejected" for an import() that returned a promise that rejected.
std::string ImportOutcome(napi_env env, napi_value box) {
  return BoxText(env, box, "sync") + ", " + BoxText(env, box, "outcome");
}

napi_status RunInRealm(napi_env env,
                       unofficial_napi_realm realm,
                       const char* code,
                       napi_value* result_out) {
  const unofficial_napi_js_source source = unofficial_napi_js_source_from_text(Str(env, code));
  return unofficial_napi_run_script(env, realm, &source, Str(env, "realm.js"), 0, 0, -1, true,
                                    false, false, Sym(env, "hdo"), result_out);
}

void RunMicrotasks(napi_env env) {
  EXPECT_EQ(unofficial_napi_event_loop_checkpoint(
                env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
            napi_ok);
}

}  // namespace

// Module hooks with no dynamic import callback: none at all, only the
// import.meta one, or one that is not a function. An import() then has
// nothing to load it with, in the env's realm or in another, and rejects. On
// V8 the import callback the hooks install returned no promise and threw
// nothing there, and V8 aborts the process on that (CHECK has_exception()).
TEST_F(Test65UnofficialContextify, DynamicImportWithModuleHooksButNoImportCallbackRejects) {
  enum Hooks { kNoCallbacks, kImportMetaOnly, kImportNotAFunction };
  for (Hooks which : {kNoCallbacks, kImportMetaOnly, kImportNotAFunction}) {
    SCOPED_TRACE(which == kNoCallbacks     ? "no callbacks"
                 : which == kImportMetaOnly ? "import.meta callback only"
                                            : "import callback undefined");
    EnvScope s(runtime_.get());
    napi_value undefined = nullptr;
    ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
    napi_value meta = nullptr;
    ASSERT_EQ(napi_run_script(s.env, Str(s.env, "(() => {})"), &meta), napi_ok);
    unofficial_napi_module_hooks hooks{};
    hooks.size = sizeof(hooks);
    hooks.version = UNOFFICIAL_NAPI_MODULE_HOOKS_VERSION;
    if (which == kImportMetaOnly) hooks.initialize_import_meta_object = meta;
    if (which == kImportNotAFunction) hooks.import_module_dynamically = undefined;
    ASSERT_EQ(unofficial_napi_module_wrap_set_hooks(s.env, &hooks), napi_ok);

    napi_value sandbox = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    unofficial_napi_realm sandbox_realm = nullptr;
    ASSERT_EQ(CreateSandboxRealm(s.env, sandbox, false, &sandbox_realm), napi_ok);
    unofficial_napi_realm fresh_realm = nullptr;
    napi_value fresh_global = nullptr;
    ASSERT_EQ(unofficial_napi_create_realm(s.env, nullptr, UNOFFICIAL_NAPI_REALM_MODE_FRESH, 0, nullptr, &fresh_realm, &fresh_global, nullptr), napi_ok);

    napi_value env_box = nullptr;
    ASSERT_EQ(napi_run_script(s.env, Str(s.env, kImportBox), &env_box), napi_ok);
    napi_value sandbox_box = nullptr;
    ASSERT_EQ(RunInRealm(s.env, sandbox_realm, kImportBox, &sandbox_box), napi_ok);
    napi_value fresh_box = nullptr;
    ASSERT_EQ(RunInRealm(s.env, fresh_realm, kImportBox, &fresh_box), napi_ok);
    RunMicrotasks(s.env);
    EXPECT_EQ(ImportOutcome(s.env, env_box), "returned, rejected") << "the env's realm";
    EXPECT_EQ(ImportOutcome(s.env, sandbox_box), "returned, rejected") << "a sandbox realm";
    EXPECT_EQ(ImportOutcome(s.env, fresh_box), "returned, rejected") << "a fresh realm";
  }
}

namespace {

struct ImportsAtRelease {
  napi_env env = nullptr;
  bool ran = false;
  std::vector<std::string> outcomes;
};

// A module whose evaluation imports; returns its box.
napi_value ImportFromAModule(napi_env env, std::string* failed) {
  napi_value wrapper = nullptr;
  napi_value undefined = nullptr;
  napi_create_object(env, &wrapper);
  napi_get_undefined(env, &undefined);
  const std::string code = std::string("export const box = ") + kImportBox + ";";
  const unofficial_napi_js_source source =
      unofficial_napi_js_source_from_text(Str(env, code.c_str()));
  unofficial_napi_module_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  options.kind = unofficial_napi_module_source_text;
  options.wrapper = wrapper;
  options.url = Str(env, "imports.mjs");
  options.payload.source_text.source = &source;
  options.payload.source_text.host_defined_option_id = undefined;
  unofficial_napi_module_create_result created{};
  if (unofficial_napi_module_wrap_create(env, &options, &created) != napi_ok) {
    *failed = "create";
    return nullptr;
  }
  napi_value evaluated = nullptr;
  napi_value ns = nullptr;
  napi_value box = nullptr;
  if (unofficial_napi_module_wrap_link(env, created.module, 0, nullptr) != napi_ok) {
    *failed = "link";
  } else if (unofficial_napi_module_wrap_instantiate(env, created.module) != napi_ok) {
    *failed = "instantiate";
  } else if (unofficial_napi_module_wrap_evaluate(env, created.module, -1, false, &evaluated) !=
             napi_ok) {
    *failed = "evaluate";
  } else if (unofficial_napi_module_wrap_get_namespace(env, created.module, &ns) != napi_ok ||
             napi_get_named_property(env, ns, "box", &box) != napi_ok) {
    *failed = "namespace";
  }
  unofficial_napi_module_wrap_destroy(env, created.module);
  return box;
}

// Added before the module hooks, so the release runs it after the module
// loader's own cleanup hook (most recent first), which on V8 lets go of the
// hooks and of the loader's state. It imports from a script; from a module,
// which makes the loader's state again, with no hooks; and from a script
// again.
void ImportAfterTheLoadersCleanupHook(void* arg) {
  auto* imports = static_cast<ImportsAtRelease*>(arg);
  napi_env env = imports->env;
  imports->ran = true;
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) return;
  napi_value boxes[3] = {};
  std::string failed;
  if (napi_run_script(env, Str(env, kImportBox), &boxes[0]) != napi_ok) failed = "script";
  if (failed.empty()) boxes[1] = ImportFromAModule(env, &failed);
  if (failed.empty() && napi_run_script(env, Str(env, kImportBox), &boxes[2]) != napi_ok) {
    failed = "second script";
  }
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    napi_value ignored = nullptr;
    napi_get_and_clear_last_exception(env, &ignored);
    failed += " (exception pending)";
  }
  unofficial_napi_event_loop_checkpoint(env, unofficial_napi_event_loop_checkpoint_microtasks,
                                        true, nullptr);
  if (!failed.empty()) imports->outcomes.push_back("failed: " + failed);
  for (napi_value box : boxes) {
    if (box != nullptr) imports->outcomes.push_back(ImportOutcome(env, box));
  }
  napi_close_handle_scope(env, scope);
}

}  // namespace

// An import() a cleanup hook makes at release, after the module loader's own
// cleanup hook, returns a promise that settles. On V8 the import callback the
// hooks installed, still the isolate's, found no loader state there and
// returned no promise with nothing thrown, and V8 aborts the process on that.
// How it settles is the providers' own: V8's loader lets go of its hooks in
// its cleanup hook, so the import() rejects; QuickJS's loader keeps them
// until the release's pass is over, so they load it.
TEST_F(Test65UnofficialContextify, DynamicImportFromACleanupHookAfterTheLoadersCleanupHookSettles) {
  ImportsAtRelease imports;
  {
    EnvScope s(runtime_.get());
    imports.env = s.env;
    ASSERT_EQ(napi_add_env_cleanup_hook(s.env, ImportAfterTheLoadersCleanupHook, &imports),
              napi_ok);
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
    // Before the release the hooks load it.
    napi_value box = nullptr;
    ASSERT_EQ(napi_run_script(s.env, Str(s.env, kImportBox), &box), napi_ok);
    RunMicrotasks(s.env);
    EXPECT_EQ(ImportOutcome(s.env, box), "returned, resolved:hooks");
    ASSERT_EQ(unofficial_napi_release_env(s.scope, nullptr), napi_ok);
    s.scope = nullptr;
    s.env = nullptr;
  }
  EXPECT_TRUE(imports.ran);
#if defined(NAPI_TEST_ENGINE_V8)
  const std::string expected = "returned, rejected";
#else
  const std::string expected = "returned, resolved:hooks";
#endif
  EXPECT_EQ(imports.outcomes, std::vector<std::string>(3, expected));
}

namespace {

std::string ValueText(napi_env env, napi_value value) {
  napi_value string = nullptr;
  char text[256] = {};
  size_t length = 0;
  if (value == nullptr || napi_coerce_to_string(env, value, &string) != napi_ok ||
      napi_get_value_string_utf8(env, string, text, sizeof(text), &length) != napi_ok) {
    return "<unprintable>";
  }
  return std::string(text, length);
}

// Takes the pending exception: its message, or "" when none was pending.
std::string TakePendingMessage(napi_env env) {
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) != napi_ok || !pending) return "";
  napi_value error = nullptr;
  napi_value message = nullptr;
  if (napi_get_and_clear_last_exception(env, &error) != napi_ok) return "<not taken>";
  if (napi_get_named_property(env, error, "message", &message) != napi_ok) return "<no message>";
  return ValueText(env, message);
}

// How `promise` settles once the microtasks have run: "resolved", or
// "rejected: <message>".
std::string PromiseOutcome(napi_env env, napi_value promise) {
  napi_value global = nullptr;
  napi_value outcome = nullptr;
  if (promise == nullptr || napi_get_global(env, &global) != napi_ok ||
      napi_set_named_property(env, global, "settling", promise) != napi_ok ||
      napi_run_script(env,
                      Str(env,
                          "globalThis.outcome = 'pending';"
                          "Promise.resolve(settling).then(() => { outcome = 'resolved'; },"
                          "  (e) => { outcome = 'rejected: ' + (e && e.message); });"
                          "0"),
                      &outcome) != napi_ok) {
    return "<not observed>";
  }
  RunMicrotasks(env);
  if (napi_get_named_property(env, global, "outcome", &outcome) != napi_ok) return "<unread>";
  return ValueText(env, outcome);
}

struct Evaluated {
  napi_status evaluate = napi_generic_failure;
  std::string pending;
  std::string outcome;
  napi_value ns = nullptr;
};

// Creates the module, links it to nothing, instantiates and evaluates it.
Evaluated CreateAndEvaluate(napi_env env, const unofficial_napi_module_create_options& options) {
  Evaluated out;
  unofficial_napi_module_create_result created{};
  EXPECT_EQ(unofficial_napi_module_wrap_create(env, &options, &created), napi_ok);
  if (created.module == nullptr) return out;
  EXPECT_EQ(unofficial_napi_module_wrap_link(env, created.module, 0, nullptr), napi_ok);
  EXPECT_EQ(unofficial_napi_module_wrap_instantiate(env, created.module), napi_ok);
  napi_value result = nullptr;
  out.evaluate = unofficial_napi_module_wrap_evaluate(env, created.module, -1, false, &result);
  out.pending = TakePendingMessage(env);
  if (out.evaluate == napi_ok) out.outcome = PromiseOutcome(env, result);
  EXPECT_EQ(unofficial_napi_module_wrap_get_namespace(env, created.module, &out.ns), napi_ok);
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(env, created.module), napi_ok);
  return out;
}

}  // namespace

// A synthetic module whose evaluation steps are not a function has none to
// run: it evaluates, and its exports stay undefined. V8's evaluation steps
// callback returned no result with nothing thrown there, and V8 aborts the
// process on that (CHECK has_exception()); QuickJS skipped the steps.
TEST_F(Test65UnofficialContextify, SyntheticModuleWhoseStepsAreNotAFunctionEvaluates) {
  for (const char* steps_source : {"({})", "undefined"}) {
    SCOPED_TRACE(steps_source);
    EnvScope s(runtime_.get());
    napi_value wrapper = nullptr;
    napi_value names = nullptr;
    napi_value steps = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &wrapper), napi_ok);
    ASSERT_EQ(napi_run_script(s.env, Str(s.env, "['value']"), &names), napi_ok);
    ASSERT_EQ(napi_run_script(s.env, Str(s.env, steps_source), &steps), napi_ok);
    unofficial_napi_module_create_options options{};
    options.size = sizeof(options);
    options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
    options.kind = unofficial_napi_module_synthetic;
    options.wrapper = wrapper;
    options.url = Str(s.env, "synthetic");
    options.payload.synthetic.export_names = names;
    options.payload.synthetic.synthetic_evaluation_steps = steps;
    const Evaluated evaluated = CreateAndEvaluate(s.env, options);
    EXPECT_EQ(evaluated.evaluate, napi_ok);
    EXPECT_EQ(evaluated.pending, "");
    EXPECT_EQ(evaluated.outcome, "resolved");
    napi_value value = nullptr;
    napi_valuetype type = napi_null;
    ASSERT_NE(evaluated.ns, nullptr);
    ASSERT_EQ(napi_get_named_property(s.env, evaluated.ns, "value", &value), napi_ok);
    ASSERT_EQ(napi_typeof(s.env, value, &type), napi_ok);
    EXPECT_EQ(type, napi_undefined);
  }
}

// What the import.meta callback throws is thrown from the import.meta
// expression, as in Node (ModuleWrap's callback rethrows it): the module stops
// there, and its evaluation rejects with it. V8 left it in N-API's pending
// exception instead: the module ran on, and the error surfaced from the next
// N-API call that checked for one.
TEST_F(Test65UnofficialContextify, ImportMetaCallbackExceptionThrowsFromTheModule) {
  EnvScope s(runtime_.get());
  napi_value meta = nullptr;
  ASSERT_EQ(napi_run_script(s.env,
                            Str(s.env,
                                "globalThis.metaCalls = 0;"
                                "(id, meta, wrap) => { metaCalls++; throw new Error('meta'); }"),
                            &meta),
            napi_ok);
  unofficial_napi_module_hooks hooks{};
  hooks.size = sizeof(hooks);
  hooks.version = UNOFFICIAL_NAPI_MODULE_HOOKS_VERSION;
  hooks.initialize_import_meta_object = meta;
  ASSERT_EQ(unofficial_napi_module_wrap_set_hooks(s.env, &hooks), napi_ok);
  napi_value wrapper = nullptr;
  napi_value undefined = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &wrapper), napi_ok);
  ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
  const unofficial_napi_js_source source = unofficial_napi_js_source_from_text(
      Str(s.env, "globalThis.continued = 'no'; import.meta; globalThis.continued = 'yes';"));
  unofficial_napi_module_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  options.kind = unofficial_napi_module_source_text;
  options.wrapper = wrapper;
  options.url = Str(s.env, "meta.mjs");
  options.payload.source_text.source = &source;
  options.payload.source_text.host_defined_option_id = undefined;
  const Evaluated evaluated = CreateAndEvaluate(s.env, options);
  EXPECT_EQ(evaluated.evaluate, napi_ok);
  EXPECT_EQ(evaluated.pending, "") << "left pending by the evaluation";
  EXPECT_EQ(evaluated.outcome, "rejected: meta");
  napi_value continued = nullptr;
  napi_value calls = nullptr;
  ASSERT_EQ(napi_run_script(s.env, Str(s.env, "continued"), &continued), napi_ok);
  ASSERT_EQ(napi_run_script(s.env, Str(s.env, "metaCalls"), &calls), napi_ok);
  EXPECT_EQ(ValueText(s.env, continued), "no");
  EXPECT_EQ(ValueText(s.env, calls), "1");
  EXPECT_EQ(TakePendingMessage(s.env), "");
}
