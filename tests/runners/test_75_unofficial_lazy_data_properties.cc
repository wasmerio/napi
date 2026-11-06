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
// error. QuickJS implements the same observable behavior with engine-backed
// lazy data properties; the shared tests below apply to both providers.

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

napi_value RunIn(napi_env env, unofficial_napi_realm realm, const char* text) {
  const unofficial_napi_js_source source = unofficial_napi_js_source_from_text(String(env, text));
  napi_value result = nullptr;
  EXPECT_EQ(unofficial_napi_run_script(env, realm, &source, String(env, "lazy-realm.js"),
                                       0, 0, -1, true, false, false, nullptr, &result),
            napi_ok) << text;
  return result;
}

bool IsCollected(napi_env env, napi_ref weak) {
  napi_handle_scope scope = nullptr;
  EXPECT_EQ(napi_open_handle_scope(env, &scope), napi_ok);
  napi_value value = nullptr;
  EXPECT_EQ(napi_get_reference_value(env, weak, &value), napi_ok);
  const bool collected = value == nullptr;
  EXPECT_EQ(napi_close_handle_scope(env, scope), napi_ok);
  return collected;
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
// calls initializer.call(holder, name) once and replaces the property in
// place with a data property that has the attributes asked for.
TEST_F(Test75UnofficialLazyDataProperties, FirstReadCallsTheInitializerOnce) {
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
  EXPECT_EQ(RunScriptToString(s.env,
      "(() => { const keys = []; for (const key in holder) keys.push(key); return keys.join(); })()"),
      "before,all,after");
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
TEST_F(Test75UnofficialLazyDataProperties, AssignmentAndDeletionBeforeAReadCallNothing) {
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
TEST_F(Test75UnofficialLazyDataProperties, AThrowingInitializerLeavesThePropertyLazy) {
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
TEST_F(Test75UnofficialLazyDataProperties, InitializersThatTouchTheirOwnPropertyThrow) {
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
TEST_F(Test75UnofficialLazyDataProperties, AnInitializerThatRedefinesItsPropertyLazilyThrows) {
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
TEST_F(Test75UnofficialLazyDataProperties, TheQueryCallsNothing) {
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

// Reading through a prototype initializes the holder, not the receiver.
// Read-only properties reject inherited writes without being initialized.
TEST_F(Test75UnofficialLazyDataProperties, PrototypeReadsAndReadOnlyWrites) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  EXPECT_EQ(RunScriptToString(s.env, R"(
    const base = {}, child = Object.create(base), calls = [];
    const init = function (name) { calls.push(name); return { holder: this, name }; };
    defineLazy(base, ["read", "reflected"], init);
    defineLazy(base, ["readonly"], init, 0);
    child.readonly = 8;
    let strictError = "none";
    try { (function () { "use strict"; child.readonly = 9; })(); }
    catch (e) { strictError = e.constructor.name; }
    globalThis.base = base;
    globalThis.child = child;
    globalThis.calls = calls;
    [calls.length, strictError, Object.hasOwn(child, "readonly"),
     child.read.holder === base, Object.hasOwn(child, "read"),
     Reflect.get(base, "reflected", {}).holder === base, calls.join(";")].join()
  )"), "0,TypeError,false,true,false,true,read;reflected");
  bool lazy = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, Global(s.env, "base"),
                                                  String(s.env, "readonly"), &lazy), napi_ok);
  EXPECT_TRUE(lazy);
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, Global(s.env, "child"),
                                                  String(s.env, "readonly"), &lazy), napi_ok);
  EXPECT_FALSE(lazy) << "the query must inspect only own properties";
}

// The native setters must treat a read-only lazy property on the prototype
// exactly like the corresponding eager data property, including its status.
TEST_F(Test75UnofficialLazyDataProperties, NativeInheritedReadOnlyWritesDoNotInitialize) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  RunScript(s.env, R"(
    globalThis.calls = 0;
    globalThis.lazyBase = {};
    globalThis.lazyChild = Object.create(lazyBase);
    globalThis.eagerBase = {};
    globalThis.eagerChild = Object.create(eagerBase);
    defineLazy(lazyBase, ["named", "keyed"], function () { calls++; return 42; }, 0);
    Object.defineProperty(eagerBase, "named", { value: 42 });
    Object.defineProperty(eagerBase, "keyed", { value: 42 });
  )");
  napi_value value = nullptr;
  ASSERT_EQ(napi_create_int32(s.env, 8, &value), napi_ok);
  napi_value eager = Global(s.env, "eagerChild");
  napi_value lazy = Global(s.env, "lazyChild");
  const napi_status eager_named = napi_set_named_property(s.env, eager, "named", value);
  EXPECT_EQ(napi_set_named_property(s.env, lazy, "named", value), eager_named);
  const napi_status eager_keyed = napi_set_property(s.env, eager, String(s.env, "keyed"), value);
  EXPECT_EQ(napi_set_property(s.env, lazy, String(s.env, "keyed"), value), eager_keyed);
  // Own readonly lazy properties are also refused without a descriptor read.
  napi_value base = Global(s.env, "lazyBase");
  EXPECT_EQ(napi_set_named_property(s.env, base, "named", value), napi_ok);
  EXPECT_EQ(napi_set_property(s.env, base, String(s.env, "keyed"), value), napi_ok);
  bool pending = true;
  ASSERT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
  EXPECT_FALSE(pending);
  EXPECT_EQ(RunScriptToString(s.env,
      "[calls, Reflect.ownKeys(lazyChild).length].join()"), "0,0");
  for (const char* name : {"named", "keyed"}) {
    bool is_lazy = false;
    ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, Global(s.env, "lazyBase"),
                                                    String(s.env, name), &is_lazy), napi_ok);
    EXPECT_TRUE(is_lazy);
  }
}

