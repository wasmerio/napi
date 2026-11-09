#ifdef NAPI_MODULE
#undef NAPI_MODULE
#endif
#include "node_api.h"

#include "test_env.h"

#include "unofficial_napi.h"

#include <string>
#include <vector>

// Realm entry under pressure: script errors, handle scopes, nesting, classes,
// realms only a function or a deferred keeps alive, termination, env release
// with scopes open, and recursion.
class Test70RealmEntryAttack : public FixtureTestBase {};

namespace {

napi_value Str(napi_env env, const char* value) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out) != napi_ok) return nullptr;
  return out;
}

// Runs `source` with napi_run_script, in the current realm.
napi_status EvalStatus(napi_env env, const char* source, napi_value* result) {
  return napi_run_script(env, Str(env, source), result);
}

napi_value Eval(napi_env env, const char* source) {
  napi_value result = nullptr;
  if (EvalStatus(env, source, &result) != napi_ok) return nullptr;
  return result;
}

// Runs `source` in `realm` (the env's own realm when null).
napi_status RunInStatus(napi_env env,
                        unofficial_napi_realm realm,
                        const char* source,
                        napi_value* result) {
  const unofficial_napi_js_source script = unofficial_napi_js_source_from_text(Str(env, source));
  return unofficial_napi_run_script(env, realm, &script, Str(env, "realm.js"), 0, 0, -1, true,
                                    false, false, nullptr, result);
}

napi_value RunIn(napi_env env, unofficial_napi_realm realm, const char* source) {
  napi_value result = nullptr;
  if (RunInStatus(env, realm, source, &result) != napi_ok) return nullptr;
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
  if (unofficial_napi_create_realm(env, &options, UNOFFICIAL_NAPI_REALM_MODE_FRESH, 0, nullptr, &realm, global_out, nullptr) != napi_ok) return nullptr;
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

napi_value RefValue(napi_env env, napi_ref ref) {
  napi_value value = nullptr;
  napi_get_reference_value(env, ref, &value);
  return value;
}

// Checks in its own handle scope, so the check does not keep the value alive.
bool IsCollected(napi_env env, napi_ref ref) {
  napi_handle_scope handles = nullptr;
  if (napi_open_handle_scope(env, &handles) != napi_ok) return false;
  napi_value value = nullptr;
  bool collected = napi_get_reference_value(env, ref, &value) == napi_ok && value == nullptr;
  napi_close_handle_scope(env, handles);
  return collected;
}

// Whether `value`'s prototype is `global[ctor].prototype`. Runs no script, so
// it works in any realm and inside callbacks.
bool ProtoIs(napi_env env, napi_value value, napi_value global, const char* ctor) {
  napi_value proto = nullptr, constructor = nullptr, ctor_proto = nullptr;
  if (value == nullptr || global == nullptr) return false;
  if (napi_get_prototype(env, value, &proto) != napi_ok) return false;
  if (napi_get_named_property(env, global, ctor, &constructor) != napi_ok) return false;
  if (napi_get_named_property(env, constructor, "prototype", &ctor_proto) != napi_ok) return false;
  return StrictEquals(env, proto, ctor_proto);
}

// Takes the pending exception; null when there is none.
napi_value ClearException(napi_env env) {
  napi_value error = nullptr;
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) != napi_ok || !pending) return nullptr;
  if (napi_get_and_clear_last_exception(env, &error) != napi_ok) return nullptr;
  return error;
}

napi_value ReturnUndefined(napi_env env, napi_callback_info) {
  return Undefined(env);
}

napi_value ReturnGlobal(napi_env env, napi_callback_info) {
  return Global(env);
}

void* CallbackData(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  return data;
}

}  // namespace

// A script error thrown in an entered realm is still pending after leaving
// it, and is made by the realm whose code threw.
TEST_F(Test70RealmEntryAttack, JsThrowInEnteredRealmIsPendingAfterLeave) {
  EnvScope s(runtime_.get());
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  unofficial_napi_realm_scope scope = Enter(s.env, a);
  napi_value result = nullptr;
  EXPECT_EQ(EvalStatus(s.env, "throw new TypeError('t')", &result), napi_pending_exception);
  Leave(s.env, scope);
  napi_value error = ClearException(s.env);
  ASSERT_NE(error, nullptr);
  EXPECT_TRUE(ProtoIs(s.env, error, a_global, "TypeError"));

  // An error the engine makes.
  scope = Enter(s.env, a);
  EXPECT_EQ(EvalStatus(s.env, "null.x", &result), napi_pending_exception);
  Leave(s.env, scope);
  error = ClearException(s.env);
  ASSERT_NE(error, nullptr);
  EXPECT_TRUE(ProtoIs(s.env, error, a_global, "TypeError"));

  // Script run in B while A is entered throws B's error, and A stays current.
  scope = Enter(s.env, a);
  EXPECT_EQ(RunInStatus(s.env, b, "undefinedName", &result), napi_pending_exception);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), a_global));
  napi_value b_error = ClearException(s.env);
  Leave(s.env, scope);
  ASSERT_NE(b_error, nullptr);
  EXPECT_TRUE(ProtoIs(s.env, b_error, b_global, "ReferenceError"));

  // A function of A called from the env's realm throws A's error.
  napi_value thrower = RunIn(s.env, a, "(function () { return null.y; })");
  ASSERT_NE(thrower, nullptr);
  EXPECT_EQ(napi_call_function(s.env, Undefined(s.env), thrower, 0, nullptr, &result),
            napi_pending_exception);
  error = ClearException(s.env);
  ASSERT_NE(error, nullptr);
  EXPECT_TRUE(ProtoIs(s.env, error, a_global, "TypeError"));
}

