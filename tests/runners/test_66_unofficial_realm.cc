#ifdef NAPI_MODULE
#undef NAPI_MODULE
#endif
#include "node_api.h"

#include "test_env.h"

#include "unofficial_napi.h"

#include <string>
#include <vector>

class Test66UnofficialRealm : public FixtureTestBase {};

namespace {

napi_value Str(napi_env env, const char* value) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out) != napi_ok) return nullptr;
  return out;
}

napi_value Eval(napi_env env, const char* source) {
  napi_value result = nullptr;
  if (napi_run_script(env, Str(env, source), &result) != napi_ok) return nullptr;
  return result;
}

// Runs `source` in `realm` (the env's own realm when null).
napi_value RunIn(napi_env env, unofficial_napi_realm realm, const char* source) {
  const unofficial_napi_js_source script = unofficial_napi_js_source_from_text(Str(env, source));
  napi_value result = nullptr;
  if (unofficial_napi_run_script(env, realm, &script, Str(env, "realm.js"), 0, 0, -1, true, false,
                                 false, nullptr, &result) != napi_ok) {
    return nullptr;
  }
  return result;
}

bool StrictEquals(napi_env env, napi_value a, napi_value b) {
  bool same = false;
  return napi_strict_equals(env, a, b, &same) == napi_ok && same;
}

napi_value Global(napi_env env) {
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok) return nullptr;
  return global;
}

napi_value Prototype(napi_env env, napi_value object) {
  napi_value prototype = nullptr;
  if (napi_get_prototype(env, object, &prototype) != napi_ok) return nullptr;
  return prototype;
}

int32_t Int32(napi_env env, napi_value value) {
  int32_t out = -1;
  if (value == nullptr || napi_get_value_int32(env, value, &out) != napi_ok) return -1;
  return out;
}

std::string Utf8(napi_env env, napi_value value) {
  char buffer[256] = {};
  size_t length = 0;
  if (value == nullptr ||
      napi_get_value_string_utf8(env, value, buffer, sizeof(buffer), &length) != napi_ok) {
    return std::string();
  }
  return std::string(buffer, length);
}

unofficial_napi_realm CreateRealm(napi_env env,
                                  napi_value* global_out = nullptr,
                                  napi_value sandbox = nullptr,
                                  bool own_microtask_queue = false) {
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = sandbox;
  options.own_microtask_queue = own_microtask_queue;
  unofficial_napi_realm realm = nullptr;
  if (unofficial_napi_create_realm(env, &options, &realm, global_out) != napi_ok) return nullptr;
  return realm;
}

// Two handles for one realm may differ, so realms compare by their globals.
bool SameRealm(napi_env env, unofficial_napi_realm a, unofficial_napi_realm b) {
  return a != nullptr && b != nullptr &&
         StrictEquals(env, RunIn(env, a, "globalThis"), RunIn(env, b, "globalThis"));
}

bool IsCollected(napi_env env, napi_ref ref) {
  napi_value value = nullptr;
  return napi_get_reference_value(env, ref, &value) == napi_ok && value == nullptr;
}

struct CallRecord {
  napi_env env = nullptr;
  bool saw_env_global = false;
  bool made_env_object = false;
  int calls = 0;
};

// Records the env a native callback gets, and which realm its global and the
// objects it makes belong to.
napi_value RecordCall(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data) != napi_ok) return nullptr;
  auto* record = static_cast<CallRecord*>(data);
  record->env = env;
  record->calls++;
  napi_value env_global = Eval(env, "globalThis");
  record->saw_env_global = StrictEquals(env, Global(env), env_global);
  napi_value object = nullptr;
  if (napi_create_object(env, &object) != napi_ok) return nullptr;
  record->made_env_object =
      StrictEquals(env, Prototype(env, object), Eval(env, "Object.prototype"));
  return object;
}

}  // namespace