// Filtering key lists needs property flags, not a value descriptor. Neither
// N-API key enumeration path may read any initializer while testing flags.
TEST_F(Test75UnofficialLazyDataProperties, NativeFilteredEnumerationDoesNotInitialize) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  RunScript(s.env, R"(
    globalThis.holder = { 1: 1 };
    globalThis.calls = 0;
    const init = function () { calls++; throw new Error("enumeration read a lazy value"); };
    defineLazy(holder, ["all"], init);
    defineLazy(holder, ["readonly"], init, 6);
    defineLazy(holder, ["hidden"], init, 5);
    defineLazy(holder, ["fixed"], init, 3);
    defineLazy(holder, [Symbol("lazy")], init);
  )");
  struct Case {
    int flags;
    const char* all_keys;
    const char* non_index_keys;
  } cases[] = {
      {napi_key_writable, "1;all;hidden;fixed", "all;hidden;fixed"},
      {napi_key_configurable, "1;all;readonly;hidden", "all;readonly;hidden"},
      {napi_key_writable | napi_key_configurable, "1;all;hidden", "all;hidden"},
  };
  napi_value holder = Global(s.env, "holder");
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  for (const Case& c : cases) {
    SCOPED_TRACE(c.flags);
    napi_value keys = nullptr;
    const auto filter = static_cast<napi_key_filter>(c.flags | napi_key_skip_symbols);
    ASSERT_EQ(napi_get_all_property_names(s.env, holder, napi_key_own_only, filter,
                                          napi_key_numbers_to_strings, &keys), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, global, "filteredKeys", keys), napi_ok);
    EXPECT_EQ(RunScriptToString(s.env, "filteredKeys.join(';')"), c.all_keys);
    EXPECT_EQ(RunScriptToString(s.env, "calls"), "0");
    ASSERT_EQ(unofficial_napi_get_own_non_index_properties(s.env, holder,
        static_cast<uint32_t>(filter), &keys), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, global, "filteredKeys", keys), napi_ok);
    EXPECT_EQ(RunScriptToString(s.env, "filteredKeys.join(';')"), c.non_index_keys);
    EXPECT_EQ(RunScriptToString(s.env, "calls"), "0");
  }
}

namespace {
napi_value CollectGarbageFromJs(napi_env env, napi_callback_info) {
  EXPECT_EQ(unofficial_napi_collect_garbage(env), napi_ok);
  return nullptr;
}
}  // namespace

// User code may resize and compact the property array, initialize a sibling,
// and run a GC. The outer read must find its property again after returning.
TEST_F(Test75UnofficialLazyDataProperties, InitializerMayReallocatePropertyStorageAndCollect) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  Expose(s.env, "collectGarbage", CollectGarbageFromJs);
  EXPECT_EQ(RunScriptToString(s.env, R"(
    const holder = { before: 1 };
    let calls = 0;
    defineLazy(holder, ["outer", "inner"], function (name) {
      calls++;
      if (name === "inner") return 41;
      for (let i = 0; i < 2048; i++) this["added" + i] = i;
      for (let i = 0; i < 2047; i++) delete this["added" + i];
      collectGarbage();
      return { value: this.inner + 1, holder: this };
    });
    holder.after = 2;
    const value = holder.outer;
    const descriptor = Object.getOwnPropertyDescriptor(holder, "outer");
    [value.value, value.holder === holder, descriptor.value === value,
     descriptor.writable, descriptor.enumerable, descriptor.configurable,
     calls, Reflect.ownKeys(holder).join(";")].join()
  )"), "42,true,true,true,true,true,2,before;outer;inner;after;added2047");
}