// Handle scopes and realm scopes can be opened in either order, and values
// escape from inside a realm.
TEST_F(Test70RealmEntryAttack, HandleScopesInterleaveWithRealmScopes) {
  EnvScope s(runtime_.get());
  napi_value env_global = Global(s.env);
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  // Handle scope outside, realm scope inside.
  napi_handle_scope handles = nullptr;
  ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
  unofficial_napi_realm_scope scope = Enter(s.env, a);
  napi_value object = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);
  Leave(s.env, scope);
  EXPECT_TRUE(ProtoIs(s.env, object, a_global, "Object"));
  ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);

  // A value escapes from two realms deep.
  napi_value escaped = nullptr;
  {
    napi_escapable_handle_scope esc = nullptr;
    ASSERT_EQ(napi_open_escapable_handle_scope(s.env, &esc), napi_ok);
    unofficial_napi_realm_scope in_a = Enter(s.env, a);
    unofficial_napi_realm_scope in_b = Enter(s.env, b);
    napi_value array = nullptr;
    ASSERT_EQ(napi_create_array(s.env, &array), napi_ok);
    ASSERT_EQ(napi_escape_handle(s.env, esc, array, &escaped), napi_ok);
    Leave(s.env, in_b);
    Leave(s.env, in_a);
    ASSERT_EQ(napi_close_escapable_handle_scope(s.env, esc), napi_ok);
  }
  ASSERT_NE(escaped, nullptr);
  EXPECT_TRUE(ProtoIs(s.env, escaped, b_global, "Array"));

  // Realm scope outside, handle scope opened inside and closed after leaving.
  scope = Enter(s.env, a);
  ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
  napi_value in_a_object = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &in_a_object), napi_ok);
  Leave(s.env, scope);
  napi_value env_object = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &env_object), napi_ok);
  EXPECT_TRUE(ProtoIs(s.env, in_a_object, a_global, "Object"));
  EXPECT_TRUE(ProtoIs(s.env, env_object, env_global, "Object"));
  ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);

  // An escapable scope opened inside the realm scope, escaped after leaving.
  napi_value escaped_date = nullptr;
  {
    napi_escapable_handle_scope esc = nullptr;
    scope = Enter(s.env, a);
    ASSERT_EQ(napi_open_escapable_handle_scope(s.env, &esc), napi_ok);
    napi_value date = nullptr;
    ASSERT_EQ(napi_create_date(s.env, 1, &date), napi_ok);
    Leave(s.env, scope);
    ASSERT_EQ(napi_escape_handle(s.env, esc, date, &escaped_date), napi_ok);
    ASSERT_EQ(napi_close_escapable_handle_scope(s.env, esc), napi_ok);
  }
  EXPECT_TRUE(ProtoIs(s.env, escaped_date, a_global, "Date"));
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));
}

namespace {

struct ThreeRealms {
  napi_ref a_global = nullptr, b_global = nullptr, c_global = nullptr;
  unofficial_napi_realm b = nullptr;
  napi_ref js_b = nullptr;
  std::vector<std::string> log;
};

std::string WhichGlobal(napi_env env, ThreeRealms* t) {
  napi_value global = Global(env);
  if (StrictEquals(env, global, RefValue(env, t->a_global))) return "A";
  if (StrictEquals(env, global, RefValue(env, t->b_global))) return "B";
  if (StrictEquals(env, global, RefValue(env, t->c_global))) return "C";
  return "env";
}

napi_value ThrowInC(napi_env env, napi_callback_info info) {
  auto* t = static_cast<ThreeRealms*>(CallbackData(env, info));
  t->log.push_back("C:" + WhichGlobal(env, t));
  napi_throw_error(env, "ECODE", "thrown in C");
  return nullptr;
}

// Enters B, calls B's function (which calls C's ThrowInC), leaves B and
// rethrows C's error.
napi_value CallFromA(napi_env env, napi_callback_info info) {
  auto* t = static_cast<ThreeRealms*>(CallbackData(env, info));
  t->log.push_back("A:" + WhichGlobal(env, t));
  unofficial_napi_realm_scope scope = 0;
  if (unofficial_napi_enter_realm(env, t->b, &scope) != napi_ok) return nullptr;
  t->log.push_back("A-entered:" + WhichGlobal(env, t));
  napi_value result = nullptr;
  napi_status status =
      napi_call_function(env, Undefined(env), RefValue(env, t->js_b), 0, nullptr, &result);
  t->log.push_back("A-after-call:" + WhichGlobal(env, t) +
                   (status == napi_pending_exception ? ":pending"
                                                    : ":status" + std::to_string(status)));
  napi_value error = ClearException(env);
  t->log.push_back(std::string("A-error-from-C:") +
                   (ProtoIs(env, error, RefValue(env, t->c_global), "Error") ? "yes" : "no"));
  if (unofficial_napi_leave_realm(env, scope) != napi_ok) t->log.push_back("A-leave-failed");
  t->log.push_back("A-left:" + WhichGlobal(env, t));
  napi_throw(env, error);
  return nullptr;
}

}  // namespace

// A's callback enters B and calls B's script, which calls C's native function
// that throws. Each frame sees its own realm, and the error is C's.
TEST_F(Test70RealmEntryAttack, NestedCallbacksAcrossThreeRealms) {
  EnvScope s(runtime_.get());
  napi_value env_global = Global(s.env);
  napi_value a_global = nullptr, b_global = nullptr, c_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  unofficial_napi_realm c = CreateRealm(s.env, &c_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);
  ThreeRealms t;
  t.b = b;
  ASSERT_EQ(napi_create_reference(s.env, a_global, 1, &t.a_global), napi_ok);
  ASSERT_EQ(napi_create_reference(s.env, b_global, 1, &t.b_global), napi_ok);
  ASSERT_EQ(napi_create_reference(s.env, c_global, 1, &t.c_global), napi_ok);

  napi_value throw_in_c = nullptr, call_from_a = nullptr;
  unofficial_napi_realm_scope scope = Enter(s.env, c);
  ASSERT_EQ(napi_create_function(s.env, "throwInC", NAPI_AUTO_LENGTH, ThrowInC, &t, &throw_in_c),
            napi_ok);
  Leave(s.env, scope);
  scope = Enter(s.env, a);
  ASSERT_EQ(napi_create_function(s.env, "callFromA", NAPI_AUTO_LENGTH, CallFromA, &t, &call_from_a),
            napi_ok);
  Leave(s.env, scope);
  ASSERT_EQ(napi_set_named_property(s.env, b_global, "throwInC", throw_in_c), napi_ok);
  napi_value js_b = RunIn(s.env, b, "(function jsB() { return throwInC(); })");
  ASSERT_NE(js_b, nullptr);
  ASSERT_EQ(napi_create_reference(s.env, js_b, 1, &t.js_b), napi_ok);

  // Called from the env's realm.
  napi_value result = nullptr;
  EXPECT_EQ(napi_call_function(s.env, Undefined(s.env), call_from_a, 0, nullptr, &result),
            napi_pending_exception);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));
  napi_value error = ClearException(s.env);
  EXPECT_TRUE(ProtoIs(s.env, error, c_global, "Error"));
  std::string joined;
  for (auto& line : t.log) joined += line + ";";
  EXPECT_EQ(joined,
            "A:A;A-entered:B;C:C;A-after-call:B:pending;A-error-from-C:yes;A-left:A;");

  // Called from the env's script, which catches the error.
  t.log.clear();
  ASSERT_EQ(napi_set_named_property(s.env, env_global, "callFromA", call_from_a), napi_ok);
  napi_value caught = Eval(s.env,
                           "(() => { try { callFromA(); return 'no throw'; }"
                           " catch (e) { return e.message + '|' + e.code; } })()");
  EXPECT_EQ(Utf8(s.env, caught), "thrown in C|ECODE");
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));

  for (napi_ref ref : {t.a_global, t.b_global, t.c_global, t.js_b})
    ASSERT_EQ(napi_delete_reference(s.env, ref), napi_ok);
}

