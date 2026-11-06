// unofficial_napi_define_lazy_data_properties and
// unofficial_napi_is_lazy_data_property: data properties whose value an
// initializer makes on their first read.
//
// An embedder that installs many properties a script may never read (a
// browser realm's interface objects) defines them lazily and builds each one
// when it is first read. On V8 this is Object::SetLazyDataProperty. V8
// CHECKs, fatally, that the property is still the lazy accessor when it
// replaces it, so the provider turns a read from the property's own
// initializer, and an initializer that changes the property, into a thrown
// error. QuickJS has no lazy data properties: both calls check their
// arguments and return napi_generic_failure.

#if defined(NAPI_TEST_ENGINE_V8)
#include <v8.h>
#endif

#include "test_env.h"

#include "unofficial_napi.h"

#include <cstdint>
#include <initializer_list>
#include <string>

class Test75UnofficialLazyDataProperties : public FixtureTestBase {};

namespace {

constexpr napi_property_attributes kAll =
    static_cast<napi_property_attributes>(napi_writable | napi_enumerable | napi_configurable);

napi_value RunScript(napi_env env, const char* source) {
  napi_value script = nullptr;
  EXPECT_EQ(napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &script), napi_ok);
  napi_value result = nullptr;
  EXPECT_EQ(napi_run_script(env, script, &result), napi_ok) << source;
  return result;
}

std::string RunScriptToString(napi_env env, const char* source) {
  napi_value result = RunScript(env, source);
  if (result == nullptr) return "<no result>";
  napi_value as_string = nullptr;
  EXPECT_EQ(napi_coerce_to_string(env, result, &as_string), napi_ok);
  char buffer[512] = {};
  size_t length = 0;
  EXPECT_EQ(napi_get_value_string_utf8(env, as_string, buffer, sizeof(buffer), &length), napi_ok);
  return std::string(buffer, length);
}

napi_value Global(napi_env env, const char* name) {
  napi_value global = nullptr;
  EXPECT_EQ(napi_get_global(env, &global), napi_ok);
  napi_value value = nullptr;
  EXPECT_EQ(napi_get_named_property(env, global, name, &value), napi_ok);
  return value;
}

napi_value String(napi_env env, const char* text) {
  napi_value value = nullptr;
  EXPECT_EQ(napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &value), napi_ok);
  return value;
}

// An array of the given strings.
napi_value Names(napi_env env, std::initializer_list<const char*> names) {
  napi_value array = nullptr;
  EXPECT_EQ(napi_create_array_with_length(env, names.size(), &array), napi_ok);
  uint32_t index = 0;
  for (const char* name : names) {
    EXPECT_EQ(napi_set_element(env, array, index++, String(env, name)), napi_ok);
  }
  return array;
}

}  // namespace

// Both engines check pointers and attribute bits before anything else.
TEST_F(Test75UnofficialLazyDataProperties, RejectsNullArgumentsAndUnknownAttributes) {
  EnvScope s(runtime_.get());
  napi_value object = RunScript(s.env, "({})");
  napi_value names = Names(s.env, {"a"});
  napi_value initializer = RunScript(s.env, "(function () { return 1; })");
  napi_value key = String(s.env, "a");
  bool result = true;

  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(nullptr, object, names, initializer, kAll),
            napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, nullptr, names, initializer, kAll),
            napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, object, nullptr, initializer, kAll),
            napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, object, names, nullptr, kAll),
            napi_invalid_arg);
  for (int attributes : {8, static_cast<int>(napi_static), static_cast<int>(napi_static | kAll)}) {
    SCOPED_TRACE(attributes);
    EXPECT_EQ(unofficial_napi_define_lazy_data_properties(
                  s.env, object, names, initializer,
                  static_cast<napi_property_attributes>(attributes)),
              napi_invalid_arg);
  }

  EXPECT_EQ(unofficial_napi_is_lazy_data_property(nullptr, object, key, &result),
            napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_is_lazy_data_property(s.env, nullptr, key, &result),
            napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_is_lazy_data_property(s.env, object, nullptr, &result),
            napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_is_lazy_data_property(s.env, object, key, nullptr), napi_invalid_arg);

  // Nothing was defined.
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "object", object), napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, "Reflect.ownKeys(object).length"), "0");
}

#if defined(NAPI_TEST_ENGINE_V8)

namespace {

void Expose(napi_env env, const char* name, napi_callback callback) {
  napi_value function = nullptr;
  ASSERT_EQ(napi_create_function(env, name, NAPI_AUTO_LENGTH, callback, nullptr, &function),
            napi_ok);
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(env, global, name, function), napi_ok);
}

// defineLazy(object, names, initializer[, attributes]) for scripts: the status
// times ten, plus one if an exception is pending (it then propagates).
napi_value DefineLazyFromJs(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  EXPECT_EQ(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr), napi_ok);
  int32_t attributes = kAll;
  if (argc > 3) EXPECT_EQ(napi_get_value_int32(env, argv[3], &attributes), napi_ok);
  const napi_status status = unofficial_napi_define_lazy_data_properties(
      env, argv[0], argv[1], argv[2], static_cast<napi_property_attributes>(attributes));
  bool pending = false;
  EXPECT_EQ(napi_is_exception_pending(env, &pending), napi_ok);
  napi_value result = nullptr;
  EXPECT_EQ(napi_create_int32(env, status * 10 + (pending ? 1 : 0), &result), napi_ok);
  return result;
}

}  // namespace

