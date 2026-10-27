#ifdef NAPI_MODULE
#undef NAPI_MODULE
#endif
#include "node_api.h"

#include "test_env.h"

#include "unofficial_napi.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

// The current realm: what N-API creates while a realm is entered or one of
// its native functions runs, and how long realms entered or used that way
// live.
class Test69UnofficialRealmEntry : public FixtureTestBase {};

namespace {

napi_value Str(napi_env env, const char* value) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out) != napi_ok) return nullptr;
  return out;
}

// Runs `source` with napi_run_script, in the current realm.
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
  return a != nullptr && b != nullptr && napi_strict_equals(env, a, b, &same) == napi_ok && same;
}

napi_value Global(napi_env env) {
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok) return nullptr;
  return global;
}

napi_value Undefined(napi_env env) {
  napi_value undefined = nullptr;
  if (napi_get_undefined(env, &undefined) != napi_ok) return nullptr;
  return undefined;
}

std::string Utf8(napi_env env, napi_value value) {
  char buffer[512] = {};
  size_t length = 0;
  if (value == nullptr ||
      napi_get_value_string_utf8(env, value, buffer, sizeof(buffer), &length) != napi_ok) {
    return "<not a string>";
  }
  return std::string(buffer, length);
}

unofficial_napi_realm CreateRealm(napi_env env,
                                  napi_value* global_out = nullptr,
                                  napi_value sandbox = nullptr) {
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = sandbox;
  unofficial_napi_realm realm = nullptr;
  if (unofficial_napi_create_realm(env, &options, &realm, global_out) != napi_ok) return nullptr;
  return realm;
}

unofficial_napi_realm_scope Enter(napi_env env, unofficial_napi_realm realm) {
  unofficial_napi_realm_scope scope = 0;
  EXPECT_EQ(unofficial_napi_enter_realm(env, realm, &scope), napi_ok);
  EXPECT_NE(scope, 0u);
  return scope;
}

void Leave(napi_env env, unofficial_napi_realm_scope scope) {
  EXPECT_EQ(unofficial_napi_leave_realm(env, scope), napi_ok);
}

bool IsCollected(napi_env env, napi_ref ref) {
  napi_value value = nullptr;
  return napi_get_reference_value(env, ref, &value) == napi_ok && value == nullptr;
}

napi_value ReturnUndefined(napi_env env, napi_callback_info) {
  return Undefined(env);
}

// Makes one of each kind of value N-API creates, in the current realm:
// [global, object, array, function, error, type error, promise, date,
//  array buffer, typed array]. A thrown error is checked on its own, since a
// throw from a callback reaches its caller.
napi_value MakeValues(napi_env env) {
  napi_value values[10] = {};
  napi_deferred deferred = nullptr;
  napi_value buffer = nullptr;
  // Reports the first call that fails.
  int step = 0;
  auto ok = [&](napi_status status) {
    ++step;
    if (status == napi_ok) return true;
    std::fprintf(stderr, "MakeValues: call %d failed with status %d\n", step, status);
    return false;
  };
  if (!ok(napi_get_global(env, &values[0])) || !ok(napi_create_object(env, &values[1])) ||
      !ok(napi_create_array(env, &values[2])) ||
      !ok(napi_create_function(env, "made", NAPI_AUTO_LENGTH, ReturnUndefined, nullptr,
                               &values[3])) ||
      !ok(napi_create_error(env, nullptr, Str(env, "error"), &values[4])) ||
      !ok(napi_create_type_error(env, nullptr, Str(env, "type error"), &values[5])) ||
      !ok(napi_create_promise(env, &deferred, &values[6])) ||
      !ok(napi_resolve_deferred(env, deferred, Undefined(env))) ||
      !ok(napi_create_date(env, 0, &values[7])) ||
      !ok(napi_create_arraybuffer(env, 8, nullptr, &buffer)) ||
      !ok(napi_create_typedarray(env, napi_uint8_array, 8, buffer, 0, &values[9]))) {
    return nullptr;
  }
  values[8] = buffer;
  napi_value array = nullptr;
  if (napi_create_array_with_length(env, 10, &array) != napi_ok) return nullptr;
  for (uint32_t i = 0; i < 10; ++i) {
    if (napi_set_element(env, array, i, values[i]) != napi_ok) return nullptr;
  }
  return array;
}