// Object.defineProperty observes V8's lazy descriptor first. The QuickJS
// provider preserves this API contract, including re-entry and freeze behavior.
TEST_F(Test75UnofficialLazyDataProperties, DescriptorRedefinitionAndFreezeMatchDataProperties) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  EXPECT_EQ(RunScriptToString(s.env, R"(
    const holder = {}, calls = [];
    defineLazy(holder, ["defined", "accessor", "frozen", "self"], function (name) {
      calls.push(name);
      if (name === "self") Object.defineProperty(this, name, { value: 9 });
      return 7;
    });
    Object.defineProperty(holder, "defined", { value: 8, enumerable: false });
    Object.defineProperty(holder, "accessor", { get() { return 9; } });
    let error;
    try { holder.self; } catch (e) { error = e.constructor.name; }
    delete holder.self;
    Object.freeze(holder);
    globalThis.frozenHolder = holder;
    globalThis.descriptorCalls = calls;
    [holder.defined, holder.accessor, error, calls.join(";")].join()
  )"), "8,9,RangeError,defined;accessor;self");
  bool lazy = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, Global(s.env, "frozenHolder"),
                                                  String(s.env, "frozen"), &lazy), napi_ok);
  EXPECT_TRUE(lazy) << "freeze must preserve the uninitialized property";
  EXPECT_EQ(RunScriptToString(s.env, R"(
    const d = Object.getOwnPropertyDescriptor(frozenHolder, "frozen");
    [d.value, d.writable, d.enumerable, d.configurable,
     descriptorCalls.join(";")].join()
  )"), "7,false,true,false,defined;accessor;self;frozen");
}

TEST_F(Test75UnofficialLazyDataProperties, DefinitionDoesNotRetainTheCallingRealm) {
  EnvScope s(runtime_.get());
  napi_value holder = RunScript(s.env, "globalThis.holder = ({})");
  napi_value initializer = RunScript(s.env, "(function () { return 42; })");
  napi_value names = Names(s.env, {"value"});
  napi_ref weak_global = nullptr;
  {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    unofficial_napi_realm realm = nullptr;
    napi_value global = nullptr;
    ASSERT_EQ(unofficial_napi_create_realm(s.env, nullptr, &realm, &global), napi_ok);
    ASSERT_EQ(napi_create_reference(s.env, global, 0, &weak_global), napi_ok);
    unofficial_napi_realm_scope entered = 0;
    ASSERT_EQ(unofficial_napi_enter_realm(s.env, realm, &entered), napi_ok);
    ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, holder, names, initializer, kAll),
              napi_ok);
    ASSERT_EQ(unofficial_napi_leave_realm(s.env, entered), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_global))
      << "the definition retained the realm entered only during the define call";
  bool lazy = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, holder, String(s.env, "value"), &lazy),
            napi_ok);
  EXPECT_TRUE(lazy);
  EXPECT_EQ(RunScriptToString(s.env, "holder.value"), "42");
  ASSERT_EQ(napi_delete_reference(s.env, weak_global), napi_ok);
}

TEST_F(Test75UnofficialLazyDataProperties, InitializerRetainsItsRealmUntilNoLongerNeeded) {
  EnvScope s(runtime_.get());
  napi_value holder = RunScript(s.env, "globalThis.holder = ({})");
  napi_ref weak_global = nullptr;
  {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    unofficial_napi_realm realm = nullptr;
    napi_value global = nullptr;
    ASSERT_EQ(unofficial_napi_create_realm(s.env, nullptr, &realm, &global), napi_ok);
    ASSERT_EQ(napi_create_reference(s.env, global, 0, &weak_global), napi_ok);
    napi_value initializer = RunIn(s.env, realm,
        "globalThis.marker = 41; (function (name) { "
        "return { value: marker + 1, self: this, prototype: Object.prototype }; })");
    ASSERT_NE(initializer, nullptr);
    ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, holder, Names(s.env, {"value"}),
                                                          initializer, kAll), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_FALSE(IsCollected(s.env, weak_global)) << "the lazy initializer lost its realm";
  EXPECT_EQ(RunScriptToString(s.env,
      "[holder.value.value, holder.value.self === holder, "
      "Object.getPrototypeOf(holder.value) === holder.value.prototype, "
      "holder.value.prototype !== Object.prototype].join()"), "42,true,true,true");
  RunScript(s.env, "delete holder.value;");
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_global)) << "the initialized definition leaked its realm";
  ASSERT_EQ(napi_delete_reference(s.env, weak_global), napi_ok);
}