// Defining calls nothing and puts each name in the key order; the first read
// calls initializer.call(holder, name) once and V8 replaces the property in
// place with a data property that has the attributes asked for.
TEST_F(Test75UnofficialLazyDataProperties, V8FirstReadCallsTheInitializerOnce) {
  EnvScope s(runtime_.get());
  RunScript(s.env, R"(
    globalThis.holder = { before: 1 };
    globalThis.symbol = Symbol("s");
    globalThis.calls = [];
    globalThis.init = function (name) {
      calls.push([this === holder, typeof name, String(name), arguments.length].join());
      return { made: String(name) };
    };
  )");
  napi_value holder = Global(s.env, "holder");
  napi_value init = Global(s.env, "init");
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, holder, Names(s.env, {"all"}), init,
                                                        kAll),
            napi_ok);
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, holder, Names(s.env, {"hidden"}),
                                                        init, napi_default),
            napi_ok);
  napi_value symbols = nullptr;
  ASSERT_EQ(napi_create_array(s.env, &symbols), napi_ok);
  ASSERT_EQ(napi_set_element(s.env, symbols, 0, Global(s.env, "symbol")), napi_ok);
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, holder, symbols, init, kAll),
            napi_ok);
  RunScript(s.env, "holder.after = 2;");

  EXPECT_EQ(RunScriptToString(s.env, "calls.length"), "0");
  EXPECT_EQ(RunScriptToString(s.env, "JSON.stringify(Object.keys(holder))"),
            R"(["before","all","after"])");
  EXPECT_EQ(RunScriptToString(s.env,
                              "JSON.stringify(Reflect.ownKeys(holder).map(String))"),
            R"json(["before","all","hidden","after","Symbol(s)"])json");
  EXPECT_EQ(RunScriptToString(s.env,
                              "'all' in holder && Object.prototype.hasOwnProperty.call(holder, "
                              "'hidden') && symbol in holder"),
            "true");
  EXPECT_EQ(RunScriptToString(s.env, "calls.length"), "0");

  bool lazy = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, holder, String(s.env, "all"), &lazy),
            napi_ok);
  EXPECT_TRUE(lazy);
  EXPECT_EQ(RunScriptToString(s.env, "calls.length"), "0") << "the query called the initializer";

  // The first read, from C.
  napi_value value = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, holder, "all", &value), napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, "JSON.stringify(calls)"), R"(["true,string,all,1"])");
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, holder, String(s.env, "all"), &lazy),
            napi_ok);
  EXPECT_FALSE(lazy);
  // Later reads get the same value without a call.
  EXPECT_EQ(RunScriptToString(s.env, "holder.all === holder.all && holder.all.made"), "all");
  EXPECT_EQ(RunScriptToString(s.env, "calls.length"), "1");
  EXPECT_EQ(RunScriptToString(
                s.env,
                "const d = Object.getOwnPropertyDescriptor(holder, 'all');"
                "[d.value === holder.all, d.writable, d.enumerable, d.configurable].join()"),
            "true,true,true,true");

  // A descriptor read is a first read too.
  EXPECT_EQ(RunScriptToString(
                s.env,
                "const h = Object.getOwnPropertyDescriptor(holder, 'hidden');"
                "[h.value.made, h.writable, h.enumerable, h.configurable].join()"),
            "hidden,false,false,false");
  EXPECT_EQ(RunScriptToString(s.env, "holder[symbol].made"), "Symbol(s)");
  EXPECT_EQ(RunScriptToString(s.env, "calls.join(';')"),
            "true,string,all,1;true,string,hidden,1;true,symbol,Symbol(s),1");
  EXPECT_EQ(RunScriptToString(s.env,
                              "JSON.stringify(Reflect.ownKeys(holder).map(String))"),
            R"json(["before","all","hidden","after","Symbol(s)"])json");
}

// Assigning or deleting a lazy property before its first read replaces it
// without calling the initializer.
TEST_F(Test75UnofficialLazyDataProperties, V8AssignmentAndDeletionBeforeAReadCallNothing) {
  EnvScope s(runtime_.get());
  RunScript(s.env, R"(
    globalThis.holder = {};
    globalThis.count = 0;
    globalThis.init = function () { count++; return "made"; };
  )");
  napi_value holder = Global(s.env, "holder");
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, holder, Names(s.env, {"set", "gone"}),
                                                        Global(s.env, "init"), kAll),
            napi_ok);
  RunScript(s.env, "holder.set = 5; delete holder.gone;");
  EXPECT_EQ(RunScriptToString(s.env, "[count, holder.set, 'gone' in holder].join()"),
            "0,5,false");
  for (const char* name : {"set", "gone"}) {
    SCOPED_TRACE(name);
    bool lazy = true;
    ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, holder, String(s.env, name), &lazy),
              napi_ok);
    EXPECT_FALSE(lazy);
  }
  EXPECT_EQ(RunScriptToString(s.env, "count"), "0");
}

// A throwing initializer makes the read throw and leaves the property lazy;
// the next read calls it again.
TEST_F(Test75UnofficialLazyDataProperties, V8AThrowingInitializerLeavesThePropertyLazy) {
  EnvScope s(runtime_.get());
  RunScript(s.env, R"(
    globalThis.holder = {};
    globalThis.count = 0;
    globalThis.fail = true;
    globalThis.init = function () {
      count++;
      if (fail) throw new RangeError("refused");
      return 7;
    };
  )");
  napi_value holder = Global(s.env, "holder");
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, holder, Names(s.env, {"boom"}),
                                                        Global(s.env, "init"), kAll),
            napi_ok);

  napi_value value = nullptr;
  EXPECT_EQ(napi_get_named_property(s.env, holder, "boom", &value), napi_pending_exception);
  napi_value error = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &error), napi_ok);
  napi_value message = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, error, "message", &message), napi_ok);
  char buffer[64] = {};
  size_t length = 0;
  ASSERT_EQ(napi_get_value_string_utf8(s.env, message, buffer, sizeof(buffer), &length), napi_ok);
  EXPECT_EQ(std::string(buffer, length), "refused");

  bool lazy = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, holder, String(s.env, "boom"), &lazy),
            napi_ok);
  EXPECT_TRUE(lazy);
  EXPECT_EQ(RunScriptToString(
                s.env, "let e; try { holder.boom; } catch (error) { e = error; } [count, e.message]"),
            "2,refused");

  RunScript(s.env, "fail = false;");
  EXPECT_EQ(RunScriptToString(s.env, "[holder.boom, holder.boom, count].join()"), "7,7,3");
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, holder, String(s.env, "boom"), &lazy),
            napi_ok);
  EXPECT_FALSE(lazy);
}

