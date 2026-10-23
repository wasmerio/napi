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
  if (napi_get_value_int32(env, value, &out) != napi_ok) return -1;
  return out;
}

std::string Utf8(napi_env env, napi_value value) {
  char buffer[256] = {};
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, buffer, sizeof(buffer), &length) != napi_ok) {
    return std::string();
  }
  return std::string(buffer, length);
}

napi_env CreateRealm(napi_env parent, napi_value* global_out = nullptr) {
  napi_env realm = nullptr;
  if (unofficial_napi_create_realm(parent, nullptr, &realm, global_out) != napi_ok) {
    return nullptr;
  }
  return realm;
}

struct CallRecord {
  napi_env env = nullptr;
  napi_value global = nullptr;
  int calls = 0;
};

napi_value RecordCall(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data) != napi_ok) return nullptr;
  auto* record = static_cast<CallRecord*>(data);
  record->env = env;
  record->calls++;
  napi_value object = nullptr;
  if (napi_create_object(env, &object) != napi_ok) return nullptr;
  return object;
}

// Instance-data finalizer: records 1 for principal_data and 2 for realm_data.
int* g_principal_data = nullptr;

void PushOrder(napi_env /*env*/, void* data, void* hint) {
  static_cast<std::vector<int>*>(hint)->push_back(data == g_principal_data ? 1 : 2);
}

struct CleanupProbe {
  std::vector<int>* order = nullptr;
  int id = 0;
};

void RecordCleanup(void* arg) {
  auto* probe = static_cast<CleanupProbe*>(arg);
  probe->order->push_back(probe->id);
}

}  // namespace

TEST_F(Test66UnofficialRealm, CreateRealmValidatesItsArguments) {
  EnvScope s(runtime_.get());
  napi_env realm = nullptr;

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
}

TEST_F(Test66UnofficialRealm, RealmHasItsOwnGlobal) {
  EnvScope s(runtime_.get());
  napi_value realm_global = nullptr;
  napi_env realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);
  EXPECT_NE(realm, s.env);

  EXPECT_TRUE(StrictEquals(s.env, Global(realm), realm_global));
  EXPECT_FALSE(StrictEquals(s.env, Global(realm), Global(s.env)));
  EXPECT_TRUE(StrictEquals(s.env, Eval(realm, "globalThis"), realm_global));

  ASSERT_NE(Eval(realm, "globalThis.answer = 42"), nullptr);
  EXPECT_EQ(Utf8(s.env, Eval(s.env, "typeof answer")), "undefined");
  EXPECT_EQ(Int32(realm, Eval(realm, "answer")), 42);
}

TEST_F(Test66UnofficialRealm, RealmCreatesObjectsWithItsIntrinsics) {
  EnvScope s(runtime_.get());
  napi_env realm = CreateRealm(s.env);
  ASSERT_NE(realm, nullptr);

  napi_value realm_object_prototype = Eval(realm, "Object.prototype");
  napi_value realm_array_prototype = Eval(realm, "Array.prototype");
  napi_value realm_error_prototype = Eval(realm, "Error.prototype");
  ASSERT_NE(realm_object_prototype, nullptr);
  EXPECT_FALSE(StrictEquals(s.env, realm_object_prototype, Eval(s.env, "Object.prototype")));

  napi_value object = nullptr;
  napi_value array = nullptr;
  napi_value error = nullptr;
  ASSERT_EQ(napi_create_object(realm, &object), napi_ok);
  ASSERT_EQ(napi_create_array(realm, &array), napi_ok);
  ASSERT_EQ(napi_create_error(realm, nullptr, Str(realm, "boom"), &error), napi_ok);
  EXPECT_TRUE(StrictEquals(realm, Prototype(realm, object), realm_object_prototype));
  EXPECT_TRUE(StrictEquals(realm, Prototype(realm, array), realm_array_prototype));
  EXPECT_TRUE(StrictEquals(realm, Prototype(realm, error), realm_error_prototype));

  // The parent keeps creating in its own realm.
  ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, Prototype(s.env, object), Eval(s.env, "Object.prototype")));
}

TEST_F(Test66UnofficialRealm, ValuesAreSharedAcrossRealms) {
  EnvScope s(runtime_.get());
  napi_env realm = CreateRealm(s.env);
  ASSERT_NE(realm, nullptr);

  napi_handle_scope scope = nullptr;
  ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
  napi_value shared = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &shared), napi_ok);
  ASSERT_EQ(napi_set_named_property(realm, Global(realm), "shared", shared), napi_ok);
  ASSERT_NE(Eval(realm, "shared.x = 7"), nullptr);

  napi_value x = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, shared, "x", &x), napi_ok);
  EXPECT_EQ(Int32(s.env, x), 7);

  // A value made through the realm env is usable through the parent.
  napi_value from_realm = Eval(realm, "({ y: 9 })");
  napi_value y = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, from_realm, "y", &y), napi_ok);
  EXPECT_EQ(Int32(s.env, y), 9);
  ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
}