napi_value MakeValuesCallback(napi_env env, napi_callback_info) {
  return MakeValues(env);
}

// Lists what in `values` (from MakeValues) does not come from the realm whose
// global is `realm_global`; empty when everything does. Runs in the env's
// realm, so call it with no realm entered.
std::string ValuesNotFrom(napi_env env, napi_value values, napi_value realm_global) {
  napi_value global = Global(env);
  if (values == nullptr || realm_global == nullptr ||
      napi_set_named_property(env, global, "__values", values) != napi_ok ||
      napi_set_named_property(env, global, "__realm", realm_global) != napi_ok) {
    return "<no values>";
  }
  napi_value report = Eval(env,
                           "(() => {"
                           "  const [g, o, a, f, e, te, p, d, ab, ta] = __values;"
                           "  const R = __realm;"
                           "  const proto = Object.getPrototypeOf;"
                           "  const wrong = [];"
                           "  const check = (name, ok) => { if (!ok) wrong.push(name); };"
                           "  check('global', g === R);"
                           "  check('object', proto(o) === R.Object.prototype);"
                           "  check('array', proto(a) === R.Array.prototype);"
                           "  check('function', proto(f) === R.Function.prototype);"
                           "  check('error', proto(e) === R.Error.prototype);"
                           "  check('type error', proto(te) === R.TypeError.prototype);"
                           "  check('promise', proto(p) === R.Promise.prototype);"
                           "  check('date', proto(d) === R.Date.prototype);"
                           "  check('array buffer', proto(ab) === R.ArrayBuffer.prototype);"
                           "  check('typed array', proto(ta) === R.Uint8Array.prototype);"
                           "  delete globalThis.__values;"
                           "  delete globalThis.__realm;"
                           "  return wrong.join(', ');"
                           "})()");
  return Utf8(env, report);
}

}  // namespace

TEST_F(Test69UnofficialRealmEntry, EnteredRealmMakesValuesWithItsIntrinsics) {
  EnvScope s(runtime_.get());
  napi_value env_global = Global(s.env);
  napi_value realm_global = nullptr;
  unofficial_napi_realm realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);

  unofficial_napi_realm_scope scope = Enter(s.env, realm);
  napi_value in_realm = MakeValues(s.env);
  Leave(s.env, scope);
  ASSERT_NE(in_realm, nullptr);
  EXPECT_EQ(ValuesNotFrom(s.env, in_realm, realm_global), "");

  // Without a scope, values come from the env's realm again.
  napi_value in_env = MakeValues(s.env);
  ASSERT_NE(in_env, nullptr);
  EXPECT_EQ(ValuesNotFrom(s.env, in_env, env_global), "");
}

TEST_F(Test69UnofficialRealmEntry, GetGlobalFollowsTheCurrentRealm) {
  EnvScope s(runtime_.get());
  napi_value env_global = Global(s.env);
  napi_value realm_global = nullptr;
  unofficial_napi_realm realm = CreateRealm(s.env, &realm_global);
  ASSERT_NE(realm, nullptr);
  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
  unofficial_napi_realm sandbox_realm = CreateRealm(s.env, nullptr, sandbox);
  ASSERT_NE(sandbox_realm, nullptr);

  unofficial_napi_realm_scope scope = Enter(s.env, realm);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), realm_global));
  // The current realm's global names the realm.
  unofficial_napi_realm found = nullptr;
  ASSERT_EQ(unofficial_napi_get_realm(s.env, Global(s.env), &found), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, RunIn(s.env, found, "globalThis"), realm_global));
  // napi_run_script runs in the current realm; a NULL realm still means the
  // env's own.
  EXPECT_TRUE(StrictEquals(s.env, Eval(s.env, "globalThis"), realm_global));
  EXPECT_TRUE(StrictEquals(s.env, RunIn(s.env, nullptr, "globalThis"), env_global));

  // A sandbox realm's global is its sandbox, as create_realm reported.
  unofficial_napi_realm_scope sandbox_scope = Enter(s.env, sandbox_realm);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), sandbox));
  found = nullptr;
  ASSERT_EQ(unofficial_napi_get_realm(s.env, Global(s.env), &found), napi_ok);
  EXPECT_NE(found, nullptr);
  Leave(s.env, sandbox_scope);

  // NULL enters the env's realm from inside another.
  unofficial_napi_realm_scope env_scope = Enter(s.env, nullptr);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));
  Leave(s.env, env_scope);

  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), realm_global));
  Leave(s.env, scope);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));
}