TEST_F(Test66UnofficialRealm, CreateRealmValidatesItsArguments) {
  EnvScope s(runtime_.get());
  unofficial_napi_realm realm = nullptr;

  EXPECT_EQ(unofficial_napi_create_realm(nullptr, nullptr, &realm, nullptr), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_create_realm(s.env, nullptr, nullptr, nullptr), napi_invalid_arg);

  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  EXPECT_TRUE(options.allow_code_gen_strings);
  EXPECT_TRUE(options.allow_code_gen_wasm);
  EXPECT_FALSE(options.own_microtask_queue);

  options.size = sizeof(options) - 1;
  EXPECT_EQ(unofficial_napi_create_realm(s.env, &options, &realm, nullptr), napi_invalid_arg);
  unofficial_napi_realm_options_init(&options);
  options.version += 1;
  EXPECT_EQ(unofficial_napi_create_realm(s.env, &options, &realm, nullptr), napi_invalid_arg);
  EXPECT_EQ(realm, nullptr);

  napi_value number = nullptr;
  ASSERT_EQ(napi_create_int32(s.env, 1, &number), napi_ok);
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = number;
  EXPECT_NE(unofficial_napi_create_realm(s.env, &options, &realm, nullptr), napi_ok);

  // A sandbox belongs to one realm.
  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
  ASSERT_NE(CreateRealm(s.env, nullptr, sandbox), nullptr);
  options.sandbox_or_null = sandbox;
  realm = nullptr;
  EXPECT_EQ(unofficial_napi_create_realm(s.env, &options, &realm, nullptr), napi_invalid_arg);
  EXPECT_EQ(realm, nullptr);
}

// As with vm.createContext(globalThis) in Node.
TEST_F(Test66UnofficialRealm, TheEnvsGlobalCanBeASandbox) {
  EnvScope s(runtime_.get());
  unofficial_napi_realm realm = CreateRealm(s.env, nullptr, Global(s.env));
  ASSERT_NE(realm, nullptr);
  EXPECT_NE(RunIn(s.env, realm, "!function() { var x = Object; }()"), nullptr);
}

TEST_F(Test66UnofficialRealm, RealmHasItsOwnGlobal) {
  EnvScope s(runtime_.get());
  napi_value realm_global = nullptr;
  unofficial_napi_realm realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);
  ASSERT_NE(realm_global, nullptr);

  EXPECT_TRUE(StrictEquals(s.env, RunIn(s.env, realm, "globalThis"), realm_global));
  EXPECT_FALSE(StrictEquals(s.env, realm_global, Global(s.env)));
  EXPECT_TRUE(StrictEquals(s.env, RunIn(s.env, nullptr, "globalThis"), Global(s.env)));
  EXPECT_TRUE(StrictEquals(s.env, Eval(s.env, "globalThis"), Global(s.env)));

  // The realm has its own intrinsics.
  EXPECT_FALSE(
      StrictEquals(s.env, RunIn(s.env, realm, "Object.prototype"), Eval(s.env, "Object.prototype")));
}

TEST_F(Test66UnofficialRealm, ValuesCrossRealms) {
  EnvScope s(runtime_.get());
  napi_value realm_global = nullptr;
  unofficial_napi_realm realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);

  napi_value shared = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &shared), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, realm_global, "shared", shared), napi_ok);
  EXPECT_EQ(Int32(s.env, RunIn(s.env, realm, "shared.x = 7")), 7);
  napi_value x = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, shared, "x", &x), napi_ok);
  EXPECT_EQ(Int32(s.env, x), 7);
}

// A native function belongs to the env's realm, also when code in another
// realm calls it, as in Node.
TEST_F(Test66UnofficialRealm, NativeFunctionsStayInTheEnvsRealm) {
  EnvScope s(runtime_.get());
  napi_value realm_global = nullptr;
  unofficial_napi_realm realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);

  CallRecord record;
  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_function(s.env, "probe", NAPI_AUTO_LENGTH, RecordCall, &record, &fn),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, realm_global, "probe", fn), napi_ok);

  napi_value result = RunIn(s.env, realm, "probe()");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(record.calls, 1);
  EXPECT_EQ(record.env, s.env);
  EXPECT_TRUE(record.saw_env_global);
  EXPECT_TRUE(record.made_env_object);
  EXPECT_TRUE(StrictEquals(s.env, Prototype(s.env, result), Eval(s.env, "Object.prototype")));
}