namespace {

struct ClassRecord {
  napi_ref a_global = nullptr;
  bool ctor_ran_in_a = false;
  int native = 1234;
};

void NoopFinalize(node_api_basic_env, void*, void*) {}

napi_value WrappingCtor(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  napi_value self = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, &self, &data);
  auto* record = static_cast<ClassRecord*>(data);
  record->ctor_ran_in_a = StrictEquals(env, Global(env), RefValue(env, record->a_global));
  if (napi_wrap(env, self, &record->native, NoopFinalize, nullptr, nullptr) != napi_ok) {
    return nullptr;
  }
  return self;
}

napi_value UnwrapMethod(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, &self, nullptr);
  void* native = nullptr;
  if (napi_unwrap(env, self, &native) != napi_ok) return nullptr;
  napi_value out = nullptr;
  napi_create_int32(env, *static_cast<int*>(native), &out);
  return out;
}

}  // namespace

// A class of A constructed while B is current: the constructor runs in A,
// wrap/unwrap and instanceof work across realms, and so does a subclass made
// in B.
TEST_F(Test70RealmEntryAttack, ClassOfAConstructedWhileBIsCurrent) {
  EnvScope s(runtime_.get());
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ClassRecord record;
  ASSERT_EQ(napi_create_reference(s.env, a_global, 1, &record.a_global), napi_ok);

  const napi_property_descriptor properties[] = {
      {"unwrap", nullptr, UnwrapMethod, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_value thing = nullptr;
  unofficial_napi_realm_scope scope = Enter(s.env, a);
  ASSERT_EQ(napi_define_class(s.env, "Thing", NAPI_AUTO_LENGTH, WrappingCtor, &record, 1,
                              properties, &thing),
            napi_ok);
  Leave(s.env, scope);
  napi_value thing_proto = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, thing, "prototype", &thing_proto), napi_ok);

  scope = Enter(s.env, b);
  napi_value instance = nullptr;
  ASSERT_EQ(napi_new_instance(s.env, thing, 0, nullptr, &instance), napi_ok);
  EXPECT_TRUE(record.ctor_ran_in_a);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), b_global));
  napi_value proto = nullptr;
  ASSERT_EQ(napi_get_prototype(s.env, instance, &proto), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, proto, thing_proto));
  void* native = nullptr;
  ASSERT_EQ(napi_unwrap(s.env, instance, &native), napi_ok);
  EXPECT_EQ(native, &record.native);
  bool is_instance = false;
  ASSERT_EQ(napi_instanceof(s.env, instance, thing, &is_instance), napi_ok);
  EXPECT_TRUE(is_instance);
  napi_value b_object_ctor = nullptr, a_object_ctor = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, b_global, "Object", &b_object_ctor), napi_ok);
  ASSERT_EQ(napi_get_named_property(s.env, a_global, "Object", &a_object_ctor), napi_ok);
  ASSERT_EQ(napi_instanceof(s.env, instance, b_object_ctor, &is_instance), napi_ok);
  EXPECT_FALSE(is_instance);
  ASSERT_EQ(napi_instanceof(s.env, instance, a_object_ctor, &is_instance), napi_ok);
  EXPECT_TRUE(is_instance);
  Leave(s.env, scope);

  // A subclass made in B: the instance gets the subclass's prototype, the
  // native constructor still runs in A, and the inherited method unwraps.
  ASSERT_EQ(napi_set_named_property(s.env, b_global, "Thing", thing), napi_ok);
  record.ctor_ran_in_a = false;
  napi_value out = RunIn(s.env, b,
                         "class Sub extends Thing { constructor() { super(); this.sub = 1; } }"
                         "const x = new Sub();"
                         "[Object.getPrototypeOf(x) === Sub.prototype, x instanceof Thing,"
                         " x.sub === 1, x.unwrap()].join()");
  EXPECT_EQ(Utf8(s.env, out), "true,true,true,1234");
  EXPECT_TRUE(record.ctor_ran_in_a);
  ASSERT_EQ(napi_delete_reference(s.env, record.a_global), napi_ok);
}

namespace {

struct Rediscover {
  bool got_realm = false;
  bool ran_in_it = false;
  bool new_function_in_it = false;
};

// Finds the callback's realm from its global, runs script in it and makes a
// function with it entered.
napi_value RediscoverRealm(napi_env env, napi_callback_info info) {
  auto* r = static_cast<Rediscover*>(CallbackData(env, info));
  napi_value global = Global(env);
  unofficial_napi_realm realm = nullptr;
  if (unofficial_napi_get_realm(env, global, &realm) != napi_ok || realm == nullptr) return nullptr;
  r->got_realm = true;
  r->ran_in_it = StrictEquals(env, RunIn(env, realm, "globalThis"), global);
  unofficial_napi_realm_scope scope = 0;
  if (unofficial_napi_enter_realm(env, realm, &scope) != napi_ok) return nullptr;
  napi_value fn = nullptr;
  napi_create_function(env, "inner", NAPI_AUTO_LENGTH, ReturnUndefined, nullptr, &fn);
  r->new_function_in_it = ProtoIs(env, fn, global, "Function");
  unofficial_napi_leave_realm(env, scope);
  return global;
}

}  // namespace

// A function whose realm is reachable only through it finds that realm again
// with get_realm, after collections and new realms, and can use it.
TEST_F(Test70RealmEntryAttack, FunctionFindsItsRealmAfterItsGlobalIsUnreachable) {
  EnvScope s(runtime_.get());
  Rediscover r;
  napi_ref fn_ref = nullptr;
  napi_ref weak_global = nullptr;
  {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    napi_value global = nullptr;
    unofficial_napi_realm realm = CreateRealm(s.env, &global);
    ASSERT_NE(realm, nullptr);
    unofficial_napi_realm_scope scope = Enter(s.env, realm);
    napi_value fn = nullptr;
    ASSERT_EQ(napi_create_function(s.env, "rediscover", NAPI_AUTO_LENGTH, RediscoverRealm, &r, &fn),
              napi_ok);
    Leave(s.env, scope);
    ASSERT_EQ(napi_create_reference(s.env, fn, 1, &fn_ref), napi_ok);
    ASSERT_EQ(napi_create_reference(s.env, global, 0, &weak_global), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  // New realms release what collected realms left behind.
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  for (int i = 0; i < 4; ++i) {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    ASSERT_NE(CreateRealm(s.env), nullptr);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);

  {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    napi_value fn = RefValue(s.env, fn_ref);
    ASSERT_NE(fn, nullptr);
    napi_value result = nullptr;
    ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), fn, 0, nullptr, &result), napi_ok);
    EXPECT_TRUE(r.got_realm);
    EXPECT_TRUE(r.ran_in_it);
    EXPECT_TRUE(r.new_function_in_it);
    EXPECT_TRUE(StrictEquals(s.env, result, RefValue(s.env, weak_global)));
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  ASSERT_EQ(napi_delete_reference(s.env, fn_ref), napi_ok);
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_global));
  ASSERT_EQ(napi_delete_reference(s.env, weak_global), napi_ok);
}