TEST_F(Test69UnofficialRealmEntry, ScopesNestAndCloseInOrder) {
  EnvScope s(runtime_.get());
  napi_value env_global = Global(s.env);
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  EXPECT_EQ(unofficial_napi_enter_realm(nullptr, a, nullptr), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_enter_realm(s.env, a, nullptr), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_leave_realm(s.env, 0), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_leave_realm(s.env, 12345), napi_invalid_arg);

  unofficial_napi_realm_scope in_a = Enter(s.env, a);
  unofficial_napi_realm_scope in_b = Enter(s.env, b);
  unofficial_napi_realm_scope in_env = Enter(s.env, nullptr);
  EXPECT_EQ(std::set<unofficial_napi_realm_scope>({in_a, in_b, in_env}).size(), 3u);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));

  // Only the innermost scope can be left; a refused leave changes nothing.
  EXPECT_EQ(unofficial_napi_leave_realm(s.env, in_b), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_leave_realm(s.env, in_a), napi_invalid_arg);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));

  Leave(s.env, in_env);
  EXPECT_EQ(unofficial_napi_leave_realm(s.env, in_env), napi_invalid_arg);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), b_global));
  Leave(s.env, in_b);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), a_global));
  Leave(s.env, in_a);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));
  EXPECT_EQ(unofficial_napi_leave_realm(s.env, in_a), napi_invalid_arg);

  // A token is never reused.
  unofficial_napi_realm_scope again = Enter(s.env, a);
  EXPECT_EQ(std::set<unofficial_napi_realm_scope>({in_a, in_b, in_env, again}).size(), 4u);
  Leave(s.env, again);
}

TEST_F(Test69UnofficialRealmEntry, NativeFunctionRunsInTheRealmThatMadeIt) {
  EnvScope s(runtime_.get());
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  napi_value probe = nullptr;
  unofficial_napi_realm_scope scope = Enter(s.env, a);
  ASSERT_EQ(napi_create_function(s.env, "probe", NAPI_AUTO_LENGTH, MakeValuesCallback, nullptr,
                                 &probe),
            napi_ok);
  Leave(s.env, scope);
  ASSERT_EQ(napi_set_named_property(s.env, b_global, "probe", probe), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, a_global, "probe", probe), napi_ok);

  // The function is one of A's.
  EXPECT_TRUE(StrictEquals(s.env, RunIn(s.env, a, "Object.getPrototypeOf(probe)"),
                           RunIn(s.env, a, "Function.prototype")));

  // Called from B, from A and from the env's realm, it runs in A.
  EXPECT_EQ(ValuesNotFrom(s.env, RunIn(s.env, b, "probe()"), a_global), "");
  EXPECT_EQ(ValuesNotFrom(s.env, RunIn(s.env, a, "probe()"), a_global), "");
  napi_value result = nullptr;
  ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), probe, 0, nullptr, &result), napi_ok);
  EXPECT_EQ(ValuesNotFrom(s.env, result, a_global), "");

  // Also while B is entered, and B is current again afterwards.
  scope = Enter(s.env, b);
  result = nullptr;
  ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), probe, 0, nullptr, &result), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), b_global));
  Leave(s.env, scope);
  EXPECT_EQ(ValuesNotFrom(s.env, result, a_global), "");
}