TEST_F(Test66UnofficialRealm, NativeFunctionsRunInTheirRealm) {
  EnvScope s(runtime_.get());
  napi_value realm_global = nullptr;
  napi_env realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);

  CallRecord record;
  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_function(realm, "probe", NAPI_AUTO_LENGTH, RecordCall, &record, &fn),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, Global(s.env), "probe", fn), napi_ok);

  napi_value result = Eval(s.env, "probe()");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(record.calls, 1);
  EXPECT_EQ(record.env, realm);
  EXPECT_TRUE(StrictEquals(s.env, Prototype(s.env, result), Eval(realm, "Object.prototype")));
}

TEST_F(Test66UnofficialRealm, GetRealmFindsRealmsByGlobalAndFunction) {
  EnvScope s(runtime_.get());
  napi_value realm_global = nullptr;
  napi_env realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);

  napi_env found = nullptr;
  ASSERT_EQ(unofficial_napi_get_realm(s.env, realm_global, &found), napi_ok);
  EXPECT_EQ(found, realm);
  found = nullptr;
  ASSERT_EQ(unofficial_napi_get_realm(realm, Global(s.env), &found), napi_ok);
  EXPECT_EQ(found, s.env);

  napi_value fn = Eval(realm, "(function f() {})");
  ASSERT_NE(fn, nullptr);
  found = nullptr;
  ASSERT_EQ(unofficial_napi_get_realm(s.env, fn, &found), napi_ok);
  EXPECT_EQ(found, realm);

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

  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = sandbox;
  napi_env realm = nullptr;
  napi_value global_out = nullptr;
  ASSERT_EQ(unofficial_napi_create_realm(s.env, &options, &realm, &global_out), napi_ok);
  ASSERT_NE(realm, nullptr);
  EXPECT_TRUE(StrictEquals(s.env, global_out, sandbox));

  napi_env found = nullptr;
  ASSERT_EQ(unofficial_napi_get_realm(s.env, sandbox, &found), napi_ok);
  EXPECT_EQ(found, realm);

  // Running in the realm env with no sandbox argument uses the realm's sandbox.
  const unofficial_napi_js_source source =
      unofficial_napi_js_source_from_text(Str(s.env, "output = input * 2; input"));
  napi_value result = nullptr;
  ASSERT_EQ(unofficial_napi_contextify_run_script(realm,
                                                  nullptr,
                                                  &source,
                                                  Str(s.env, "sandbox.js"),
                                                  0,
                                                  0,
                                                  -1,
                                                  true,
                                                  false,
                                                  false,
                                                  nullptr,
                                                  &result),
            napi_ok);
  EXPECT_EQ(Int32(s.env, result), 5);
  napi_value output = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, sandbox, "output", &output), napi_ok);
  EXPECT_EQ(Int32(s.env, output), 10);
  EXPECT_EQ(Utf8(s.env, Eval(s.env, "typeof output")), "undefined");
}

TEST_F(Test66UnofficialRealm, MakeContextCreatesARealmEnv) {
  EnvScope s(runtime_.get());
  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
  napi_value three = nullptr;
  ASSERT_EQ(napi_create_int32(s.env, 3, &three), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, sandbox, "x", three), napi_ok);

  napi_value name = Str(s.env, "ctx");
  napi_value contextified = nullptr;
  ASSERT_EQ(unofficial_napi_contextify_make_context(
                s.env, sandbox, name, nullptr, true, true, false, nullptr, &contextified),
            napi_ok);

  napi_env realm = nullptr;
  ASSERT_EQ(unofficial_napi_get_realm(s.env, sandbox, &realm), napi_ok);
  ASSERT_NE(realm, nullptr);
  EXPECT_NE(realm, s.env);

  // The old sandbox argument and the realm env select the same realm.
  const unofficial_napi_js_source source =
      unofficial_napi_js_source_from_text(Str(s.env, "x + 1"));
  napi_value via_sandbox = nullptr;
  napi_value via_realm = nullptr;
  ASSERT_EQ(unofficial_napi_contextify_run_script(s.env, sandbox, &source, Str(s.env, "a.js"),
                                                  0, 0, -1, true, false, false, nullptr,
                                                  &via_sandbox),
            napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_run_script(realm, nullptr, &source, Str(s.env, "b.js"),
                                                  0, 0, -1, true, false, false, nullptr,
                                                  &via_realm),
            napi_ok);
  EXPECT_EQ(Int32(s.env, via_sandbox), 4);
  EXPECT_EQ(Int32(s.env, via_realm), 4);
}