namespace {

struct Settled {
  int calls = 0;
  int32_t value = 0;
  bool in_env_realm = false;
  napi_ref env_global = nullptr;
};

napi_value RecordSettled(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {};
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  auto* r = static_cast<Settled*>(data);
  r->calls++;
  napi_get_value_int32(env, argv[0], &r->value);
  r->in_env_realm = StrictEquals(env, Global(env), RefValue(env, r->env_global));
  return Undefined(env);
}

}  // namespace

// A deferred of A, whose global and promise nothing else holds, is resolved
// later with B's thenable while B is entered; A is collected once settled.
TEST_F(Test70RealmEntryAttack, DeferredOfAnUnreachableRealmSettlesWithAThenable) {
  EnvScope s(runtime_.get());
  Settled settled;
  ASSERT_EQ(napi_create_reference(s.env, Global(s.env), 1, &settled.env_global), napi_ok);
  napi_value recorder = nullptr;
  ASSERT_EQ(napi_create_function(s.env, "recorder", NAPI_AUTO_LENGTH, RecordSettled, &settled,
                                 &recorder),
            napi_ok);
  napi_ref recorder_ref = nullptr;
  ASSERT_EQ(napi_create_reference(s.env, recorder, 1, &recorder_ref), napi_ok);

  napi_value b_global = nullptr;
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(b, nullptr);

  napi_deferred deferred = nullptr;
  napi_ref weak_global = nullptr;
  {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    napi_value a_global = nullptr;
    unofficial_napi_realm a = CreateRealm(s.env, &a_global);
    ASSERT_NE(a, nullptr);
    unofficial_napi_realm_scope scope = Enter(s.env, a);
    napi_value promise = nullptr;
    ASSERT_EQ(napi_create_promise(s.env, &deferred, &promise), napi_ok);
    napi_value then = nullptr, ignored = nullptr;
    ASSERT_EQ(napi_get_named_property(s.env, promise, "then", &then), napi_ok);
    napi_value reaction = RefValue(s.env, recorder_ref);
    ASSERT_EQ(napi_call_function(s.env, promise, then, 1, &reaction, &ignored), napi_ok);
    Leave(s.env, scope);
    ASSERT_EQ(napi_create_reference(s.env, a_global, 0, &weak_global), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  for (int i = 0; i < 4; ++i) {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    ASSERT_NE(CreateRealm(s.env), nullptr);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);

  napi_handle_scope resolve_handles = nullptr;
  ASSERT_EQ(napi_open_handle_scope(s.env, &resolve_handles), napi_ok);
  unofficial_napi_realm_scope scope = Enter(s.env, b);
  napi_value thenable = RunIn(s.env, b, "({ then(resolve) { resolve(42); } })");
  ASSERT_NE(thenable, nullptr);
  ASSERT_EQ(napi_resolve_deferred(s.env, deferred, thenable), napi_ok);
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
            napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), b_global));
  Leave(s.env, scope);
  EXPECT_EQ(settled.calls, 1);
  EXPECT_EQ(settled.value, 42);
  EXPECT_TRUE(settled.in_env_realm);
  ASSERT_EQ(napi_close_handle_scope(s.env, resolve_handles), napi_ok);

  ASSERT_EQ(napi_delete_reference(s.env, recorder_ref), napi_ok);
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
            napi_ok);
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_global));
  ASSERT_EQ(napi_delete_reference(s.env, weak_global), napi_ok);
  ASSERT_EQ(napi_delete_reference(s.env, settled.env_global), napi_ok);
}

// compile_function compiles in the realm it is given, not the current one: a
// NULL realm is the env's even while B is entered.
TEST_F(Test70RealmEntryAttack, CompileFunctionTakesItsRealmExplicitly) {
  EnvScope s(runtime_.get());
  napi_value env_global = Global(s.env);
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  unofficial_napi_realm_scope scope = Enter(s.env, b);
  // Compiles `return globalThis;` in `realm` and returns what calling it gives.
  auto compile = [&](unofficial_napi_realm realm) {
    const unofficial_napi_js_source source =
        unofficial_napi_js_source_from_text(Str(s.env, "return globalThis;"));
    napi_value fn = nullptr;
    EXPECT_EQ(unofficial_napi_compile_function(s.env, realm, &source, Str(s.env, "f.js"), 0, 0,
                                               Undefined(s.env), Undefined(s.env), nullptr, &fn),
              napi_ok);
    napi_value out = nullptr;
    if (fn == nullptr) return out;
    // The result may be the function or a record holding it.
    napi_valuetype type = napi_undefined;
    napi_typeof(s.env, fn, &type);
    if (type != napi_function) napi_get_named_property(s.env, fn, "function", &fn);
    EXPECT_EQ(napi_call_function(s.env, Undefined(s.env), fn, 0, nullptr, &out), napi_ok);
    return out;
  };
  EXPECT_TRUE(StrictEquals(s.env, compile(nullptr), env_global));
  EXPECT_TRUE(StrictEquals(s.env, compile(a), a_global));
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), b_global));
  Leave(s.env, scope);
}

namespace {

struct LoopRecord {
  unofficial_napi_realm b = nullptr;
  int calls = 0;
};

// Enters B and returns without leaving it.
napi_value EnterBAndReturn(napi_env env, napi_callback_info info) {
  auto* r = static_cast<LoopRecord*>(CallbackData(env, info));
  r->calls++;
  unofficial_napi_realm_scope scope = 0;
  unofficial_napi_enter_realm(env, r->b, &scope);
  napi_value object = nullptr;
  napi_create_object(env, &object);
  return object;
}

unofficial_napi_heap_statistics Stats(napi_env env) {
  unofficial_napi_heap_statistics stats;
  unofficial_napi_heap_statistics_init(&stats);
  EXPECT_EQ(unofficial_napi_get_heap_statistics(env, &stats), napi_ok);
  return stats;
}

}  // namespace