// The error introduced by lazy-property machinery belongs to the initializer's
// realm, just like errors thrown by its JavaScript body, even when another
// realm reads the property.
TEST_F(Test75UnofficialLazyDataProperties, InitializationErrorsBelongToTheInitializerRealm) {
  EnvScope s(runtime_.get());
  unofficial_napi_realm initializer_realm = nullptr;
  unofficial_napi_realm reader_realm = nullptr;
  napi_value reader_global = nullptr;
  ASSERT_EQ(unofficial_napi_create_realm(s.env, nullptr, &initializer_realm, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_create_realm(s.env, nullptr, &reader_realm, &reader_global), napi_ok);
  napi_value holder = RunScript(s.env, "({})");
  napi_value initializer = RunIn(s.env, initializer_realm, R"(
    (function (name) {
      if (name === "changed") { delete this[name]; return 1; }
      return this[name];
    })
  )");
  ASSERT_NE(initializer, nullptr);
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, holder,
      Names(s.env, {"changed", "recursive"}), initializer, kAll), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, reader_global, "holder", holder), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, reader_global, "initializerTypeError",
      RunIn(s.env, initializer_realm, "TypeError")), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, reader_global, "initializerRangeError",
      RunIn(s.env, initializer_realm, "RangeError")), napi_ok);
  napi_value result = RunIn(s.env, reader_realm, R"(
    (() => {
      let changed, recursive;
      try { holder.changed; } catch (error) { changed = error; }
      try { holder.recursive; } catch (error) { recursive = error; }
      return changed.constructor === initializerTypeError && !(changed instanceof TypeError) &&
          recursive.constructor === initializerRangeError && !(recursive instanceof RangeError) &&
          !("changed" in holder);
    })()
  )");
  ASSERT_NE(result, nullptr);
  bool correct_realm = false;
  ASSERT_EQ(napi_get_value_bool(s.env, result, &correct_realm), napi_ok);
  EXPECT_TRUE(correct_realm);
  bool lazy = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, holder, String(s.env, "recursive"), &lazy),
            napi_ok);
  EXPECT_TRUE(lazy) << "the recursive read must remain retryable";
}

namespace {
struct NativeInitializerRecord {
  napi_env env = nullptr;
  napi_value global = nullptr;
  napi_value prototype = nullptr;
  napi_value holder = nullptr;
  int calls = 0;
  bool delete_property = false;
};

napi_value RecordNativeInitializerRealm(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value name = nullptr;
  napi_value holder = nullptr;
  void* data = nullptr;
  EXPECT_EQ(napi_get_cb_info(env, info, &argc, &name, &holder, &data), napi_ok);
  auto* record = static_cast<NativeInitializerRecord*>(data);
  if (record == nullptr) return nullptr;
  ++record->calls;
  EXPECT_EQ(env, record->env);
  EXPECT_EQ(argc, 1u);
  napi_value global = nullptr;
  EXPECT_EQ(napi_get_global(env, &global), napi_ok);
  bool same = false;
  EXPECT_EQ(napi_strict_equals(env, global, record->global, &same), napi_ok);
  EXPECT_TRUE(same) << "the callback must enter the native function's creation realm";
  EXPECT_EQ(napi_strict_equals(env, holder, record->holder, &same), napi_ok);
  EXPECT_TRUE(same) << "the lazy holder must be the callback receiver";
  char key[16] = {};
  size_t length = 0;
  EXPECT_EQ(napi_get_value_string_utf8(env, name, key, sizeof(key), &length), napi_ok);
  EXPECT_EQ(std::string(key, length), "value");
  napi_value object = nullptr;
  EXPECT_EQ(napi_create_object(env, &object), napi_ok);
  napi_value prototype = nullptr;
  EXPECT_EQ(napi_get_prototype(env, object, &prototype), napi_ok);
  EXPECT_EQ(napi_strict_equals(env, prototype, record->prototype, &same), napi_ok);
  EXPECT_TRUE(same) << "objects made by the callback must use the initializer's intrinsics";
  if (record->delete_property) {
    EXPECT_EQ(napi_delete_property(env, holder, name, nullptr), napi_ok);
  }
  return object;
}
}  // namespace