// The cases where V8 would hit its CHECK: a read from the property's own
// initializer throws a RangeError and leaves it lazy (an initializer that
// catches it still makes the value); an initializer that deletes or assigns
// its property makes the read throw a TypeError, the property keeping what
// the initializer did.
TEST_F(Test75UnofficialLazyDataProperties, V8InitializersThatTouchTheirOwnPropertyThrow) {
  EnvScope s(runtime_.get());
  RunScript(s.env, R"(
    globalThis.holder = {};
    globalThis.inner = "none";
    globalThis.init = function (name) {
      if (name === "loop") return this.loop;
      if (name === "again") {
        try { this.again; } catch (e) { inner = e.constructor.name; }
        return 3;
      }
      if (name === "gone") { delete this.gone; return 1; }
      if (name === "assigned") { this.assigned = "assigned"; return 1; }
      return 0;
    };
    globalThis.thrown = (name) => {
      try { holder[name]; return "no error"; } catch (e) { return e.constructor.name; }
    };
  )");
  napi_value holder = Global(s.env, "holder");
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(
                s.env, holder, Names(s.env, {"loop", "again", "gone", "assigned"}), Global(s.env, "init"),
                kAll),
            napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, "[thrown('loop'), thrown('gone'), thrown('assigned')].join()"),
            "RangeError,TypeError,TypeError");
  bool lazy = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, holder, String(s.env, "loop"), &lazy),
            napi_ok);
  EXPECT_TRUE(lazy);
  EXPECT_EQ(RunScriptToString(s.env, "[holder.again, inner].join()"), "3,RangeError");
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, holder, String(s.env, "again"), &lazy),
            napi_ok);
  EXPECT_FALSE(lazy);
  EXPECT_EQ(RunScriptToString(s.env, "['gone' in holder, holder.assigned].join()"),
            "false,assigned");
}

// An initializer that defines its own property lazily again, with another
// initializer or with itself, makes the read throw a TypeError: the new
// definition stays, and the next read calls its initializer.
TEST_F(Test75UnofficialLazyDataProperties, V8AnInitializerThatRedefinesItsPropertyLazilyThrows) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  EXPECT_EQ(RunScriptToString(s.env, R"(
    const thrown = (object, name) => {
      try { object[name]; return "no error"; } catch (e) { return e.constructor.name; }
    };
    const other = {};
    defineLazy(other, ["x"], function () {
      defineLazy(this, ["x"], function () { return "second"; });
      return "first";
    });
    let redefined = false;
    const same = {};
    const init = function (name) {
      if (redefined) return "second";
      redefined = true;
      defineLazy(this, [name], init);
      return "first";
    };
    defineLazy(same, ["x"], init);
    [thrown(other, "x"), other.x, thrown(same, "x"), same.x].join()
  )"),
            "TypeError,second,TypeError,second");
}

// The query reads no getter and no proxy trap, and is false for everything
// that is not a lazy property.
TEST_F(Test75UnofficialLazyDataProperties, V8TheQueryCallsNothing) {
  EnvScope s(runtime_.get());
  RunScript(s.env, R"(
    globalThis.calls = 0;
    globalThis.accessor = { data: 1, get getter() { calls++; return 1; } };
    globalThis.proxy = new Proxy({}, new Proxy({}, { get() { calls++; return undefined; } }));
    globalThis.array = [1, 2];
  )");
  struct Case {
    const char* object;
    const char* key;
  } cases[] = {{"accessor", "data"}, {"accessor", "getter"}, {"accessor", "missing"},
               {"proxy", "x"},       {"array", "length"}};
  for (const Case& c : cases) {
    SCOPED_TRACE(std::string(c.object) + "." + c.key);
    bool lazy = true;
    ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, Global(s.env, c.object),
                                                    String(s.env, c.key), &lazy),
              napi_ok);
    EXPECT_FALSE(lazy);
  }
  EXPECT_EQ(RunScriptToString(s.env, "calls"), "0");
}

// An embedder's native accessor property: the query calls its getter, and a
// lazy property that getter reads is neither built nor taken for the one
// asked about.
namespace {

int g_native_getter_calls = 0;

void NativeGetterThatReadsALazyProperty(v8::Local<v8::Name>,
                                        const v8::PropertyCallbackInfo<v8::Value>& info) {
  ++g_native_getter_calls;
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Value> holder;
  v8::Local<v8::Value> value;
  if (!context->Global()
           ->Get(context, v8::String::NewFromUtf8Literal(isolate, "lazyHolder"))
           .ToLocal(&holder) ||
      !holder.As<v8::Object>()
           ->Get(context, v8::String::NewFromUtf8Literal(isolate, "lazy"))
           .ToLocal(&value)) {
    return;
  }
  info.GetReturnValue().Set(value);
}

napi_value InstallNativeProperty(napi_env, napi_callback_info) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  EXPECT_TRUE(context->Global()
                  ->SetNativeDataProperty(context,
                                          v8::String::NewFromUtf8Literal(isolate, "native"),
                                          NativeGetterThatReadsALazyProperty)
                  .FromMaybe(false));
  return nullptr;
}

}  // namespace