// Many enter/leave cycles and calls of a callback that leaves its scope open
// keep no frames, handles or heap behind.
TEST_F(Test70RealmEntryAttack, ManyEntriesAndCallsDoNotGrow) {
  EnvScope s(runtime_.get());
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  LoopRecord record;
  record.b = b;
  napi_value fn = nullptr;
  unofficial_napi_realm_scope scope = Enter(s.env, a);
  ASSERT_EQ(napi_create_function(s.env, "enterB", NAPI_AUTO_LENGTH, EnterBAndReturn, &record, &fn),
            napi_ok);
  Leave(s.env, scope);
  ASSERT_EQ(napi_set_named_property(s.env, b_global, "enterB", fn), napi_ok);
  napi_value loop_in_b =
      RunIn(s.env, b, "(function (n) { for (let i = 0; i < n; i++) enterB(); })");
  ASSERT_NE(loop_in_b, nullptr);

  // n cycles from native code, then n calls from B's script.
  auto round = [&](int n) {
    for (int i = 0; i < n; ++i) {
      napi_handle_scope handles = nullptr;
      ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
      unofficial_napi_realm_scope in_a = Enter(s.env, a);
      napi_value object = nullptr;
      ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);
      unofficial_napi_realm_scope in_b = Enter(s.env, b);
      napi_value result = nullptr;
      ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), fn, 0, nullptr, &result), napi_ok);
      Leave(s.env, in_b);
      Leave(s.env, in_a);
      ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
    }
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    napi_value count = nullptr, result = nullptr;
    ASSERT_EQ(napi_create_int32(s.env, n, &count), napi_ok);
    ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), loop_in_b, 1, &count, &result), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  };

  round(2000);
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  const unofficial_napi_heap_statistics before = Stats(s.env);
  round(50000);
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  const unofficial_napi_heap_statistics after = Stats(s.env);
  EXPECT_EQ(record.calls, 2 * (2000 + 50000));
  // 256 KiB of noise; 100,000 leaked frames would be far more.
  EXPECT_LT(after.used_heap_size, before.used_heap_size + 256 * 1024);
  if (after.valid_fields & unofficial_napi_heap_stat_used_global_handles_size) {
    EXPECT_LE(after.used_global_handles_size, before.used_global_handles_size + 4096);
  }
}

namespace {

struct TerminateRecord {
  unofficial_napi_realm b = nullptr;
  napi_status enter = napi_generic_failure;
};

// Enters B, terminates, and returns with B still entered.
napi_value EnterBAndTerminate(napi_env env, napi_callback_info info) {
  auto* r = static_cast<TerminateRecord*>(CallbackData(env, info));
  unofficial_napi_realm_scope scope = 0;
  r->enter = unofficial_napi_enter_realm(env, r->b, &scope);
  unofficial_napi_terminate_execution(env);
  return nullptr;
}

}  // namespace

// Termination with a realm entered: leaving still works, the env's realm is
// current again, and entering works once termination is cancelled.
TEST_F(Test70RealmEntryAttack, TerminationWithARealmScopeOpen) {
  EnvScope s(runtime_.get());
  napi_value env_global = Global(s.env);
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  unofficial_napi_realm_scope scope = Enter(s.env, a);
  ASSERT_EQ(unofficial_napi_terminate_execution(s.env), napi_ok);
  napi_value result = nullptr;
  EXPECT_NE(EvalStatus(s.env, "1", &result), napi_ok);
  Leave(s.env, scope);
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  ClearException(s.env);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));
  EXPECT_TRUE(StrictEquals(s.env, Eval(s.env, "globalThis"), env_global));
  scope = Enter(s.env, a);
  EXPECT_TRUE(StrictEquals(s.env, Eval(s.env, "globalThis"), a_global));
  Leave(s.env, scope);

  // A callback of A, called from A's script, enters B and terminates.
  TerminateRecord record;
  record.b = b;
  napi_value fn = nullptr;
  scope = Enter(s.env, a);
  ASSERT_EQ(napi_create_function(s.env, "term", NAPI_AUTO_LENGTH, EnterBAndTerminate, &record,
                                 &fn),
            napi_ok);
  Leave(s.env, scope);
  ASSERT_EQ(napi_set_named_property(s.env, a_global, "term", fn), napi_ok);
  // V8 notices a termination at its next interrupt check, such as a call.
  EXPECT_NE(RunInStatus(s.env, a, "term(); (() => {})(); globalThis.after = 1;", &result),
            napi_ok);
  EXPECT_EQ(record.enter, napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  ClearException(s.env);
  EXPECT_EQ(Utf8(s.env, RunIn(s.env, a, "String(globalThis.after)")), "undefined");
  scope = Enter(s.env, b);
  EXPECT_TRUE(StrictEquals(s.env, Eval(s.env, "globalThis"), b_global));
  Leave(s.env, scope);
}

// Releasing the env, outside any callback, with realm scopes still open; then
// also a deferred, a pending exception and a handle scope.
TEST_F(Test70RealmEntryAttack, EnvReleasedWithScopesOpen) {
  for (int variant = 0; variant < 4; ++variant) {
    SCOPED_TRACE(variant);
    EnvScope s(runtime_.get());
    napi_value a_global = nullptr, b_global = nullptr;
    unofficial_napi_realm a = CreateRealm(s.env, &a_global);
    unofficial_napi_realm b = CreateRealm(s.env, &b_global);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    Enter(s.env, a);
    Enter(s.env, b);
    if (variant >= 1) Enter(s.env, nullptr);
    if (variant >= 2) {
      Enter(s.env, b);
      napi_deferred deferred = nullptr;
      napi_value promise = nullptr;
      ASSERT_EQ(napi_create_promise(s.env, &deferred, &promise), napi_ok);
      napi_throw_error(s.env, nullptr, "left pending");
    }
    if (variant >= 3) {
      napi_handle_scope handles = nullptr;
      ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
      napi_value object = nullptr;
      ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);
    }
    // ~EnvScope releases the env.
  }
}

namespace {

struct ReactionRecord {
  napi_ref b_global = nullptr;
  bool ran_in_b = false;
  int calls = 0;
};

napi_value ReactionInB(napi_env env, napi_callback_info info) {
  auto* r = static_cast<ReactionRecord*>(CallbackData(env, info));
  r->calls++;
  r->ran_in_b = StrictEquals(env, Global(env), RefValue(env, r->b_global));
  return Undefined(env);
}

int g_interrupt_calls = 0;
bool g_interrupt_in_env = false;
napi_ref g_env_global = nullptr;

void InterruptProbe(napi_env env, void*) {
  g_interrupt_calls++;
  g_interrupt_in_env = StrictEquals(env, Global(env), RefValue(env, g_env_global));
}

}  // namespace

