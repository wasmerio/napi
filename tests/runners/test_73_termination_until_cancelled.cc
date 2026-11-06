// A termination (unofficial_napi_terminate_execution, as EdgeJS's
// process.exit() and worker.terminate() use it) lasts until it is cancelled:
// calls into JS fail meanwhile, and nothing the JS wrapped in try/catch
// catches it, also when native code goes on after a call it failed, cancels
// it, or runs where the engine does not look for it.
//
// The JS asks for it as test_72's does: a native call that terminates, then
// a call to a JS function, so that V8, which acts on a termination at its
// next stack check, acts on it at once.

#include "test_env.h"

#include "node_api.h"
#include "unofficial_napi.h"

#include <string>

class Test73TerminationUntilCancelled : public FixtureTestBase {};

namespace {

napi_value Str(napi_env env, const char* value) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out) != napi_ok) return nullptr;
  return out;
}

napi_value Global(napi_env env) {
  napi_value global = nullptr;
  EXPECT_EQ(napi_get_global(env, &global), napi_ok);
  return global;
}

// Terminates and calls tick(), so that V8 acts on it.
void TerminateNow(napi_env env) {
  EXPECT_EQ(unofficial_napi_terminate_execution(env), napi_ok);
  napi_value tick = nullptr;
  napi_value ignored = nullptr;
  napi_get_named_property(env, Global(env), "tick", &tick);
  napi_call_function(env, Global(env), tick, 0, nullptr, &ignored);
}

// Terminates, then cancels, without taking what the failed call left
// pending.
napi_value TerminateThenCancel(napi_env env, napi_callback_info /*info*/) {
  TerminateNow(env);
  EXPECT_EQ(unofficial_napi_cancel_terminate_execution(env), napi_ok);
  return nullptr;
}

void DefineGlobalFunction(napi_env env, const char* name, napi_callback cb, void* data = nullptr) {
  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, data, &fn), napi_ok);
  ASSERT_EQ(napi_set_named_property(env, Global(env), name, fn), napi_ok);
}

napi_status RunScript(napi_env env, const char* source) {
  napi_value ignored = nullptr;
  return napi_run_script(env, Str(env, source), &ignored);
}

napi_value Eval(napi_env env, const char* source) {
  napi_value result = nullptr;
  EXPECT_EQ(napi_run_script(env, Str(env, source), &result), napi_ok) << source;
  return result;
}

std::string GlobalText(napi_env env, const char* name) {
  napi_value value = nullptr;
  napi_value string = nullptr;
  char text[256] = {};
  size_t length = 0;
  if (napi_get_named_property(env, Global(env), name, &value) != napi_ok ||
      napi_coerce_to_string(env, value, &string) != napi_ok ||
      napi_get_value_string_utf8(env, string, text, sizeof(text), &length) != napi_ok) {
    return "<unreadable>";
  }
  return std::string(text, length);
}

void InstallHelpers(napi_env env) {
  DefineGlobalFunction(env, "terminateThenCancel", TerminateThenCancel);
  // Each catch block sets its flag first, by a plain store, at which no
  // engine looks for a termination.
  ASSERT_EQ(RunScript(env,
                      "globalThis.tick = function tick() {};"
                      "globalThis.caught = 'no'; globalThis.after = 'no';"
                      "globalThis.innerCaught = 'no'; globalThis.innerAfter = 'no';"
                      "globalThis.caughtError = 'none'; globalThis.afterCall = 'no';"),
            napi_ok);
}

// What the JS that led there managed to do.
std::string Flags(napi_env env) {
  return "caught=" + GlobalText(env, "caught") + " after=" + GlobalText(env, "after") +
         " innerCaught=" + GlobalText(env, "innerCaught") +
         " innerAfter=" + GlobalText(env, "innerAfter") +
         " error=" + GlobalText(env, "caughtError") + " afterCall=" + GlobalText(env, "afterCall");
}

void SetModuleHooks(napi_env env, napi_value import_callback, napi_value meta_callback) {
  unofficial_napi_module_hooks hooks{};
  hooks.size = sizeof(hooks);
  hooks.version = UNOFFICIAL_NAPI_MODULE_HOOKS_VERSION;
  hooks.import_module_dynamically = import_callback;
  hooks.initialize_import_meta_object = meta_callback;
  ASSERT_EQ(unofficial_napi_module_wrap_set_hooks(env, &hooks), napi_ok);
}