TEST_F(Test66UnofficialRealm, GetRealmFindsRealmsByTheirGlobal) {
  EnvScope s(runtime_.get());
  napi_value realm_global = nullptr;
  unofficial_napi_realm realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);
  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
  unofficial_napi_realm sandbox_realm = CreateRealm(s.env, nullptr, sandbox);
  ASSERT_NE(sandbox_realm, nullptr);

  unofficial_napi_realm found = nullptr;
  ASSERT_EQ(unofficial_napi_get_realm(s.env, realm_global, &found), napi_ok);
  EXPECT_TRUE(SameRealm(s.env, found, realm));
  found = nullptr;
  ASSERT_EQ(unofficial_napi_get_realm(s.env, sandbox, &found), napi_ok);
  EXPECT_TRUE(SameRealm(s.env, found, sandbox_realm));
  EXPECT_FALSE(SameRealm(s.env, found, realm));

  // The env's own global and other objects, functions included, are not a
  // realm's global.
  found = realm;
  ASSERT_EQ(unofficial_napi_get_realm(s.env, Global(s.env), &found), napi_ok);
  EXPECT_EQ(found, nullptr);
  for (const char* source : {"(function f() {})", "({})", "[]"}) {
    napi_value object = RunIn(s.env, realm, source);
    ASSERT_NE(object, nullptr);
    found = realm;
    ASSERT_EQ(unofficial_napi_get_realm(s.env, object, &found), napi_ok) << source;
    EXPECT_EQ(found, nullptr) << source;
  }

  napi_value number = nullptr;
  ASSERT_EQ(napi_create_int32(s.env, 1, &number), napi_ok);
  EXPECT_EQ(unofficial_napi_get_realm(s.env, number, &found), napi_object_expected);
  EXPECT_EQ(unofficial_napi_get_realm(s.env, nullptr, &found), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_get_realm(s.env, realm_global, nullptr), napi_invalid_arg);
}

TEST_F(Test66UnofficialRealm, SandboxRealmReadsAndWritesTheSandbox) {
  EnvScope s(runtime_.get());
  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
  napi_value five = nullptr;
  ASSERT_EQ(napi_create_int32(s.env, 5, &five), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, sandbox, "input", five), napi_ok);

  napi_value global_out = nullptr;
  unofficial_napi_realm realm = CreateRealm(s.env, &global_out, sandbox);
  ASSERT_NE(realm, nullptr);
  EXPECT_TRUE(StrictEquals(s.env, global_out, sandbox));

  EXPECT_EQ(Int32(s.env, RunIn(s.env, realm, "output = input * 2; input")), 5);
  napi_value output = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, sandbox, "output", &output), napi_ok);
  EXPECT_EQ(Int32(s.env, output), 10);
  EXPECT_EQ(Utf8(s.env, Eval(s.env, "typeof output")), "undefined");
}

TEST_F(Test66UnofficialRealm, ScriptsAndFunctionsRunInTheirRealm) {
  EnvScope s(runtime_.get());
  napi_value realm_global = nullptr;
  unofficial_napi_realm realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);

  napi_value undefined = nullptr;
  ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
  const unofficial_napi_js_source body =
      unofficial_napi_js_source_from_text(Str(s.env, "return globalThis;"));
  for (unofficial_napi_realm target : {realm, static_cast<unofficial_napi_realm>(nullptr)}) {
    napi_value compiled = nullptr;
    ASSERT_EQ(unofficial_napi_compile_function(s.env, target, &body, Str(s.env, "f.js"), 0, 0,
                                               undefined, undefined, undefined, &compiled),
              napi_ok);
    napi_value fn = nullptr;
    ASSERT_EQ(napi_get_named_property(s.env, compiled, "function", &fn), napi_ok);
    napi_value returned = nullptr;
    ASSERT_EQ(napi_call_function(s.env, undefined, fn, 0, nullptr, &returned), napi_ok);
    EXPECT_TRUE(StrictEquals(s.env, returned, target != nullptr ? realm_global : Global(s.env)));
  }
}

TEST_F(Test66UnofficialRealm, ModulesEvaluateInTheirRealm) {
  EnvScope s(runtime_.get());
  napi_value realm_global = nullptr;
  unofficial_napi_realm realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);

  napi_value wrapper = nullptr;
  napi_value undefined = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &wrapper), napi_ok);
  ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
  const unofficial_napi_js_source source =
      unofficial_napi_js_source_from_text(Str(s.env, "export const g = globalThis;"));
  unofficial_napi_module_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  options.kind = unofficial_napi_module_source_text;
  options.wrapper = wrapper;
  options.url = Str(s.env, "realm.mjs");
  options.realm_or_null = realm;
  options.payload.source_text.source = &source;
  options.payload.source_text.host_defined_option_id = undefined;
  unofficial_napi_module_create_result created{};
  ASSERT_EQ(unofficial_napi_module_wrap_create(s.env, &options, &created), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, created.module, 0, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, created.module), napi_ok);
  napi_value evaluated = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, created.module, -1, false, &evaluated),
            napi_ok);
  napi_value ns = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_get_namespace(s.env, created.module, &ns), napi_ok);
  napi_value g = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, ns, "g", &g), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, g, realm_global));
  EXPECT_FALSE(StrictEquals(s.env, g, Global(s.env)));
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, created.module), napi_ok);
}