TEST_F(Test75UnofficialLazyDataProperties, V8TheQueryLooksOnlyForItsOwnProperty) {
  EnvScope s(runtime_.get());
  g_native_getter_calls = 0;
  Expose(s.env, "installNativeProperty", InstallNativeProperty);
  RunScript(s.env, R"(
    globalThis.lazyHolder = {};
    globalThis.count = 0;
    globalThis.init = function () { count++; return "built"; };
    installNativeProperty();
  )");
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, Global(s.env, "lazyHolder"),
                                                        Names(s.env, {"lazy"}),
                                                        Global(s.env, "init"), kAll),
            napi_ok);
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  bool lazy = true;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, global, String(s.env, "native"), &lazy),
            napi_ok);
  EXPECT_FALSE(lazy);
  EXPECT_EQ(g_native_getter_calls, 1);
  EXPECT_EQ(RunScriptToString(s.env, "count"), "0");
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, Global(s.env, "lazyHolder"),
                                                  String(s.env, "lazy"), &lazy),
            napi_ok);
  EXPECT_TRUE(lazy);
  EXPECT_EQ(RunScriptToString(s.env, "[native, count, lazyHolder.lazy].join()"), "built,1,built");
}

// A native getter can enter another isolate on the thread and use lazy
// properties there. That isolate's lazy getters run as usual, and when its
// code comes back into the first isolate, the first isolate's query still
// holds: a lazy getter there throws and calls nothing.
namespace {

// globalThis[object_name][property_name] becomes an embedder's native
// accessor property with `getter`, in the current context.
void InstallNativeGetter(const char* object_name,
                         const char* property_name,
                         v8::AccessorNameGetterCallback getter) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Value> object;
  ASSERT_TRUE(context->Global()
                  ->Get(context, v8::String::NewFromUtf8(isolate, object_name).ToLocalChecked())
                  .ToLocal(&object));
  ASSERT_TRUE(object->IsObject());
  EXPECT_TRUE(object.As<v8::Object>()
                  ->SetNativeDataProperty(
                      context, v8::String::NewFromUtf8(isolate, property_name).ToLocalChecked(),
                      getter)
                  .FromMaybe(false));
}

struct TwoIsolates {
  v8::Isolate* a = nullptr;
  v8::Isolate* b = nullptr;
  napi_env env_a = nullptr;
  napi_env env_b = nullptr;
  napi_value holder_a = nullptr;  // A's holderA, with the lazy `lazyA`.
  napi_value holder_b = nullptr;  // B's holderB, with the lazy `lazyB` and `nativeB`.
  bool query_in_b = false;        // What `nativeA` does in B.
  int native_b_calls = 0;
  napi_status read_a_status = napi_ok;
  napi_status query_b_status = napi_ok;
  bool query_b_result = true;
  napi_status read_b_status = napi_ok;
};
TwoIsolates g_two;

// B's `nativeB`: enters A again and reads A's lazy `lazyA`.
void NativeGetterThatReadsLazyAInA(v8::Local<v8::Name>,
                                   const v8::PropertyCallbackInfo<v8::Value>& info) {
  ++g_two.native_b_calls;
  {
    v8::Isolate::Scope enter_a(g_two.a);
    napi_value value = nullptr;
    g_two.read_a_status = napi_get_named_property(g_two.env_a, g_two.holder_a, "lazyA", &value);
    bool pending = false;
    EXPECT_EQ(napi_is_exception_pending(g_two.env_a, &pending), napi_ok);
    if (pending) {
      napi_value exception = nullptr;
      EXPECT_EQ(napi_get_and_clear_last_exception(g_two.env_a, &exception), napi_ok);
    }
  }
  info.GetReturnValue().Set(1);
}

// A's `nativeA`: enters B and queries B's `nativeB`, or reads B's lazy `lazyB`.
void NativeGetterThatUsesB(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
  {
    v8::Isolate::Scope enter_b(g_two.b);
    if (g_two.query_in_b) {
      g_two.query_b_status = unofficial_napi_is_lazy_data_property(
          g_two.env_b, g_two.holder_b, String(g_two.env_b, "nativeB"), &g_two.query_b_result);
    } else {
      napi_value value = nullptr;
      g_two.read_b_status = napi_get_named_property(g_two.env_b, g_two.holder_b, "lazyB", &value);
    }
  }
  info.GetReturnValue().Set(1);
}

napi_value InstallNativeA(napi_env, napi_callback_info) {
  InstallNativeGetter("globalThis", "nativeA", NativeGetterThatUsesB);
  return nullptr;
}

napi_value InstallNativeB(napi_env, napi_callback_info) {
  InstallNativeGetter("holderB", "nativeB", NativeGetterThatReadsLazyAInA);
  return nullptr;
}