// EdgeJS never enters a realm: its functions run in the env's realm wherever
// they are called from.
TEST_F(Test69UnofficialRealmEntry, FunctionMadeOutsideAScopeRunsInTheEnvsRealm) {
  EnvScope s(runtime_.get());
  napi_value env_global = Global(s.env);
  napi_value a_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  ASSERT_NE(a, nullptr);

  napi_value probe = nullptr;
  ASSERT_EQ(napi_create_function(s.env, "probe", NAPI_AUTO_LENGTH, MakeValuesCallback, nullptr,
                                 &probe),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, a_global, "probe", probe), napi_ok);
  EXPECT_EQ(ValuesNotFrom(s.env, RunIn(s.env, a, "probe()"), env_global), "");

  unofficial_napi_realm_scope scope = Enter(s.env, a);
  napi_value result = nullptr;
  ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), probe, 0, nullptr, &result), napi_ok);
  // A function made with the env's realm entered from inside A is the env's.
  unofficial_napi_realm_scope env_scope = Enter(s.env, nullptr);
  napi_value env_probe = nullptr;
  ASSERT_EQ(napi_create_function(s.env, "envProbe", NAPI_AUTO_LENGTH, MakeValuesCallback, nullptr,
                                 &env_probe),
            napi_ok);
  Leave(s.env, env_scope);
  Leave(s.env, scope);
  EXPECT_EQ(ValuesNotFrom(s.env, result, env_global), "");

  ASSERT_EQ(napi_set_named_property(s.env, a_global, "envProbe", env_probe), napi_ok);
  EXPECT_EQ(ValuesNotFrom(s.env, RunIn(s.env, a, "envProbe()"), env_global), "");
}

namespace {

napi_value ReturnThis(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &self, nullptr) != napi_ok) return nullptr;
  return self;
}

napi_value GetGlobalCallback(napi_env env, napi_callback_info) {
  return Global(env);
}

}  // namespace

TEST_F(Test69UnofficialRealmEntry, ClassesAndPropertiesBelongToTheirRealm) {
  EnvScope s(runtime_.get());
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  const napi_property_descriptor properties[] = {
      {"values", nullptr, MakeValuesCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"global", nullptr, nullptr, GetGlobalCallback, nullptr, nullptr, napi_default, nullptr},
      {"staticValues", nullptr, MakeValuesCallback, nullptr, nullptr, nullptr, napi_static,
       nullptr},
  };
  napi_value thing = nullptr;
  napi_value object = nullptr;
  unofficial_napi_realm_scope scope = Enter(s.env, a);
  ASSERT_EQ(napi_define_class(s.env, "Thing", NAPI_AUTO_LENGTH, ReturnThis, nullptr, 3, properties,
                              &thing),
            napi_ok);
  ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);
  ASSERT_EQ(napi_define_properties(s.env, object, 2, properties), napi_ok);
  Leave(s.env, scope);
  ASSERT_EQ(napi_set_named_property(s.env, b_global, "Thing", thing), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, b_global, "object", object), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, a_global, "Thing", thing), napi_ok);

  EXPECT_EQ(Utf8(s.env, RunIn(s.env, a,
                              "[Object.getPrototypeOf(Thing) === Function.prototype,"
                              " Object.getPrototypeOf(Thing.prototype) === Object.prototype,"
                              " Object.getPrototypeOf(new Thing()) === Thing.prototype].join()")),
            "true,true,true");
  // Constructed, called and read from B, everything runs in A.
  EXPECT_TRUE(StrictEquals(s.env, RunIn(s.env, b, "new Thing().global"), a_global));
  EXPECT_EQ(ValuesNotFrom(s.env, RunIn(s.env, b, "new Thing().values()"), a_global), "");
  EXPECT_EQ(ValuesNotFrom(s.env, RunIn(s.env, b, "Thing.staticValues()"), a_global), "");
  EXPECT_TRUE(StrictEquals(s.env, RunIn(s.env, b, "object.global"), a_global));
  EXPECT_EQ(ValuesNotFrom(s.env, RunIn(s.env, b, "object.values()"), a_global), "");
}

TEST_F(Test69UnofficialRealmEntry, DeferredBelongsToItsRealm) {
  EnvScope s(runtime_.get());
  napi_value a_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  ASSERT_NE(a, nullptr);

  napi_deferred deferred = nullptr;
  napi_value promise = nullptr;
  unofficial_napi_realm_scope scope = Enter(s.env, a);
  ASSERT_EQ(napi_create_promise(s.env, &deferred, &promise), napi_ok);
  Leave(s.env, scope);
  ASSERT_EQ(napi_set_named_property(s.env, a_global, "promise", promise), napi_ok);
  EXPECT_EQ(Utf8(s.env, RunIn(s.env, a,
                              "promise.then((v) => { globalThis.got = v; });"
                              "String(Object.getPrototypeOf(promise) === Promise.prototype)")),
            "true");

  // Settled later from the env's realm.
  napi_value value = nullptr;
  ASSERT_EQ(napi_create_int32(s.env, 9, &value), napi_ok);
  ASSERT_EQ(napi_resolve_deferred(s.env, deferred, value), napi_ok);
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
            napi_ok);
  EXPECT_EQ(Utf8(s.env, RunIn(s.env, a, "String(globalThis.got)")), "9");
}