TEST_F(Test66UnofficialRealm, BytecodeIsOpenedForARealm) {
  EnvScope s(runtime_.get());
  napi_value realm_global = nullptr;
  unofficial_napi_realm realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);

  napi_value undefined = nullptr;
  ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
  unofficial_napi_bytecode_open_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION;
  options.source_text = Str(s.env, "globalThis");
  options.filename = Str(s.env, "bytecode.js");
  options.shape = unofficial_napi_bytecode_shape_script;
  options.params_or_undefined = undefined;
  options.host_defined_option_id = undefined;
  unofficial_napi_bytecode_open_result opened{};

  unofficial_napi_bytecode_open_options v1 = options;
  v1.version = 1;
  EXPECT_EQ(unofficial_napi_bytecode_open(s.env, &v1, &opened), napi_invalid_arg);

  for (unofficial_napi_realm target : {realm, static_cast<unofficial_napi_realm>(nullptr)}) {
    options.realm_or_null = target;
    opened = {};
    ASSERT_EQ(unofficial_napi_bytecode_open(s.env, &options, &opened), napi_ok);
    ASSERT_NE(opened.bytecode, nullptr);
    const unofficial_napi_js_source source =
        unofficial_napi_js_source_from_bytecode(opened.bytecode);
    napi_value result = nullptr;
    ASSERT_EQ(unofficial_napi_run_script(s.env, target, &source, Str(s.env, "bytecode.js"), 0, 0,
                                         -1, true, false, false, nullptr, &result),
              napi_ok);
    EXPECT_TRUE(StrictEquals(s.env, result, target != nullptr ? realm_global : Global(s.env)));
    EXPECT_EQ(unofficial_napi_bytecode_release(s.env, opened.bytecode), napi_ok);
  }
}

// An exception thrown in a realm is pending on the env afterwards.
TEST_F(Test66UnofficialRealm, ExceptionsInARealmArePendingOnTheEnv) {
  EnvScope s(runtime_.get());
  unofficial_napi_realm realm = CreateRealm(s.env);
  ASSERT_NE(realm, nullptr);

  const unofficial_napi_js_source script =
      unofficial_napi_js_source_from_text(Str(s.env, "throw new Error('inside realm')"));
  napi_value result = nullptr;
  EXPECT_EQ(unofficial_napi_run_script(s.env, realm, &script, Str(s.env, "throw.js"), 0, 0, -1,
                                       true, false, false, nullptr, &result),
            napi_pending_exception);
  bool pending = false;
  ASSERT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
  ASSERT_TRUE(pending);
  napi_value error = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &error), napi_ok);
  napi_value message = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, error, "message", &message), napi_ok);
  EXPECT_EQ(Utf8(s.env, message), "inside realm");
}

// The env's hooks fire for code running in any of its realms.
TEST_F(Test66UnofficialRealm, PromiseRejectCallbackFiresForRealms) {
  EnvScope s(runtime_.get());
  unofficial_napi_realm realm = CreateRealm(s.env);
  ASSERT_NE(realm, nullptr);

  napi_value callback = Eval(s.env,
      "globalThis.rejections = 0;"
      "(event) => { if (event === 0) rejections++; }");
  ASSERT_NE(callback, nullptr);
  ASSERT_EQ(unofficial_napi_set_promise_reject_callback(s.env, callback), napi_ok);

  ASSERT_NE(Eval(s.env, "Promise.reject(1); 0"), nullptr);
  ASSERT_NE(RunIn(s.env, realm, "Promise.reject(2); 0"), nullptr);
  EXPECT_EQ(Int32(s.env, Eval(s.env, "rejections")), 2);
}