// Queries A's `nativeA`, whose getter goes into B, and reports
// "<A's initializer calls>,<lazyA still lazy>,<B's initializer calls>,<lazyB
// still lazy>".
void QueryAcrossTwoIsolates(bool query_in_b, std::string* report) {
  g_two = TwoIsolates{};
  g_two.query_in_b = query_in_b;
  EnvScope a(nullptr);
  g_two.a = v8::Isolate::GetCurrent();
  g_two.env_a = a.env;
  EnvScope b(nullptr);
  g_two.b = v8::Isolate::GetCurrent();
  g_two.env_b = b.env;
  ASSERT_NE(g_two.a, g_two.b);

  // B is entered now.
  Expose(b.env, "installNativeB", InstallNativeB);
  RunScript(b.env, R"(
    globalThis.holderB = {};
    globalThis.countB = 0;
    globalThis.initB = function () { countB++; return "builtB"; };
    installNativeB();
  )");
  g_two.holder_b = Global(b.env, "holderB");
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(b.env, g_two.holder_b,
                                                        Names(b.env, {"lazyB"}),
                                                        Global(b.env, "initB"), kAll),
            napi_ok);

  v8::Isolate::Scope enter_a(g_two.a);
  Expose(a.env, "installNativeA", InstallNativeA);
  RunScript(a.env, R"(
    globalThis.holderA = {};
    globalThis.countA = 0;
    globalThis.initA = function () { countA++; return "builtA"; };
    installNativeA();
  )");
  g_two.holder_a = Global(a.env, "holderA");
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(a.env, g_two.holder_a,
                                                        Names(a.env, {"lazyA"}),
                                                        Global(a.env, "initA"), kAll),
            napi_ok);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(a.env, &global), napi_ok);
  bool lazy = true;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(a.env, global, String(a.env, "nativeA"), &lazy),
            napi_ok);
  EXPECT_FALSE(lazy);

  bool lazy_a = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(a.env, g_two.holder_a, String(a.env, "lazyA"),
                                                  &lazy_a),
            napi_ok);
  const std::string count_a = RunScriptToString(a.env, "countA");
  v8::Isolate::Scope enter_b(g_two.b);
  bool lazy_b = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(b.env, g_two.holder_b, String(b.env, "lazyB"),
                                                  &lazy_b),
            napi_ok);
  const std::string count_b = RunScriptToString(b.env, "countB");
  *report = count_a + "," + (lazy_a ? "true" : "false") + "," + count_b + "," +
            (lazy_b ? "true" : "false");
}

}  // namespace

TEST_F(Test75UnofficialLazyDataProperties, V8TheQueryHoldsAcrossIsolates) {
  // A's query -> nativeA -> B reads B's lazy property: B's initializer runs.
  std::string report;
  QueryAcrossTwoIsolates(false, &report);
  EXPECT_EQ(g_two.read_b_status, napi_ok);
  EXPECT_EQ(g_two.native_b_calls, 0);
  EXPECT_EQ(report, "0,true,1,false");

  // A's query -> nativeA -> B's query -> nativeB -> A reads A's lazy property:
  // A's query still holds, so the read throws and A's initializer is not
  // called.
  QueryAcrossTwoIsolates(true, &report);
  EXPECT_EQ(g_two.query_b_status, napi_ok);
  EXPECT_FALSE(g_two.query_b_result);
  EXPECT_EQ(g_two.native_b_calls, 1);
  EXPECT_EQ(g_two.read_a_status, napi_pending_exception);
  EXPECT_EQ(report, "0,true,0,true");
}

// An embedder's named interceptor would see the lookups with which a first
// read checks its property once the initializer returns, and could fake
// them. The define refuses an object with one, defining nothing, and the
// query calls nothing on it, not even for an accessor property it has. An
// object from a template without handlers is accepted.
namespace {

int g_interceptor_calls = 0;

// Counts every call, and answers for "x" with a data descriptor.
v8::Intercepted DescriptorInterceptor(v8::Local<v8::Name> property,
                                      const v8::PropertyCallbackInfo<v8::Value>& info) {
  ++g_interceptor_calls;
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (!property->StrictEquals(v8::String::NewFromUtf8Literal(isolate, "x"))) {
    return v8::Intercepted::kNo;
  }
  v8::Local<v8::Object> descriptor = v8::Object::New(isolate);
  EXPECT_TRUE(descriptor
                  ->Set(context, v8::String::NewFromUtf8Literal(isolate, "value"),
                        v8::Integer::New(isolate, 7))
                  .FromMaybe(false));
  EXPECT_TRUE(descriptor
                  ->Set(context, v8::String::NewFromUtf8Literal(isolate, "configurable"),
                        v8::True(isolate))
                  .FromMaybe(false));
  info.GetReturnValue().Set(descriptor);
  return v8::Intercepted::kYes;
}

// Sets globalThis.intercepted (a named descriptor interceptor) and
// globalThis.templated (the same kind of template, no handlers).
napi_value MakeTemplateObjects(napi_env, napi_callback_info) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::ObjectTemplate> with_handler = v8::ObjectTemplate::New(isolate);
  with_handler->SetHandler(v8::NamedPropertyHandlerConfiguration(
      nullptr, nullptr, DescriptorInterceptor, nullptr, nullptr, nullptr));
  v8::Local<v8::Object> intercepted = with_handler->NewInstance(context).ToLocalChecked();
  v8::Local<v8::Object> templated =
      v8::ObjectTemplate::New(isolate)->NewInstance(context).ToLocalChecked();
  EXPECT_TRUE(context->Global()
                  ->Set(context, v8::String::NewFromUtf8Literal(isolate, "intercepted"),
                        intercepted)
                  .FromMaybe(false));
  EXPECT_TRUE(context->Global()
                  ->Set(context, v8::String::NewFromUtf8Literal(isolate, "templated"), templated)
                  .FromMaybe(false));
  return nullptr;
}

}  // namespace