namespace {

struct LeakyScope {
  unofficial_napi_realm realm = nullptr;
  unofficial_napi_realm_scope outer = 0;
  unofficial_napi_realm_scope leaked = 0;
  napi_status leave_outer = napi_ok;
};

// Enters a realm and returns without leaving it, after trying to leave a
// scope its caller opened.
napi_value EnterAndReturn(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data) != napi_ok) return nullptr;
  auto* record = static_cast<LeakyScope*>(data);
  if (record->outer != 0) record->leave_outer = unofficial_napi_leave_realm(env, record->outer);
  if (unofficial_napi_enter_realm(env, record->realm, &record->leaked) != napi_ok) return nullptr;
  return Global(env);
}

}  // namespace

TEST_F(Test69UnofficialRealmEntry, ScopeLeftOpenClosesWhenTheCallbackReturns) {
  EnvScope s(runtime_.get());
  napi_value env_global = Global(s.env);
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  LeakyScope record;
  record.realm = b;
  napi_value leaky = nullptr;
  ASSERT_EQ(napi_create_function(s.env, "leaky", NAPI_AUTO_LENGTH, EnterAndReturn, &record,
                                 &leaky),
            napi_ok);

  napi_value result = nullptr;
  ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), leaky, 0, nullptr, &result), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, result, b_global));
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));
  EXPECT_EQ(unofficial_napi_leave_realm(s.env, record.leaked), napi_invalid_arg);

  // A callback cannot leave a scope its caller opened.
  record.outer = Enter(s.env, a);
  ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), leaky, 0, nullptr, &result), napi_ok);
  EXPECT_EQ(record.leave_outer, napi_invalid_arg);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), a_global));
  Leave(s.env, record.outer);

  // Called from script, too.
  record.outer = 0;
  ASSERT_EQ(napi_set_named_property(s.env, a_global, "leaky", leaky), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, RunIn(s.env, a, "leaky()"), b_global));
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));
}

TEST_F(Test69UnofficialRealmEntry, PendingExceptionSurvivesLeavingARealm) {
  EnvScope s(runtime_.get());
  napi_value a_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  ASSERT_NE(a, nullptr);

  unofficial_napi_realm_scope scope = Enter(s.env, a);
  const napi_status thrown = napi_throw_type_error(s.env, nullptr, "boom");
  EXPECT_TRUE(thrown == napi_ok || thrown == napi_pending_exception);
  Leave(s.env, scope);
  bool pending = false;
  ASSERT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
  EXPECT_TRUE(pending);
  napi_value error = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &error), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, a_global, "error", error), napi_ok);
  EXPECT_EQ(Utf8(s.env, RunIn(s.env, a,
                              "String(Object.getPrototypeOf(error) === TypeError.prototype &&"
                              " error.message === 'boom')")),
            "true");
}