// A realm lives while its global, or anything made in it, is reachable, and
// is collected with it.
TEST_F(Test66UnofficialRealm, RealmIsCollectedWithItsGlobal) {
  EnvScope s(runtime_.get());

  // Realms over a sandbox, with and without their own microtask queue, and
  // realms with a fresh global.
  constexpr int kRealms = 9;
  napi_ref globals[kRealms] = {};
  for (int i = 0; i < kRealms; ++i) {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value sandbox = nullptr;
    if (i % 3 != 2) ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    napi_value global = nullptr;
    unofficial_napi_realm realm = CreateRealm(s.env, &global, sandbox, i % 3 == 0);
    ASSERT_NE(realm, nullptr);
    ASSERT_NE(RunIn(s.env, realm, "globalThis.payload = new Array(4096).fill(1); 1"), nullptr);
    ASSERT_EQ(napi_create_reference(s.env, global, 0, &globals[i]), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  // A sandbox still referenced keeps its realm.
  napi_ref kept = nullptr;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value sandbox = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    unofficial_napi_realm realm = CreateRealm(s.env, nullptr, sandbox);
    ASSERT_NE(realm, nullptr);
    ASSERT_NE(RunIn(s.env, realm, "globalThis.marker = 42; 1"), nullptr);
    ASSERT_EQ(napi_create_reference(s.env, sandbox, 1, &kept), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  for (napi_ref global : globals) {
    EXPECT_TRUE(IsCollected(s.env, global));
    ASSERT_EQ(napi_delete_reference(s.env, global), napi_ok);
  }

  // Making more realms releases what the collected ones left behind.
  for (int i = 0; i < 4; ++i) {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    ASSERT_NE(CreateRealm(s.env), nullptr);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_get_reference_value(s.env, kept, &sandbox), napi_ok);
  ASSERT_NE(sandbox, nullptr);
  unofficial_napi_realm realm = nullptr;
  ASSERT_EQ(unofficial_napi_get_realm(s.env, sandbox, &realm), napi_ok);
  ASSERT_NE(realm, nullptr);
  EXPECT_EQ(Int32(s.env, RunIn(s.env, realm, "marker")), 42);
  ASSERT_EQ(napi_delete_reference(s.env, kept), napi_ok);
}

// A realm stays usable while anything made in it is reachable, after its
// global's handle is gone and the provider released collected realms.
TEST_F(Test66UnofficialRealm, RealmOutlivesItsGlobalHandleWhileReachable) {
  EnvScope s(runtime_.get());

  napi_ref functions = nullptr;
  napi_ref weak_sandbox = nullptr;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value sandbox = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
    unofficial_napi_realm realm = CreateRealm(s.env, nullptr, sandbox, true);
    ASSERT_NE(realm, nullptr);
    napi_value made = RunIn(s.env, realm,
                            "globalThis.state = 0;"
                            "[() => { Promise.resolve().then(() => { state++; }); return 7; }]");
    ASSERT_NE(made, nullptr);
    ASSERT_EQ(napi_create_reference(s.env, made, 1, &functions), napi_ok);
    ASSERT_EQ(napi_create_reference(s.env, sandbox, 0, &weak_sandbox), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  for (int i = 0; i < 4; ++i) {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value other = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &other), napi_ok);
    ASSERT_NE(CreateRealm(s.env, nullptr, other, true), nullptr);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);

  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    // The function keeps its realm, and so the sandbox, alive.
    napi_value sandbox = nullptr;
    ASSERT_EQ(napi_get_reference_value(s.env, weak_sandbox, &sandbox), napi_ok);
    ASSERT_NE(sandbox, nullptr);
    unofficial_napi_realm realm = nullptr;
    ASSERT_EQ(unofficial_napi_get_realm(s.env, sandbox, &realm), napi_ok);
    ASSERT_NE(realm, nullptr);

    napi_value array = nullptr, function = nullptr, undefined = nullptr, result = nullptr;
    ASSERT_EQ(napi_get_reference_value(s.env, functions, &array), napi_ok);
    ASSERT_EQ(napi_get_element(s.env, array, 0, &function), napi_ok);
    ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
    ASSERT_EQ(napi_call_function(s.env, undefined, function, 0, nullptr, &result), napi_ok);
    EXPECT_EQ(Int32(s.env, result), 7);
#if defined(NAPI_TEST_ENGINE_V8)
    // The microtask went to the realm's own queue, which drains after a
    // script runs there.
    ASSERT_NE(RunIn(s.env, realm, "0"), nullptr);
    EXPECT_EQ(Int32(s.env, RunIn(s.env, realm, "state")), 1);
#else
    // The microtask went to the engine's one job queue; until it runs, the
    // pending job keeps its realm alive.
    ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                  s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
              napi_ok);
#endif
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  // Once nothing made in the realm is reachable, it is collected.
  ASSERT_EQ(napi_delete_reference(s.env, functions), napi_ok);
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_sandbox));
  ASSERT_EQ(napi_delete_reference(s.env, weak_sandbox), napi_ok);
}