TEST_F(Test75UnofficialLazyDataProperties, V8ObjectsWithANamedInterceptorAreRefused) {
  EnvScope s(runtime_.get());
  Expose(s.env, "makeTemplateObjects", MakeTemplateObjects);
  RunScript(s.env, R"(
    makeTemplateObjects();
    Object.defineProperty(intercepted, "y", { get() { return 1; }, configurable: true });
    globalThis.count = 0;
    globalThis.init = function () { count++; return "built"; };
  )");
  napi_value intercepted = Global(s.env, "intercepted");
  napi_value init = Global(s.env, "init");
  g_interceptor_calls = 0;
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, intercepted,
                                                        Names(s.env, {"x", "y"}), init, kAll),
            napi_generic_failure);
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, intercepted, Names(s.env, {}), init,
                                                        kAll),
            napi_generic_failure);
  for (const char* key : {"x", "y"}) {
    SCOPED_TRACE(key);
    bool lazy = true;
    ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, intercepted, String(s.env, key), &lazy),
              napi_ok);
    EXPECT_FALSE(lazy);
  }
  EXPECT_EQ(g_interceptor_calls, 0);
  EXPECT_EQ(RunScriptToString(s.env, "[JSON.stringify(Reflect.ownKeys(intercepted)), count]"
                                     ".join()"),
            R"(["y"],0)");

  napi_value templated = Global(s.env, "templated");
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, templated, Names(s.env, {"x"}), init,
                                                        kAll),
            napi_ok);
  bool lazy = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, templated, String(s.env, "x"), &lazy),
            napi_ok);
  EXPECT_TRUE(lazy);
  EXPECT_EQ(RunScriptToString(s.env, "[templated.x, templated.x, count].join()"), "built,built,1");
}

// Type errors define nothing; a name that cannot be defined stops there.
TEST_F(Test75UnofficialLazyDataProperties, V8ArgumentErrors) {
  EnvScope s(runtime_.get());
  RunScript(s.env, R"(
    globalThis.target = {};
    globalThis.init = function () { return 1; };
    globalThis.mixed = ["a", 1];
    globalThis.huge = new Array(2 ** 32 - 1);
    globalThis.fixed = Object.defineProperty({}, "b", { value: 1 });
    globalThis.sealed = Object.preventExtensions({});
    globalThis.proxyCalls = 0;
    globalThis.proxy = new Proxy({}, new Proxy({}, { get() { proxyCalls++; return undefined; } }));
  )");
  napi_value target = Global(s.env, "target");
  napi_value init = Global(s.env, "init");
  napi_value names = Names(s.env, {"a"});
  napi_value number = nullptr;
  ASSERT_EQ(napi_create_int32(s.env, 1, &number), napi_ok);

  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, number, names, init, kAll),
            napi_object_expected);
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, target, String(s.env, "a"), init,
                                                        kAll),
            napi_array_expected);
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, target, Global(s.env, "mixed"),
                                                        init, kAll),
            napi_name_expected);
  // A hole is not a name either; the length does not size an allocation.
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, target, Global(s.env, "huge"), init,
                                                        kAll),
            napi_name_expected);
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, target, names, target, kAll),
            napi_function_expected);
  EXPECT_EQ(RunScriptToString(s.env, "Reflect.ownKeys(target).length"), "0");

  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, Global(s.env, "fixed"),
                                                        Names(s.env, {"a", "b", "c"}), init, kAll),
            napi_generic_failure);
  EXPECT_EQ(RunScriptToString(s.env, "JSON.stringify(Reflect.ownKeys(fixed))"), R"(["b","a"])");
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, Global(s.env, "sealed"), names,
                                                        init, kAll),
            napi_generic_failure);
  EXPECT_EQ(RunScriptToString(s.env, "Reflect.ownKeys(sealed).length"), "0");
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, Global(s.env, "proxy"), names, init,
                                                        kAll),
            napi_generic_failure);
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, Global(s.env, "proxy"),
                                                        Names(s.env, {}), init, kAll),
            napi_generic_failure);
  EXPECT_EQ(RunScriptToString(s.env, "proxyCalls"), "0");
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, target, Names(s.env, {}), init,
                                                        kAll),
            napi_ok);

  // Array indices would be elements, past an array's length: refused before
  // anything is defined. Other numeric-looking strings are names.
  RunScript(s.env, "globalThis.array = [1, 2];");
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, Global(s.env, "array"),
                                                        Names(s.env, {"5"}), init, kAll),
            napi_name_expected);
  for (const char* index : {"0", "7", "4294967294"}) {
    SCOPED_TRACE(index);
    EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, target, Names(s.env, {"a", index}),
                                                          init, kAll),
              napi_name_expected);
  }
  EXPECT_EQ(RunScriptToString(s.env, "[array.length, '5' in array, Reflect.ownKeys(target).length]"
                                     ".join()"),
            "2,false,0");
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(
                s.env, target, Names(s.env, {"4294967295", "01", "-1", "1.5", ""}), init, kAll),
            napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, "JSON.stringify(Reflect.ownKeys(target))"),
            R"(["4294967295","01","-1","1.5",""])");

  bool lazy = true;
  EXPECT_EQ(unofficial_napi_is_lazy_data_property(s.env, number, String(s.env, "a"), &lazy),
            napi_object_expected);
  EXPECT_EQ(unofficial_napi_is_lazy_data_property(s.env, target, number, &lazy),
            napi_name_expected);
  EXPECT_FALSE(lazy);
}