TEST_F(Test75UnofficialLazyDataProperties, NativeInitializerUsesItsCreationRealm) {
  EnvScope s(runtime_.get());
  unofficial_napi_realm initializer_realm = nullptr;
  unofficial_napi_realm reader_realm = nullptr;
  napi_value reader_global = nullptr;
  NativeInitializerRecord record;
  record.env = s.env;
  ASSERT_EQ(unofficial_napi_create_realm(s.env, nullptr, &initializer_realm, &record.global), napi_ok);
  ASSERT_EQ(unofficial_napi_create_realm(s.env, nullptr, &reader_realm, &reader_global), napi_ok);
  record.prototype = RunIn(s.env, initializer_realm, "Object.prototype");
  record.holder = RunScript(s.env, "({})");
  napi_value initializer = nullptr;
  unofficial_napi_realm_scope entered = 0;
  ASSERT_EQ(unofficial_napi_enter_realm(s.env, initializer_realm, &entered), napi_ok);
  ASSERT_EQ(napi_create_function(s.env, "initialize", NAPI_AUTO_LENGTH,
                                RecordNativeInitializerRealm, &record, &initializer), napi_ok);
  ASSERT_EQ(unofficial_napi_leave_realm(s.env, entered), napi_ok);
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, record.holder,
      Names(s.env, {"value"}), initializer, kAll), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, reader_global, "holder", record.holder), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, reader_global, "initializerPrototype", record.prototype),
            napi_ok);
  napi_value result = RunIn(s.env, reader_realm,
      "Object.getPrototypeOf(holder.value) === initializerPrototype && holder.value === holder.value");
  ASSERT_NE(result, nullptr);
  bool correct_realm = false;
  ASSERT_EQ(napi_get_value_bool(s.env, result, &correct_realm), napi_ok);
  EXPECT_TRUE(correct_realm);
  EXPECT_EQ(record.calls, 1);

  // The engine-generated error after a native initializer mutates its
  // property must also use the native function's creation realm.
  record.delete_property = true;
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, record.holder,
      Names(s.env, {"value"}), initializer, kAll), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, reader_global, "initializerTypeError",
      RunIn(s.env, initializer_realm, "TypeError")), napi_ok);
  result = RunIn(s.env, reader_realm, R"(
    (() => {
      try { holder.value; return false; }
      catch (error) {
        return error.constructor === initializerTypeError && !(error instanceof TypeError) &&
            !("value" in holder);
      }
    })()
  )");
  ASSERT_NE(result, nullptr);
  correct_realm = false;
  ASSERT_EQ(napi_get_value_bool(s.env, result, &correct_realm), napi_ok);
  EXPECT_TRUE(correct_realm);
  EXPECT_EQ(record.calls, 2);
}

// Name getters may run JS while the full list is validated. Earlier names
// must be captured before later getters mutate the array; a throwing getter
// must propagate before any property is installed.
TEST_F(Test75UnofficialLazyDataProperties, NameGettersAreSnapshottedAndExceptionsPropagate) {
  EnvScope s(runtime_.get());
  RunScript(s.env, R"(
    globalThis.target = {};
    globalThis.init = function (name) { return name; };
    globalThis.names = ["first"];
    Object.defineProperty(names, 1, { get() {
      names[0] = "replaced";
      return "nul\u0000\ud83d\ude42";
    } });
    globalThis.throwingNames = ["untouched"];
    Object.defineProperty(throwingNames, 1, { get() { throw new RangeError("name getter"); } });
    globalThis.untouched = {};
  )");
  ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, Global(s.env, "target"),
      Global(s.env, "names"), Global(s.env, "init"), kAll), napi_ok);
  EXPECT_EQ(RunScriptToString(s.env,
      "Reflect.ownKeys(target).join('|') === 'first|nul\\u0000\\ud83d\\ude42' && "
      "target.first === 'first' && target['nul\\u0000\\ud83d\\ude42'] === 'nul\\u0000\\ud83d\\ude42' && "
      "!('replaced' in target)"), "true");
  EXPECT_EQ(unofficial_napi_define_lazy_data_properties(s.env, Global(s.env, "untouched"),
      Global(s.env, "throwingNames"), Global(s.env, "init"), kAll), napi_pending_exception);
  napi_value error = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &error), napi_ok);
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "nameError", error), napi_ok);
  EXPECT_EQ(RunScriptToString(s.env,
      "nameError instanceof RangeError && nameError.message === 'name getter' && "
      "Reflect.ownKeys(untouched).length === 0"), "true");
}