// Microtasks drained while A is entered run native reactions in their own
// realm, interrupts run in the env's realm, and A is current afterwards.
TEST_F(Test70RealmEntryAttack, MicrotasksAndInterruptsWhileEntered) {
  EnvScope s(runtime_.get());
  napi_value env_global = Global(s.env);
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ReactionRecord record;
  ASSERT_EQ(napi_create_reference(s.env, b_global, 1, &record.b_global), napi_ok);
  napi_value reaction = nullptr;
  unofficial_napi_realm_scope scope = Enter(s.env, b);
  ASSERT_EQ(napi_create_function(s.env, "reaction", NAPI_AUTO_LENGTH, ReactionInB, &record,
                                 &reaction),
            napi_ok);
  Leave(s.env, scope);

  scope = Enter(s.env, a);
  napi_deferred deferred = nullptr;
  napi_value promise = nullptr;
  ASSERT_EQ(napi_create_promise(s.env, &deferred, &promise), napi_ok);
  napi_value then = nullptr, ignored = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, promise, "then", &then), napi_ok);
  ASSERT_EQ(napi_call_function(s.env, promise, then, 1, &reaction, &ignored), napi_ok);
  ASSERT_EQ(napi_resolve_deferred(s.env, deferred, Undefined(s.env)), napi_ok);
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
            napi_ok);
  EXPECT_EQ(record.calls, 1);
  EXPECT_TRUE(record.ran_in_b);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), a_global));

  ASSERT_EQ(napi_create_reference(s.env, env_global, 1, &g_env_global), napi_ok);
  g_interrupt_calls = 0;
  g_interrupt_in_env = false;
  ASSERT_EQ(unofficial_napi_request_interrupt(s.env, InterruptProbe, nullptr), napi_ok);
  napi_value result = nullptr;
  ASSERT_EQ(EvalStatus(s.env, "let n = 0; for (let i = 0; i < 2e6; i++) n += i; n", &result),
            napi_ok);
  EXPECT_EQ(g_interrupt_calls, 1);
  EXPECT_TRUE(g_interrupt_in_env);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), a_global));
  Leave(s.env, scope);
  ASSERT_EQ(napi_delete_reference(s.env, g_env_global), napi_ok);
  g_env_global = nullptr;
  ASSERT_EQ(napi_delete_reference(s.env, record.b_global), napi_ok);
}

// An open scope keeps its realm alive after every handle and reference to it
// is gone; the realm is collected once the scope is left.
TEST_F(Test70RealmEntryAttack, OpenScopeKeepsItsRealmAlive) {
  EnvScope s(runtime_.get());
  napi_value env_global = Global(s.env);
  napi_ref weak_global = nullptr;
  unofficial_napi_realm_scope scope = 0;
  {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    napi_value global = nullptr;
    unofficial_napi_realm realm = CreateRealm(s.env, &global);
    ASSERT_NE(realm, nullptr);
    ASSERT_NE(RunIn(s.env, realm, "globalThis.marker = 'alive'; 1"), nullptr);
    ASSERT_EQ(napi_create_reference(s.env, global, 0, &weak_global), napi_ok);
    scope = Enter(s.env, realm);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  for (int i = 0; i < 4; ++i) {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    unofficial_napi_realm_scope env_scope = Enter(s.env, nullptr);
    ASSERT_NE(CreateRealm(s.env), nullptr);
    Leave(s.env, env_scope);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_FALSE(IsCollected(s.env, weak_global));
  {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    EXPECT_EQ(Utf8(s.env, Eval(s.env, "globalThis.marker")), "alive");
    unofficial_napi_realm found = nullptr;
    ASSERT_EQ(unofficial_napi_get_realm(s.env, Global(s.env), &found), napi_ok);
    EXPECT_NE(found, nullptr);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  Leave(s.env, scope);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_global));
  ASSERT_EQ(napi_delete_reference(s.env, weak_global), napi_ok);
}

// More values made with a realm entered come from it: objects from coercion,
// property name arrays, DataView, typed arrays, an external ArrayBuffer, a
// Buffer and range errors, made or thrown.
TEST_F(Test70RealmEntryAttack, MoreValuesComeFromTheEnteredRealm) {
  EnvScope s(runtime_.get());
  napi_value a_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  ASSERT_NE(a, nullptr);
  napi_value env_object = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &env_object), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, env_object, "k", Str(s.env, "v")), napi_ok);

  unofficial_napi_realm_scope scope = Enter(s.env, a);
  // Names what failed or is not from A.
  std::vector<std::string> wrong;
  // Takes the value by address: the call that writes it is an argument too,
  // and arguments are evaluated in no fixed order.
  auto check = [&](const char* name, napi_status status, napi_value* value, const char* ctor) {
    if (status != napi_ok) {
      wrong.push_back(std::string(name) + ":status" + std::to_string(status));
      ClearException(s.env);
      return;
    }
    if (!ProtoIs(s.env, *value, a_global, ctor)) wrong.push_back(name);
  };
  napi_value v = nullptr, number = nullptr;
  ASSERT_EQ(napi_create_int32(s.env, 3, &number), napi_ok);
  check("coerce number", napi_coerce_to_object(s.env, number, &v), &v, "Number");
  v = nullptr;
  check("coerce string", napi_coerce_to_object(s.env, Str(s.env, "x"), &v), &v, "String");
  v = nullptr;
  check("property names", napi_get_property_names(s.env, env_object, &v), &v, "Array");
  v = nullptr;
  check("all property names",
        napi_get_all_property_names(s.env, env_object, napi_key_own_only, napi_key_enumerable,
                                    napi_key_keep_numbers, &v),
        &v, "Array");
  napi_value ab = nullptr;
  ASSERT_EQ(napi_create_arraybuffer(s.env, 16, nullptr, &ab), napi_ok);
  v = nullptr;
  check("dataview", napi_create_dataview(s.env, 8, ab, 0, &v), &v, "DataView");
  v = nullptr;
  check("bigint64array", napi_create_typedarray(s.env, napi_bigint64_array, 1, ab, 0, &v), &v,
        "BigInt64Array");
  v = nullptr;
  check("float64array", napi_create_typedarray(s.env, napi_float64_array, 1, ab, 0, &v), &v,
        "Float64Array");
  static char external_bytes[16];
  v = nullptr;
  napi_status status = napi_create_external_arraybuffer(s.env, external_bytes,
                                                        sizeof(external_bytes), nullptr, nullptr,
                                                        &v);
  // Providers may refuse external buffers.
  if (status != napi_no_external_buffers_allowed) {
    check("external arraybuffer", status, &v, "ArrayBuffer");
  }
  v = nullptr;
  check("range error", napi_create_range_error(s.env, nullptr, Str(s.env, "r"), &v), &v,
        "RangeError");
  // Whatever a Buffer's prototype chain, it ends in A's Uint8Array.
  void* data = nullptr;
  v = nullptr;
  status = napi_create_buffer(s.env, 4, &data, &v);
  if (status == napi_ok) {
    napi_value uint8_array = nullptr;
    bool is_instance = false;
    napi_get_named_property(s.env, a_global, "Uint8Array", &uint8_array);
    napi_instanceof(s.env, v, uint8_array, &is_instance);
    if (!is_instance) wrong.push_back("buffer");
  } else {
    wrong.push_back("buffer:status" + std::to_string(status));
  }
  napi_throw_range_error(s.env, nullptr, "thrown");
  napi_value thrown = ClearException(s.env);
  if (!ProtoIs(s.env, thrown, a_global, "RangeError")) wrong.push_back("thrown range error");
  Leave(s.env, scope);
  std::string joined;
  for (auto& name : wrong) joined += name + ",";
  EXPECT_EQ(joined, "");
}