// A typed array takes every canonical numeric string for an integer index
// and can have no own property by such a name (Object.defineProperty fails),
// but V8 would define one: those names are napi_name_expected, before
// anything is defined. Other names define as on any object, and on an
// ordinary object those names are names.
TEST_F(Test75UnofficialLazyDataProperties, V8TypedArraysRefuseCanonicalNumericNames) {
  EnvScope s(runtime_.get());
  RunScript(s.env, R"(
    globalThis.typed = new Uint8Array(2);
    globalThis.plain = {};
    globalThis.count = 0;
    globalThis.init = function (name) { count++; return "built " + name; };
    globalThis.numeric = ["1.5", "-0", "Infinity", "-Infinity", "NaN", "-1", "4294967295",
                          "1e+21"];
    globalThis.other = ["a", "01", "1e21", "+1", "0.10", "-", "Infinity!"];
  )");
  // JavaScript agrees on both lists.
  EXPECT_EQ(RunScriptToString(s.env, R"(
    const define = (name) =>
        Reflect.defineProperty(new Uint8Array(2), name, { value: 1, configurable: true });
    [numeric.some(define), other.every(define)].join()
  )"),
            "false,true");

  napi_value typed = Global(s.env, "typed");
  napi_value init = Global(s.env, "init");
  for (const char* name :
       {"1.5", "-0", "Infinity", "-Infinity", "NaN", "-1", "4294967295", "1e+21"}) {
    SCOPED_TRACE(name);
    EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, typed, Names(s.env, {"a", name}),
                                                          init, kAll),
              napi_name_expected);
  }
  EXPECT_EQ(RunScriptToString(s.env, "JSON.stringify(Reflect.ownKeys(typed))"), R"(["0","1"])");

  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, typed, Global(s.env, "other"), init,
                                                        kAll),
            napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, "JSON.stringify(Reflect.ownKeys(typed))"),
            R"(["0","1","a","01","1e21","+1","0.10","-","Infinity!"])");
  EXPECT_EQ(RunScriptToString(s.env, "[count, typed['01'], typed['1e21'], count].join()"),
            "0,built 01,built 1e21,2");

  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, Global(s.env, "plain"),
                                                        Global(s.env, "numeric"), init, kAll),
            napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, "JSON.stringify(Reflect.ownKeys(plain))"),
            R"(["1.5","-0","Infinity","-Infinity","NaN","-1","4294967295","1e+21"])");
  EXPECT_EQ(RunScriptToString(s.env, "[plain['-0'], count].join()"), "built -0,3");
}

// V8 throws a TypeError for a property it cannot define when the innermost
// JavaScript frame is strict code, and only returns false otherwise. Either
// way the call is napi_generic_failure with no exception.
TEST_F(Test75UnofficialLazyDataProperties, V8FailuresDoNotDependOnTheCallersStrictness) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  EXPECT_EQ(RunScriptToString(s.env, R"(
    const init = () => 1;
    const sloppy = (object, names) => defineLazy(object, names, init);
    const strict = function (object, names) { "use strict"; return defineLazy(object, names, init); };
    const call = (f, object, names) => {
      try { return f(object, names); } catch (e) { return e.constructor.name; }
    };
    const fixed = Object.defineProperty({}, "b", { value: 1 });
    const sealed = Object.preventExtensions({});
    [call(sloppy, fixed, ["b"]), call(strict, fixed, ["b"]),
     call(sloppy, sealed, ["a"]), call(strict, sealed, ["a"]),
     Reflect.ownKeys(sealed).length].join()
  )"),
            "90,90,90,90,0");
}

// Like every N-API call that may run JS, both calls do nothing while the env
// is terminating: entering V8 then would cancel the termination.
namespace {

struct TerminatingResults {
  napi_status call_status = napi_ok;
  napi_status query_status = napi_ok;
  napi_status define_status = napi_ok;
};
TerminatingResults g_terminating;

// terminateThenUse(holder, names, initializer): terminates, lets V8 deliver
// the termination (a call into JS), then calls both.
napi_value TerminateThenUseLazyProperties(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  EXPECT_EQ(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr), napi_ok);
  EXPECT_EQ(unofficial_napi_terminate_execution(env), napi_ok);
  napi_value ignored = nullptr;
  g_terminating.call_status = napi_call_function(env, argv[0], argv[2], 0, nullptr, &ignored);
  bool lazy = false;
  g_terminating.query_status =
      unofficial_napi_is_lazy_data_property(env, argv[0], String(env, "built"), &lazy);
  g_terminating.define_status =
      unofficial_napi_define_lazy_data_properties(env, argv[0], argv[1], argv[2], kAll);
  return nullptr;
}

}  // namespace

TEST_F(Test75UnofficialLazyDataProperties, V8CallsKeepTermination) {
  EnvScope s(runtime_.get());
  g_terminating = TerminatingResults{};
  Expose(s.env, "terminateThenUse", TerminateThenUseLazyProperties);
  RunScript(s.env, R"(
    globalThis.holder = {};
    globalThis.init = function () { return 1; };
    globalThis.namesRead = false;
    globalThis.caught = "nothing";
    globalThis.after = false;
  )");
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, Global(s.env, "holder"),
                                                        Names(s.env, {"built"}),
                                                        Global(s.env, "init"), kAll),
            napi_ok);

  napi_value source = nullptr;
  ASSERT_EQ(napi_create_string_utf8(s.env, R"(
    const names = [];
    Object.defineProperty(names, 0, { get() { namesRead = true; return "x"; } });
    try { terminateThenUse(holder, names, init); } catch (e) { caught = String(e); }
    after = true;
  )",
                                    NAPI_AUTO_LENGTH, &source),
            napi_ok);
  napi_value result = nullptr;
  EXPECT_NE(napi_run_script(s.env, source, &result), napi_ok);
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  napi_value exception = nullptr;
  (void)napi_get_and_clear_last_exception(s.env, &exception);

  EXPECT_EQ(g_terminating.call_status, napi_pending_exception);
  EXPECT_EQ(g_terminating.query_status, napi_pending_exception);
  EXPECT_EQ(g_terminating.define_status, napi_pending_exception);
  EXPECT_EQ(RunScriptToString(s.env, "[namesRead, caught, after, 'x' in holder].join()"),
            "false,nothing,false,false");
  bool lazy = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, Global(s.env, "holder"),
                                                  String(s.env, "built"), &lazy),
            napi_ok);
  EXPECT_TRUE(lazy);
}