TEST_F(Test66UnofficialRealm, PromiseRejectCallbackFallsBackToTheParent) {
  EnvScope s(runtime_.get());
  napi_env realm = CreateRealm(s.env);
  ASSERT_NE(realm, nullptr);
  napi_env nested = CreateRealm(realm);
  ASSERT_NE(nested, nullptr);

  napi_value parent_callback = Eval(s.env,
      "globalThis.parentRejections = 0;"
      "(event) => { if (event === 0) parentRejections++; }");
  ASSERT_NE(parent_callback, nullptr);
  ASSERT_EQ(unofficial_napi_set_promise_reject_callback(s.env, parent_callback), napi_ok);

  ASSERT_NE(Eval(realm, "Promise.reject(1); 0"), nullptr);
  ASSERT_NE(Eval(nested, "Promise.reject(2); 0"), nullptr);
  EXPECT_EQ(Int32(s.env, Eval(s.env, "parentRejections")), 2);

  napi_value realm_callback = Eval(realm,
      "globalThis.realmRejections = 0;"
      "(event) => { if (event === 0) realmRejections++; }");
  ASSERT_NE(realm_callback, nullptr);
  ASSERT_EQ(unofficial_napi_set_promise_reject_callback(realm, realm_callback), napi_ok);

  ASSERT_NE(Eval(realm, "Promise.reject(3); 0"), nullptr);
  ASSERT_NE(Eval(nested, "Promise.reject(4); 0"), nullptr);
  ASSERT_NE(Eval(s.env, "Promise.reject(5); 0"), nullptr);
  EXPECT_EQ(Int32(realm, Eval(realm, "realmRejections")), 2);
  EXPECT_EQ(Int32(s.env, Eval(s.env, "parentRejections")), 3);
}

TEST_F(Test66UnofficialRealm, ModulesEvaluateInTheirRealm) {
  EnvScope s(runtime_.get());
  napi_value realm_global = nullptr;
  napi_env realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);

  napi_value wrapper = nullptr;
  napi_value undefined = nullptr;
  ASSERT_EQ(napi_create_object(realm, &wrapper), napi_ok);
  ASSERT_EQ(napi_get_undefined(realm, &undefined), napi_ok);
  const unofficial_napi_js_source source =
      unofficial_napi_js_source_from_text(Str(realm, "export const g = globalThis;"));
  unofficial_napi_module_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  options.kind = unofficial_napi_module_source_text;
  options.wrapper = wrapper;
  options.url = Str(realm, "realm.mjs");
  options.context_or_undefined = undefined;
  options.payload.source_text.source = &source;
  options.payload.source_text.host_defined_option_id = undefined;
  unofficial_napi_module_create_result created{};
  ASSERT_EQ(unofficial_napi_module_wrap_create(realm, &options, &created), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_link(realm, created.module, 0, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_instantiate(realm, created.module), napi_ok);
  napi_value evaluated = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_evaluate(realm, created.module, -1, false, &evaluated),
            napi_ok);
  napi_value ns = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_get_namespace(realm, created.module, &ns), napi_ok);
  napi_value g = nullptr;
  ASSERT_EQ(napi_get_named_property(realm, ns, "g", &g), napi_ok);
  EXPECT_TRUE(StrictEquals(realm, g, realm_global));
  EXPECT_FALSE(StrictEquals(realm, g, Global(s.env)));
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(realm, created.module), napi_ok);
}

TEST_F(Test66UnofficialRealm, RealmEnvsHaveTheirOwnInstanceData) {
  std::vector<int> finalized;
  int principal_data = 0;
  int realm_data = 0;
  g_principal_data = &principal_data;
  {
    EnvScope s(runtime_.get());
    napi_env realm = CreateRealm(s.env);
    ASSERT_NE(realm, nullptr);
    ASSERT_EQ(napi_set_instance_data(s.env, &principal_data, PushOrder, &finalized), napi_ok);
    ASSERT_EQ(napi_set_instance_data(realm, &realm_data, PushOrder, &finalized), napi_ok);
    void* data = nullptr;
    ASSERT_EQ(napi_get_instance_data(realm, &data), napi_ok);
    EXPECT_EQ(data, &realm_data);
    ASSERT_EQ(napi_get_instance_data(s.env, &data), napi_ok);
    EXPECT_EQ(data, &principal_data);
  }
  // Both are finalized at release, the realm's first.
  EXPECT_EQ(finalized, std::vector<int>({2, 1}));
}

TEST_F(Test66UnofficialRealm, RealmEnvsAreReleasedBeforeTheirAgent) {
  std::vector<int> order;
  CleanupProbe principal{&order, 0};
  CleanupProbe first{&order, 1};
  CleanupProbe second{&order, 2};
  CleanupProbe nested{&order, 3};
  {
    EnvScope s(runtime_.get());
    napi_env realm1 = CreateRealm(s.env);
    napi_env realm2 = CreateRealm(s.env);
    ASSERT_NE(realm1, nullptr);
    ASSERT_NE(realm2, nullptr);
    napi_env realm3 = CreateRealm(realm1);
    ASSERT_NE(realm3, nullptr);
    ASSERT_EQ(napi_add_env_cleanup_hook(s.env, RecordCleanup, &principal), napi_ok);
    ASSERT_EQ(napi_add_env_cleanup_hook(realm1, RecordCleanup, &first), napi_ok);
    ASSERT_EQ(napi_add_env_cleanup_hook(realm2, RecordCleanup, &second), napi_ok);
    ASSERT_EQ(napi_add_env_cleanup_hook(realm3, RecordCleanup, &nested), napi_ok);
  }
  // Realm envs go first, newest first; the principal env goes last.
  EXPECT_EQ(order, std::vector<int>({3, 2, 1, 0}));
}