// A job queued by code in a realm can still run after nothing else refers to
// that realm, even when the job's functions belong to the env's realm.
TEST_F(Test66UnofficialRealm, PendingJobRunsAfterItsRealmIsUnreachable) {
  EnvScope s(runtime_.get());

  napi_ref weak_global = nullptr;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value resolve = Eval(s.env,
                              "(() => {"
                              "  let resolve;"
                              "  new Promise((r) => { resolve = r; })"
                              "      .then((value) => { globalThis.got = value; });"
                              "  return resolve;"
                              "})()");
    ASSERT_NE(resolve, nullptr);
    napi_value global = nullptr;
    unofficial_napi_realm realm = CreateRealm(s.env, &global);
    ASSERT_NE(realm, nullptr);
    ASSERT_EQ(napi_set_named_property(s.env, global, "resolve", resolve), napi_ok);
    // Resolving from the realm queues the reaction job there.
    ASSERT_NE(RunIn(s.env, realm, "resolve(5); 0"), nullptr);
    ASSERT_EQ(napi_create_reference(s.env, global, 0, &weak_global), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  for (int i = 0; i < 4; ++i) {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    ASSERT_NE(CreateRealm(s.env), nullptr);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);

  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
            napi_ok);
  EXPECT_EQ(Int32(s.env, Eval(s.env, "globalThis.got")), 5);

  // Once the job has run, the realm is collected.
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_global));
  ASSERT_EQ(napi_delete_reference(s.env, weak_global), napi_ok);
}

#if defined(NAPI_TEST_ENGINE_V8)
namespace {

struct ImportProbe {
  napi_env env = nullptr;
  std::string specifier;
};

napi_value ProbeDynamicImport(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  void* data = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, &data) != napi_ok || argc < 2) {
    return nullptr;
  }
  auto* probe = static_cast<ImportProbe*>(data);
  probe->env = env;
  probe->specifier = Utf8(env, argv[1]);
  napi_value result = nullptr;
  napi_create_object(env, &result);
  return result;
}

}  // namespace

TEST_F(Test66UnofficialRealm, DynamicImportInARealmUsesTheEnvsHook) {
  EnvScope s(runtime_.get());
  unofficial_napi_realm realm = CreateRealm(s.env);
  ASSERT_NE(realm, nullptr);

  ImportProbe probe;
  napi_value callback = nullptr;
  ASSERT_EQ(napi_create_function(s.env, "import", NAPI_AUTO_LENGTH, ProbeDynamicImport, &probe,
                                 &callback),
            napi_ok);
  const unofficial_napi_module_hooks hooks = {
      sizeof(unofficial_napi_module_hooks), UNOFFICIAL_NAPI_MODULE_HOOKS_VERSION, callback,
      nullptr};
  ASSERT_EQ(unofficial_napi_module_wrap_set_hooks(s.env, &hooks), napi_ok);
  ASSERT_NE(RunIn(s.env, realm, "import('from-realm'); 0"), nullptr);
  EXPECT_EQ(probe.env, s.env);
  EXPECT_EQ(probe.specifier, "from-realm");
}

TEST_F(Test66UnofficialRealm, PrepareStackTraceCallbackRunsForRealms) {
  EnvScope s(runtime_.get());
  unofficial_napi_realm realm = CreateRealm(s.env);
  ASSERT_NE(realm, nullptr);

  ASSERT_EQ(unofficial_napi_set_prepare_stack_trace_callback(
                s.env, Eval(s.env, "() => 'prepared stack'")),
            napi_ok);
  EXPECT_EQ(Utf8(s.env, RunIn(s.env, realm, "new Error('x').stack")), "prepared stack");
  EXPECT_EQ(Utf8(s.env, Eval(s.env, "new Error('x').stack")), "prepared stack");
}

TEST_F(Test66UnofficialRealm, OwnMicrotaskQueueDrainsAfterEvaluationInTheRealm) {
  EnvScope s(runtime_.get());
  unofficial_napi_realm realm = CreateRealm(s.env, nullptr, nullptr, true);
  ASSERT_NE(realm, nullptr);
  EXPECT_EQ(Int32(s.env, RunIn(s.env, realm,
                               "globalThis.ran = 0; Promise.resolve().then(() => { ran = 1; }); 0")),
            0);
  EXPECT_EQ(Int32(s.env, RunIn(s.env, realm, "ran")), 1);
}
#endif