// Cloning reads enumerable values, so each lazy field is materialized once
// and the copy contains ordinary data properties. Hidden fields stay lazy.
TEST_F(Test75UnofficialLazyDataProperties, StructuredCloneMaterializesEnumerableValues) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  RunScript(s.env, R"(
    globalThis.source = { before: 1 };
    globalThis.calls = [];
    const init = function (name) { calls.push(name); return { made: name }; };
    defineLazy(source, ["first", "second"], init);
    defineLazy(source, ["hidden"], init, 0);
    source.after = 2;
  )");
  napi_value source = Global(s.env, "source");
  napi_value clone = nullptr;
  ASSERT_EQ(unofficial_napi_structured_clone(s.env, source, nullptr, &clone), napi_ok);
  ASSERT_NE(clone, nullptr);
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "clone", clone), napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, R"(
    [calls.join(";"), Reflect.ownKeys(clone).join(";"),
     clone.first.made, clone.second.made, clone.first !== source.first,
     Object.getOwnPropertyDescriptor(clone, "first").value === clone.first].join()
  )"), "first;second,before;first;second;after,first,second,true,true");
  bool lazy = true;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, clone, String(s.env, "first"), &lazy),
            napi_ok);
  EXPECT_FALSE(lazy);
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, source, String(s.env, "hidden"), &lazy),
            napi_ok);
  EXPECT_TRUE(lazy);
  EXPECT_EQ(RunScriptToString(s.env, "calls.length"), "2");
}

TEST_F(Test75UnofficialLazyDataProperties, SerializerMaterializesEnumerableValues) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  napi_value serdes = nullptr;
  ASSERT_EQ(unofficial_napi_create_serdes_binding(s.env, &serdes), napi_ok);
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "serdes", serdes), napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, R"(
    globalThis.source = { before: 1 };
    globalThis.calls = [];
    const init = function (name) { calls.push(name); return { made: name }; };
    defineLazy(source, ["first", "second"], init);
    defineLazy(source, ["hidden"], init, 0);
    source.after = 2;
    const serializer = new serdes.Serializer();
    serializer.writeHeader();
    const written = serializer.writeValue(source);
    const deserializer = new serdes.Deserializer(serializer.releaseBuffer());
    deserializer.readHeader();
    globalThis.clone = deserializer.readValue();
    [written, calls.join(";"), Reflect.ownKeys(clone).join(";"),
     clone.first.made, clone.second.made, clone.first !== source.first].join()
  )"), "true,first;second,before;first;second;after,first,second,true");
  bool lazy = true;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, Global(s.env, "clone"),
                                                  String(s.env, "first"), &lazy), napi_ok);
  EXPECT_FALSE(lazy);
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, Global(s.env, "source"),
                                                  String(s.env, "hidden"), &lazy), napi_ok);
  EXPECT_TRUE(lazy);
  EXPECT_EQ(RunScriptToString(s.env, "calls.length"), "2");
}

// A clone must preserve the original initializer exception in the N-API
// environment. Clearing it and retrying the clone must call the initializer
// again, because the failed read did not consume the lazy definition.
TEST_F(Test75UnofficialLazyDataProperties, CloneAndMessagePropagateInitializerExceptionsAndRetry) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  for (bool use_message : {false, true}) {
    SCOPED_TRACE(use_message ? "message" : "clone");
    auto clone_value = [&](napi_value source, napi_value* result) {
      if (!use_message) return unofficial_napi_structured_clone(s.env, source, nullptr, result);
      unofficial_napi_message message = nullptr;
      napi_status status = unofficial_napi_message_create(s.env, source, &message);
      if (status != napi_ok) return status;
      return unofficial_napi_message_take(s.env, message, result);
    };
    RunScript(s.env, R"(
      globalThis.source = {};
      globalThis.calls = 0;
      globalThis.fail = true;
      globalThis.expectedError = new RangeError("lazy clone initializer");
      defineLazy(source, ["value"], function () {
        calls++;
        if (fail) throw expectedError;
        return { made: 42 };
      });
    )");
    napi_value source = Global(s.env, "source");
    napi_value clone = nullptr;
    EXPECT_EQ(clone_value(source, &clone), napi_pending_exception);
    bool pending = false;
    ASSERT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
    EXPECT_TRUE(pending);
    napi_value exception = nullptr;
    ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &exception), napi_ok);
    bool same_error = false;
    ASSERT_EQ(napi_strict_equals(s.env, exception, Global(s.env, "expectedError"), &same_error), napi_ok);
    EXPECT_TRUE(same_error) << "structured clone must propagate the original thrown value";
    bool lazy = false;
    ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, source, String(s.env, "value"), &lazy),
              napi_ok);
    EXPECT_TRUE(lazy);
    EXPECT_EQ(RunScriptToString(s.env, "calls"), "1");

    RunScript(s.env, "fail = false;");
    ASSERT_EQ(clone_value(source, &clone), napi_ok);
    ASSERT_NE(clone, nullptr);
    napi_value global = nullptr;
    ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, global, "clone", clone), napi_ok);
    EXPECT_EQ(RunScriptToString(s.env,
        "[clone.value.made, source.value.made, clone.value !== source.value, calls].join()"),
        "42,42,true,2");
    ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, source, String(s.env, "value"), &lazy),
              napi_ok);
    EXPECT_FALSE(lazy);
  }
}