// An embedder's native getter can terminate execution while the query reads
// its descriptor, or while a first read checks its property once the
// initializer returns. The query returns napi_pending_exception, and the
// read throws nothing over the termination: V8 keeps it, and the script
// stops.
namespace {

void TerminatingNativeGetter(v8::Local<v8::Name>,
                             const v8::PropertyCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  isolate->TerminateExecution();
  // A call into JS delivers the termination.
  v8::Local<v8::Value> noop;
  if (context->Global()
          ->Get(context, v8::String::NewFromUtf8Literal(isolate, "noop"))
          .ToLocal(&noop) &&
      noop->IsFunction()) {
    (void)noop.As<v8::Function>()->Call(context, v8::Undefined(isolate), 0, nullptr);
  }
  info.GetReturnValue().Set(1);
}

napi_value InstallTerminatingT(napi_env, napi_callback_info) {
  InstallNativeGetter("holder", "t", TerminatingNativeGetter);
  return nullptr;
}

napi_value InstallTerminatingP(napi_env, napi_callback_info) {
  InstallNativeGetter("holder", "p", TerminatingNativeGetter);
  return nullptr;
}

struct QueryInCallback {
  napi_status status = napi_ok;
  bool result = true;
  bool terminating = false;
};
QueryInCallback g_query_in_callback;

napi_value QueryTFromCallback(napi_env env, napi_callback_info) {
  g_query_in_callback.status = unofficial_napi_is_lazy_data_property(
      env, Global(env, "holder"), String(env, "t"), &g_query_in_callback.result);
  g_query_in_callback.terminating = v8::Isolate::GetCurrent()->IsExecutionTerminating();
  return nullptr;
}

}  // namespace

TEST_F(Test75UnofficialLazyDataProperties, V8TheQueryReportsATerminationFromANativeGetter) {
  EnvScope s(runtime_.get());
  g_query_in_callback = QueryInCallback{};
  Expose(s.env, "installTerminatingT", InstallTerminatingT);
  Expose(s.env, "queryT", QueryTFromCallback);
  RunScript(s.env, R"(
    globalThis.noop = function () {};
    globalThis.holder = {};
    globalThis.caught = "nothing";
    globalThis.after = false;
    installTerminatingT();
  )");

  // From a callback, under JS: the termination stays, and the script stops.
  napi_value result = nullptr;
  EXPECT_NE(napi_run_script(s.env,
                            String(s.env, "try { queryT(); } catch (e) { caught = String(e); }"
                                          " after = true;"),
                            &result),
            napi_ok);
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  napi_value exception = nullptr;
  (void)napi_get_and_clear_last_exception(s.env, &exception);
  EXPECT_EQ(g_query_in_callback.status, napi_pending_exception);
  EXPECT_FALSE(g_query_in_callback.result);
  EXPECT_TRUE(g_query_in_callback.terminating);
  EXPECT_EQ(RunScriptToString(s.env, "[caught, after].join()"), "nothing,false");

  // From C with no JS on the stack.
  bool lazy = true;
  EXPECT_EQ(unofficial_napi_is_lazy_data_property(s.env, Global(s.env, "holder"),
                                                  String(s.env, "t"), &lazy),
            napi_pending_exception);
  EXPECT_FALSE(lazy);
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  (void)napi_get_and_clear_last_exception(s.env, &exception);
}

TEST_F(Test75UnofficialLazyDataProperties, V8ATerminationAfterAnInitializerReturnsStaysPending) {
  EnvScope s(runtime_.get());
  Expose(s.env, "installTerminatingP", InstallTerminatingP);
  RunScript(s.env, R"(
    globalThis.noop = function () {};
    globalThis.holder = {};
    globalThis.count = 0;
    // Replaces its own property with a native accessor that terminates when
    // the read checks the property.
    globalThis.init = function () { count++; installTerminatingP(); return "v"; };
    globalThis.caught = "nothing";
    globalThis.after = false;
  )");
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, Global(s.env, "holder"),
                                                        Names(s.env, {"p"}),
                                                        Global(s.env, "init"), kAll),
            napi_ok);
  napi_value result = nullptr;
  EXPECT_NE(napi_run_script(s.env,
                            String(s.env, "try { holder.p; } catch (e) { caught = String(e); }"
                                          " after = true;"),
                            &result),
            napi_ok);
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  napi_value exception = nullptr;
  (void)napi_get_and_clear_last_exception(s.env, &exception);
  EXPECT_EQ(RunScriptToString(s.env, "[count, caught, after].join()"), "1,nothing,false");
}

#else  // !NAPI_TEST_ENGINE_V8

// No lazy data properties: valid calls fail with napi_generic_failure and
// define nothing; the query sets its result to false.
TEST_F(Test75UnofficialLazyDataProperties, UnsupportedEngineReturnsGenericFailure) {
  EnvScope s(runtime_.get());
  RunScript(s.env, R"(
    globalThis.holder = {};
    globalThis.count = 0;
    globalThis.init = function () { count++; return 1; };
  )");
  napi_value holder = Global(s.env, "holder");
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, holder, Names(s.env, {"a"}),
                                                        Global(s.env, "init"), kAll),
            napi_generic_failure);
  EXPECT_EQ(RunScriptToString(s.env, "['a' in holder, count].join()"), "false,0");
  bool lazy = true;
  EXPECT_EQ(unofficial_napi_is_lazy_data_property(s.env, holder, String(s.env, "a"), &lazy),
            napi_generic_failure);
  EXPECT_FALSE(lazy);
}

#endif  // NAPI_TEST_ENGINE_V8