// A linked and instantiated module: source text, or synthetic with `steps`
// when `source_text` is null.
unofficial_napi_module MakeModule(napi_env env, const char* source_text, napi_value steps) {
  napi_value wrapper = nullptr;
  napi_value undefined = nullptr;
  EXPECT_EQ(napi_create_object(env, &wrapper), napi_ok);
  EXPECT_EQ(napi_get_undefined(env, &undefined), napi_ok);
  unofficial_napi_module_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  options.wrapper = wrapper;
  unofficial_napi_js_source source{};
  if (source_text != nullptr) {
    source = unofficial_napi_js_source_from_text(Str(env, source_text));
    options.kind = unofficial_napi_module_source_text;
    options.url = Str(env, "hooked.mjs");
    options.payload.source_text.source = &source;
    options.payload.source_text.host_defined_option_id = undefined;
  } else {
    options.kind = unofficial_napi_module_synthetic;
    options.url = Str(env, "synthetic");
    options.payload.synthetic.export_names = Eval(env, "['value']");
    options.payload.synthetic.synthetic_evaluation_steps = steps;
  }
  unofficial_napi_module_create_result created{};
  EXPECT_EQ(unofficial_napi_module_wrap_create(env, &options, &created), napi_ok);
  if (created.module == nullptr) return nullptr;
  EXPECT_EQ(unofficial_napi_module_wrap_link(env, created.module, 0, nullptr), napi_ok);
  EXPECT_EQ(unofficial_napi_module_wrap_instantiate(env, created.module), napi_ok);
  return created.module;
}

// Evaluates the module its data names; what that left pending is the
// trampoline's to throw.
napi_value EvaluateModule(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data) != napi_ok) return nullptr;
  napi_value result = nullptr;
  if (unofficial_napi_module_wrap_evaluate(
          env, static_cast<unofficial_napi_module>(data), -1, false, &result) != napi_ok) {
    return nullptr;
  }
  return result;
}

constexpr char kEvaluateModuleInTryCatch[] =
    "try { evaluateModule(); } catch (e) { caught = 'yes'; caughtError = e; }\n"
    "after = 'yes';\n";

}  // namespace

// Native code that terminates, makes a call the termination fails, and
// cancels the termination before it returns, without taking what that call
// left pending. Nothing threw that: the JS that called goes on as after any
// call. V8 records a termination as a null pending exception, which it would
// otherwise throw into that JS as a catchable null; QuickJS drops its
// uncatchable error at the cancel.
TEST_F(Test73TerminationUntilCancelled, CancelledInsideANativeFunction) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  EXPECT_EQ(RunScript(s.env,
                      "try { terminateThenCancel(); afterCall = 'yes'; }\n"
                      "catch (e) { caught = 'yes'; caughtError = e; }\n"
                      "after = 'yes';\n"),
            napi_ok);
  EXPECT_EQ(Flags(s.env),
            "caught=no after=yes innerCaught=no innerAfter=no error=none afterCall=yes");
  bool pending = true;
  EXPECT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
  EXPECT_FALSE(pending);
}

// The same inside the import.meta hook, which throws what its callback left
// pending from the import.meta expression.
TEST_F(Test73TerminationUntilCancelled, CancelledInsideAModuleHook) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  SetModuleHooks(s.env, nullptr,
                 Eval(s.env,
                      "(id, meta, wrap) => {\n"
                      "  try { terminateThenCancel(); afterCall = 'yes'; }\n"
                      "  catch (e) { caught = 'yes'; caughtError = e; }\n"
                      "}"));
  unofficial_napi_module module = MakeModule(
      s.env,
      "try { import.meta; } catch (e) { globalThis.innerCaught = 'yes'; caughtError = e; }\n"
      "globalThis.innerAfter = 'yes';\n",
      nullptr);
  ASSERT_NE(module, nullptr);
  DefineGlobalFunction(s.env, "evaluateModule", EvaluateModule, module);
  EXPECT_EQ(RunScript(s.env, kEvaluateModuleInTryCatch), napi_ok);
  EXPECT_EQ(Flags(s.env),
            "caught=no after=yes innerCaught=no innerAfter=yes error=none afterCall=yes");
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);
}