TEST_F(Test75UnofficialLazyDataProperties, SerializerPropagatesInitializerExceptionAndRetries) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  napi_value serdes = nullptr;
  ASSERT_EQ(unofficial_napi_create_serdes_binding(s.env, &serdes), napi_ok);
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "serdes", serdes), napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, R"(
    globalThis.source = {};
    globalThis.calls = 0;
    globalThis.fail = true;
    globalThis.expectedError = new RangeError("lazy serializer initializer");
    defineLazy(source, ["value"], function () {
      calls++;
      if (fail) throw expectedError;
      return { made: 42 };
    });
    (() => {
      const serializer = new serdes.Serializer();
      serializer.writeHeader();
      try { serializer.writeValue(source); return "no exception"; }
      catch (error) { return error === expectedError ? "original exception" : String(error); }
    })()
  )"), "original exception");
  napi_value source = Global(s.env, "source");
  bool lazy = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, source, String(s.env, "value"), &lazy),
            napi_ok);
  EXPECT_TRUE(lazy);
  EXPECT_EQ(RunScriptToString(s.env, "calls"), "1");
  EXPECT_EQ(RunScriptToString(s.env, R"(
    fail = false;
    (() => {
      const serializer = new serdes.Serializer();
      serializer.writeHeader();
      const written = serializer.writeValue(source);
      const deserializer = new serdes.Deserializer(serializer.releaseBuffer());
      deserializer.readHeader();
      const clone = deserializer.readValue();
      return [written, clone.value.made, source.value.made,
              clone.value !== source.value, calls].join();
    })()
  )"), "true,42,42,true,2");
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, source, String(s.env, "value"), &lazy),
            napi_ok);
  EXPECT_FALSE(lazy);
}

// The serializer encounters the buffer before the lazy field. Its backing
// storage must already be retained when the later initializer deletes the
// source's last reference and runs GC. Opaque messages also keep it between
// serialization and delivery, when no JavaScript value owns the payload.
TEST_F(Test75UnofficialLazyDataProperties, CloneAndMessageRetainEarlierSharedBufferDuringInitialization) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  Expose(s.env, "collectGarbage", CollectGarbageFromJs);
  for (bool use_message : {false, true}) {
    SCOPED_TRACE(use_message ? "message" : "structured clone");
    RunScript(s.env, R"(
      globalThis.source = { buffer: new SharedArrayBuffer(8) };
      new Uint8Array(source.buffer)[0] = 42;
      globalThis.calls = 0;
      defineLazy(source, ["late"], function () {
        calls++;
        delete this.buffer;
        collectGarbage();
        return 7;
      });
    )");
    napi_value source = Global(s.env, "source");
    napi_value clone = nullptr;
    if (use_message) {
      unofficial_napi_message message = nullptr;
      ASSERT_EQ(unofficial_napi_message_create(s.env, source, &message), napi_ok);
      ASSERT_NE(message, nullptr);
      ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
      ASSERT_EQ(unofficial_napi_message_take(s.env, message, &clone), napi_ok);
    } else {
      ASSERT_EQ(unofficial_napi_structured_clone(s.env, source, nullptr, &clone), napi_ok);
    }
    ASSERT_NE(clone, nullptr);
    napi_value global = nullptr;
    ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, global, "clone", clone), napi_ok);
    ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
    EXPECT_EQ(RunScriptToString(s.env, R"(
      ["buffer" in source, clone.buffer instanceof SharedArrayBuffer,
       clone.buffer.byteLength, new Uint8Array(clone.buffer)[0], clone.late, calls,
       Object.getOwnPropertyDescriptor(clone, "late").value].join()
    )"), "false,true,8,42,7,1,7");
    bool lazy = true;
    ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, clone, String(s.env, "late"), &lazy),
              napi_ok);
    EXPECT_FALSE(lazy);
  }
}