namespace {

struct SandboxProbe {
  napi_ref sandbox = nullptr;
  bool global_is_sandbox = false;
  bool found_realm = false;
};

napi_value ProbeSandbox(napi_env env, napi_callback_info info) {
  auto* p = static_cast<SandboxProbe*>(CallbackData(env, info));
  napi_value global = Global(env);
  p->global_is_sandbox = StrictEquals(env, global, RefValue(env, p->sandbox));
  unofficial_napi_realm realm = nullptr;
  p->found_realm = unofficial_napi_get_realm(env, global, &realm) == napi_ok && realm != nullptr;
  return global;
}

}  // namespace

// A native function of a sandbox realm sees the sandbox as its global, and
// get_realm finds the realm from it.
TEST_F(Test70RealmEntryAttack, SandboxRealmCallbackSeesItsSandbox) {
  EnvScope s(runtime_.get());
  napi_value sandbox = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
  unofficial_napi_realm realm = CreateRealm(s.env, nullptr, sandbox);
  ASSERT_NE(realm, nullptr);
  SandboxProbe probe;
  ASSERT_EQ(napi_create_reference(s.env, sandbox, 1, &probe.sandbox), napi_ok);
  unofficial_napi_realm_scope scope = Enter(s.env, realm);
  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_function(s.env, "probe", NAPI_AUTO_LENGTH, ProbeSandbox, &probe, &fn),
            napi_ok);
  Leave(s.env, scope);
  napi_value result = nullptr;
  ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), fn, 0, nullptr, &result), napi_ok);
  EXPECT_TRUE(probe.global_is_sandbox);
  EXPECT_TRUE(probe.found_realm);
  ASSERT_EQ(napi_delete_reference(s.env, probe.sandbox), napi_ok);
}

namespace {

// QuickJS's 1 MiB stack limit fits about 11 levels in a Debug+ASan build.
constexpr int32_t kRecursionDepth = 8;

struct Recur {
  unofficial_napi_realm b = nullptr;
  napi_ref a_global = nullptr, b_global = nullptr, js_b = nullptr;
  int bad = 0;
  int32_t deepest = kRecursionDepth + 1;
};

// Enters B and calls B's script, which calls this again with depth - 1.
// Leaves every other level's scope open for the callback's return to close.
napi_value RecurseA(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {};
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  auto* r = static_cast<Recur*>(data);
  int32_t depth = 0;
  napi_get_value_int32(env, argv[0], &depth);
  if (depth < r->deepest) r->deepest = depth;
  if (!StrictEquals(env, Global(env), RefValue(env, r->a_global))) r->bad++;
  if (depth == 0) return Undefined(env);
  unofficial_napi_realm_scope scope = 0;
  unofficial_napi_enter_realm(env, r->b, &scope);
  if (!StrictEquals(env, Global(env), RefValue(env, r->b_global))) r->bad++;
  napi_value next = nullptr, out = nullptr;
  napi_create_int32(env, depth - 1, &next);
  napi_call_function(env, Undefined(env), RefValue(env, r->js_b), 1, &next, &out);
  if (!StrictEquals(env, Global(env), RefValue(env, r->b_global))) r->bad++;
  if (depth % 2 == 0) {
    if (unofficial_napi_leave_realm(env, scope) != napi_ok) r->bad++;
    if (!StrictEquals(env, Global(env), RefValue(env, r->a_global))) r->bad++;
  }
  return out;
}

}  // namespace

// Recursion through two realms: every level sees the right realm, and the
// env's realm is current at the end.
TEST_F(Test70RealmEntryAttack, RecursionThroughRealms) {
  EnvScope s(runtime_.get());
  napi_value env_global = Global(s.env);
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  Recur r;
  r.b = b;
  ASSERT_EQ(napi_create_reference(s.env, a_global, 1, &r.a_global), napi_ok);
  ASSERT_EQ(napi_create_reference(s.env, b_global, 1, &r.b_global), napi_ok);
  napi_value fn = nullptr;
  unofficial_napi_realm_scope scope = Enter(s.env, a);
  ASSERT_EQ(napi_create_function(s.env, "recurse", NAPI_AUTO_LENGTH, RecurseA, &r, &fn), napi_ok);
  Leave(s.env, scope);
  ASSERT_EQ(napi_set_named_property(s.env, b_global, "recurse", fn), napi_ok);
  napi_value js_b = RunIn(s.env, b, "(function (n) { return recurse(n); })");
  ASSERT_NE(js_b, nullptr);
  ASSERT_EQ(napi_create_reference(s.env, js_b, 1, &r.js_b), napi_ok);
  napi_value start = nullptr, out = nullptr;
  ASSERT_EQ(napi_create_int32(s.env, kRecursionDepth, &start), napi_ok);
  EXPECT_EQ(napi_call_function(s.env, Undefined(s.env), fn, 1, &start, &out), napi_ok);
  EXPECT_EQ(r.deepest, 0);
  EXPECT_EQ(r.bad, 0);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), env_global));
  for (napi_ref ref : {r.a_global, r.b_global, r.js_b})
    ASSERT_EQ(napi_delete_reference(s.env, ref), napi_ok);
}

// A realm of one env cannot be entered on another, and a scope token of one
// env cannot be left on another.
TEST_F(Test70RealmEntryAttack, RealmsAndTokensStayInTheirEnv) {
  EnvScope s1(runtime_.get());
  unofficial_napi_realm r1 = CreateRealm(s1.env);
  ASSERT_NE(r1, nullptr);
  unofficial_napi_realm_scope t1 = Enter(s1.env, r1);
  {
    EnvScope s2(runtime_.get());
    unofficial_napi_realm_scope scope = 0;
    EXPECT_EQ(unofficial_napi_enter_realm(s2.env, r1, &scope), napi_invalid_arg);
    EXPECT_EQ(unofficial_napi_leave_realm(s2.env, t1), napi_invalid_arg);
  }
  Leave(s1.env, t1);
}