TEST_F(Test69UnofficialRealmEntry, EnteringAnUnknownOrCollectedRealmFails) {
  EnvScope s(runtime_.get());
  unofficial_napi_realm_scope scope = 0;

  // Anything but one of this env's realms is refused without being read.
  int not_a_realm = 0;
  EXPECT_EQ(unofficial_napi_enter_realm(
                s.env, reinterpret_cast<unofficial_napi_realm>(&not_a_realm), &scope),
            napi_invalid_arg);
  EXPECT_EQ(scope, 0u);

  // A handle that outlived its handle scope, once the realm is collected.
  unofficial_napi_realm stale = nullptr;
  napi_ref weak_global = nullptr;
  {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    napi_value global = nullptr;
    stale = CreateRealm(s.env, &global);
    ASSERT_NE(stale, nullptr);
    ASSERT_EQ(napi_create_reference(s.env, global, 0, &weak_global), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  ASSERT_TRUE(IsCollected(s.env, weak_global));
  EXPECT_EQ(unofficial_napi_enter_realm(s.env, stale, &scope), napi_invalid_arg);
  EXPECT_EQ(scope, 0u);
  ASSERT_EQ(napi_delete_reference(s.env, weak_global), napi_ok);

  // No JavaScript runs on entering or leaving, so both work while
  // terminating.
  unofficial_napi_realm live = CreateRealm(s.env);
  ASSERT_NE(live, nullptr);
  ASSERT_EQ(unofficial_napi_terminate_execution(s.env), napi_ok);
  ASSERT_EQ(unofficial_napi_enter_realm(s.env, live, &scope), napi_ok);
  EXPECT_EQ(unofficial_napi_leave_realm(s.env, scope), napi_ok);
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
}

TEST_F(Test69UnofficialRealmEntry, RealmLivesWhileItsNativeFunctionIsReachable) {
  EnvScope s(runtime_.get());

  // Many realms, each reachable only through a native function made in it.
  constexpr int kRealms = 64;
  napi_ref functions[kRealms] = {};
  napi_ref globals[kRealms] = {};
  for (int i = 0; i < kRealms; ++i) {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    napi_value global = nullptr;
    unofficial_napi_realm realm = CreateRealm(s.env, &global);
    ASSERT_NE(realm, nullptr);
    ASSERT_NE(RunIn(s.env, realm, "globalThis.payload = new Array(4096).fill(1); 1"), nullptr);
    unofficial_napi_realm_scope scope = Enter(s.env, realm);
    napi_value probe = nullptr;
    ASSERT_EQ(napi_create_function(s.env, "probe", NAPI_AUTO_LENGTH, MakeValuesCallback, nullptr,
                                   &probe),
              napi_ok);
    Leave(s.env, scope);
    ASSERT_EQ(napi_create_reference(s.env, probe, 1, &functions[i]), napi_ok);
    ASSERT_EQ(napi_create_reference(s.env, global, 0, &globals[i]), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }

  // Making more realms releases what collected realms left behind; the
  // reachable ones stay.
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  for (int i = 0; i < 4; ++i) {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    ASSERT_NE(CreateRealm(s.env), nullptr);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);

  for (int i = 0; i < kRealms; ++i) {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    napi_value global = nullptr, probe = nullptr, result = nullptr;
    ASSERT_EQ(napi_get_reference_value(s.env, globals[i], &global), napi_ok);
    ASSERT_NE(global, nullptr);
    ASSERT_EQ(napi_get_reference_value(s.env, functions[i], &probe), napi_ok);
    ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), probe, 0, nullptr, &result), napi_ok);
    EXPECT_EQ(ValuesNotFrom(s.env, result, global), "");
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }

  // Once its function is unreachable, each realm is collected.
  for (napi_ref function : functions) ASSERT_EQ(napi_delete_reference(s.env, function), napi_ok);
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  for (napi_ref global : globals) {
    EXPECT_TRUE(IsCollected(s.env, global));
    ASSERT_EQ(napi_delete_reference(s.env, global), napi_ok);
  }
}

namespace {

struct FinalizerRecord {
  napi_ref env_global = nullptr;
  bool ran = false;
  bool saw_env_global = false;
};

void RecordFinalizerRealm(node_api_basic_env basic_env, void*, void* hint) {
  auto env = const_cast<napi_env>(basic_env);
  auto* record = static_cast<FinalizerRecord*>(hint);
  record->ran = true;
  napi_value env_global = nullptr;
  if (napi_get_reference_value(env, record->env_global, &env_global) != napi_ok) return;
  record->saw_env_global = StrictEquals(env, Global(env), env_global);
}

}  // namespace

TEST_F(Test69UnofficialRealmEntry, FinalizersRunInTheEnvsRealm) {
  EnvScope s(runtime_.get());
  unofficial_napi_realm a = CreateRealm(s.env);
  ASSERT_NE(a, nullptr);
  FinalizerRecord record;
  ASSERT_EQ(napi_create_reference(s.env, Global(s.env), 1, &record.env_global), napi_ok);

  // The object becomes unreachable, and is collected and finalized, with A
  // entered.
  unofficial_napi_realm_scope scope = Enter(s.env, a);
  {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    napi_value object = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);
    ASSERT_EQ(napi_add_finalizer(s.env, object, nullptr, RecordFinalizerRealm, &record, nullptr),
              napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
            napi_ok);
  Leave(s.env, scope);

  EXPECT_TRUE(record.ran);
  EXPECT_TRUE(record.saw_env_global);
  ASSERT_EQ(napi_delete_reference(s.env, record.env_global), napi_ok);
}