// A raw Serializer has no default SharedArrayBuffer ID/owner registry. It
// must reject the buffer rather than emit bytes containing an unowned pointer.
TEST_F(Test75UnofficialLazyDataProperties, DefaultSerializerRejectsSharedArrayBuffer) {
  EnvScope s(runtime_.get());
  Expose(s.env, "defineLazy", DefineLazyFromJs);
  napi_value serdes = nullptr;
  ASSERT_EQ(unofficial_napi_create_serdes_binding(s.env, &serdes), napi_ok);
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "serdes", serdes), napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, R"(
    globalThis.source = { buffer: new SharedArrayBuffer(8) };
    globalThis.calls = 0;
    defineLazy(source, ["late"], function () { calls++; return 7; });
    (() => {
      const serializer = new serdes.Serializer();
      serializer.writeHeader();
      try { serializer.writeValue(source); return "no exception"; }
      catch (error) { return error instanceof Error ? "rejected" : String(error); }
    })()
  )"), "rejected");
  EXPECT_EQ(RunScriptToString(s.env, "calls"), "0");
  bool lazy = false;
  ASSERT_EQ(unofficial_napi_is_lazy_data_property(s.env, Global(s.env, "source"),
                                                  String(s.env, "late"), &lazy), napi_ok);
  EXPECT_TRUE(lazy);
}

// An unreachable holder and initializer may form a cycle. Neither the lazy
// definition nor its initializer can be a hidden permanent GC root.
TEST_F(Test75UnofficialLazyDataProperties, UnreachableInitializerCyclesAreCollected) {
  EnvScope s(runtime_.get());
  napi_ref weak_holder = nullptr;
  napi_ref weak_initializer = nullptr;
  {
    napi_handle_scope handles = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &handles), napi_ok);
    napi_value holder = RunScript(s.env,
        "(() => { const holder = {}; holder.init = function () { return holder; }; "
        "return holder; })()");
    napi_value initializer = nullptr;
    ASSERT_EQ(napi_get_named_property(s.env, holder, "init", &initializer), napi_ok);
    ASSERT_EQ(unofficial_napi_define_lazy_data_properties(s.env, holder, Names(s.env, {"value"}),
                                                          initializer, kAll), napi_ok);
    ASSERT_EQ(napi_delete_property(s.env, holder, String(s.env, "init"), nullptr), napi_ok);
    ASSERT_EQ(napi_create_reference(s.env, holder, 0, &weak_holder), napi_ok);
    ASSERT_EQ(napi_create_reference(s.env, initializer, 0, &weak_initializer), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, handles), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_TRUE(IsCollected(s.env, weak_holder));
  EXPECT_TRUE(IsCollected(s.env, weak_initializer));
  ASSERT_EQ(napi_delete_reference(s.env, weak_holder), napi_ok);
  ASSERT_EQ(napi_delete_reference(s.env, weak_initializer), napi_ok);
}

#if defined(NAPI_TEST_ENGINE_V8)

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
#endif  // NAPI_TEST_ENGINE_V8

TEST_F(Test75UnofficialLazyDataProperties, ArgumentErrors) {
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
TEST_F(Test75UnofficialLazyDataProperties, TypedArraysRefuseCanonicalNumericNames) {
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
#if defined(NAPI_TEST_ENGINE_V8)
  // V8's ordinary defineProperty agrees on both lists. QuickJS currently
  // accepts the canonical name "NaN" through Reflect.defineProperty; that
  // pre-existing engine gap is unrelated to the lazy API, which must reject
  // every canonical numeric name on both providers below.
  EXPECT_EQ(RunScriptToString(s.env, R"(
    const define = (name) =>
        Reflect.defineProperty(new Uint8Array(2), name, { value: 1, configurable: true });
    [numeric.some(define), other.every(define)].join()
  )"),
            "false,true");
#endif

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
TEST_F(Test75UnofficialLazyDataProperties, FailuresDoNotDependOnTheCallersStrictness) {
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

#if defined(NAPI_TEST_ENGINE_V8)

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

#endif  // NAPI_TEST_ENGINE_V8