namespace {

struct MakeInB {
  unofficial_napi_realm b = nullptr;
  napi_ref made = nullptr;
};

// Makes a function with B entered and returns it.
napi_value MakeFunctionInB(napi_env env, napi_callback_info info) {
  auto* m = static_cast<MakeInB*>(CallbackData(env, info));
  unofficial_napi_realm_scope scope = 0;
  unofficial_napi_enter_realm(env, m->b, &scope);
  napi_value fn = nullptr;
  napi_create_function(env, "inB", NAPI_AUTO_LENGTH, ReturnGlobal, nullptr, &fn);
  napi_create_reference(env, fn, 1, &m->made);
  unofficial_napi_leave_realm(env, scope);
  return fn;
}

}  // namespace

// A function made in A's callback with B entered is B's, and runs in B
// wherever it is called from.
TEST_F(Test70RealmEntryAttack, FunctionMadeInsideACallbackWithAnotherRealmEntered) {
  EnvScope s(runtime_.get());
  napi_value a_global = nullptr, b_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  unofficial_napi_realm b = CreateRealm(s.env, &b_global);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  MakeInB m;
  m.b = b;
  napi_value maker = nullptr;
  unofficial_napi_realm_scope scope = Enter(s.env, a);
  ASSERT_EQ(napi_create_function(s.env, "maker", NAPI_AUTO_LENGTH, MakeFunctionInB, &m, &maker),
            napi_ok);
  Leave(s.env, scope);
  napi_value made = nullptr;
  ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), maker, 0, nullptr, &made), napi_ok);
  EXPECT_TRUE(ProtoIs(s.env, made, b_global, "Function"));
  napi_value got = nullptr;
  ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), made, 0, nullptr, &got), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, got, b_global));
  ASSERT_EQ(napi_set_named_property(s.env, a_global, "inB", made), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, RunIn(s.env, a, "inB()"), b_global));
  ASSERT_EQ(napi_delete_reference(s.env, m.made), napi_ok);
}

namespace {

struct CreateInside {
  bool ok = false;
};

// Makes a realm and checks it has its own intrinsics and leaves the current
// realm alone.
napi_value CreateRealmInside(napi_env env, napi_callback_info info) {
  auto* c = static_cast<CreateInside*>(CallbackData(env, info));
  napi_value before = Global(env);
  napi_value global = nullptr;
  unofficial_napi_realm realm = CreateRealm(env, &global);
  napi_value own =
      RunIn(env, realm, "Object.getPrototypeOf({}) === Object.prototype && globalThis");
  c->ok = realm != nullptr && StrictEquals(env, own, global) &&
          StrictEquals(env, Global(env), before) && !StrictEquals(env, global, before);
  return global;
}

}  // namespace

// create_realm while another realm is entered, or inside one of its
// callbacks, makes a realm with its own intrinsics and leaves the current
// realm unchanged.
TEST_F(Test70RealmEntryAttack, CreateRealmWhileAnotherIsCurrent) {
  EnvScope s(runtime_.get());
  napi_value a_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  ASSERT_NE(a, nullptr);
  unofficial_napi_realm_scope scope = Enter(s.env, a);
  napi_value c_global = nullptr;
  unofficial_napi_realm c = CreateRealm(s.env, &c_global);
  ASSERT_NE(c, nullptr);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), a_global));
  Leave(s.env, scope);
  EXPECT_TRUE(StrictEquals(s.env, RunIn(s.env, c, "globalThis"), c_global));
  EXPECT_EQ(Utf8(s.env, RunIn(s.env, c,
                              "String(Object.getPrototypeOf(globalThis) !== null &&"
                              " Object.getPrototypeOf({}) === Object.prototype)")),
            "true");
  ASSERT_EQ(napi_set_named_property(s.env, c_global, "aGlobal", a_global), napi_ok);
  EXPECT_EQ(Utf8(s.env, RunIn(s.env, c, "String(aGlobal.Object !== Object)")), "true");

  CreateInside inside;
  napi_value fn = nullptr;
  scope = Enter(s.env, a);
  ASSERT_EQ(napi_create_function(s.env, "mk", NAPI_AUTO_LENGTH, CreateRealmInside, &inside, &fn),
            napi_ok);
  Leave(s.env, scope);
  napi_value result = nullptr;
  ASSERT_EQ(napi_call_function(s.env, Undefined(s.env), fn, 0, nullptr, &result), napi_ok);
  EXPECT_TRUE(inside.ok);
}

namespace {

struct CleanupRecord {
  napi_env env = nullptr;
  napi_ref env_global = nullptr;
  bool ran = false;
  bool in_env_realm = false;
};

CleanupRecord g_cleanup;

void CleanupHook(void* arg) {
  auto* c = static_cast<CleanupRecord*>(arg);
  c->ran = true;
  napi_handle_scope handles = nullptr;
  napi_open_handle_scope(c->env, &handles);
  c->in_env_realm = StrictEquals(c->env, Global(c->env), RefValue(c->env, c->env_global));
  napi_close_handle_scope(c->env, handles);
}

}  // namespace

// An env released with a realm entered runs its cleanup hooks in its own
// realm.
TEST_F(Test70RealmEntryAttack, CleanupHooksRunInTheEnvRealmWithScopesOpen) {
  g_cleanup = CleanupRecord{};
  {
    EnvScope s(runtime_.get());
    g_cleanup.env = s.env;
    ASSERT_EQ(napi_create_reference(s.env, Global(s.env), 1, &g_cleanup.env_global), napi_ok);
    ASSERT_EQ(napi_add_env_cleanup_hook(s.env, CleanupHook, &g_cleanup), napi_ok);
    unofficial_napi_realm a = CreateRealm(s.env);
    ASSERT_NE(a, nullptr);
    Enter(s.env, a);
  }
  EXPECT_TRUE(g_cleanup.ran);
  EXPECT_TRUE(g_cleanup.in_env_realm);
}

// Entering and leaving keep a pending exception pending.
TEST_F(Test70RealmEntryAttack, EnterAndLeaveWithAPendingException) {
  EnvScope s(runtime_.get());
  napi_value a_global = nullptr;
  unofficial_napi_realm a = CreateRealm(s.env, &a_global);
  ASSERT_NE(a, nullptr);
  napi_throw_error(s.env, nullptr, "pending");
  unofficial_napi_realm_scope scope = 0;
  ASSERT_EQ(unofficial_napi_enter_realm(s.env, a, &scope), napi_ok);
  EXPECT_TRUE(StrictEquals(s.env, Global(s.env), a_global));
  Leave(s.env, scope);
  EXPECT_NE(ClearException(s.env), nullptr);
}
