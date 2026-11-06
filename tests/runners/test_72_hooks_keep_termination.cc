// A termination (unofficial_napi_terminate_execution, as EdgeJS's
// process.exit() and worker.terminate() use it) that reaches a callback the
// provider runs JS from for the engine (a module hook, a promise hook, a
// serializer hook, the default import() helper) must come back out of it as a
// termination: what the callback caught is not thrown again as a catchable
// exception, and the JS that led there stops.
//
// The JS here asks for it as Node's process.exit() does: a native call that
// terminates, then a call to a JS function, so that V8, which acts on a
// termination at its next stack check, acts on it at once. JS that runs on
// before V8 acts (and what it throws then) is V8's own behaviour.

#include "test_env.h"

#include "node_api.h"
#include "unofficial_napi.h"

#include <string>

class Test72HooksKeepTermination : public FixtureTestBase {};

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

napi_value Terminate(napi_env env, napi_callback_info /*info*/) {
  EXPECT_EQ(unofficial_napi_terminate_execution(env), napi_ok);
  return nullptr;
}

// Terminates and calls tick(), so that V8 acts on it.
void TerminateNow(napi_env env) {
  EXPECT_EQ(unofficial_napi_terminate_execution(env), napi_ok);
  napi_value tick = nullptr;
  napi_value ignored = nullptr;
  napi_get_named_property(env, Global(env), "tick", &tick);
  napi_call_function(env, Global(env), tick, 0, nullptr, &ignored);
}

// Terminates, then throws, like native code that goes on after it asked for
// the termination.
napi_value TerminateAndThrow(napi_env env, napi_callback_info /*info*/) {
  TerminateNow(env);
  napi_throw_error(env, nullptr, "after terminate");
  return nullptr;
}

// Terminates, then hands its argument to napi_fatal_exception.
napi_value TerminateThenFatal(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value error = nullptr;
  EXPECT_EQ(napi_get_cb_info(env, info, &argc, &error, nullptr, nullptr), napi_ok);
  TerminateNow(env);
  napi_fatal_exception(env, error);
  return nullptr;
}

// Evaluates the module its data names and returns the evaluation's promise;
// what the evaluation left pending is the trampoline's to throw.
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

// What EdgeJS does once the stack has unwound: take the pending exception,
// then cancel the termination. Returns whether one was pending.
bool FinishTermination(napi_env env) {
  bool pending = false;
  EXPECT_EQ(napi_is_exception_pending(env, &pending), napi_ok);
  if (pending) {
    napi_value exception = nullptr;
    EXPECT_EQ(napi_get_and_clear_last_exception(env, &exception), napi_ok);
  }
  EXPECT_EQ(unofficial_napi_cancel_terminate_execution(env), napi_ok);
  return pending;
}

void InstallHelpers(napi_env env) {
  DefineGlobalFunction(env, "terminate", Terminate);
  DefineGlobalFunction(env, "terminateAndThrow", TerminateAndThrow);
  DefineGlobalFunction(env, "terminateThenFatal", TerminateThenFatal);
  // Each catch block sets its flag first, by a plain store, at which no
  // engine looks for a termination.
  ASSERT_EQ(RunScript(env,
                      "globalThis.tick = function tick() {};"
                      "globalThis.caught = 'no'; globalThis.after = 'no';"
                      "globalThis.innerCaught = 'no'; globalThis.innerAfter = 'no';"
                      "globalThis.caughtError = 'none';"),
            napi_ok);
}

// What the JS that led to the callback managed to do.
std::string Flags(napi_env env) {
  return "caught=" + GlobalText(env, "caught") + " after=" + GlobalText(env, "after") +
         " innerCaught=" + GlobalText(env, "innerCaught") +
         " innerAfter=" + GlobalText(env, "innerAfter") +
         " error=" + GlobalText(env, "caughtError");
}

constexpr char kNothingRan[] = "caught=no after=no innerCaught=no innerAfter=no error=none";

// Runs `script` and expects it to come back terminated, as test_67's runs do,
// with nothing it wrapped in try/catch caught, and nothing after that run.
void ExpectRunTerminated(napi_env env, const char* script) {
  EXPECT_EQ(RunScript(env, script), napi_pending_exception);
  EXPECT_TRUE(FinishTermination(env)) << "nothing pending after the run";
  EXPECT_EQ(Flags(env), kNothingRan);
  // Cancelling leaves the env usable.
  EXPECT_EQ(RunScript(env, "globalThis.resumed = 'yes';"), napi_ok);
  EXPECT_EQ(GlobalText(env, "resumed"), "yes");
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

// The callback bodies: it terminates; it terminates, then throws; a native
// function it calls terminates, then throws.
constexpr const char* kTerminatingBodies[] = {
    "{ terminate(); tick(); }",
    "{ terminate(); tick(); throw new Error('after terminate'); }",
    "{ terminateAndThrow(); }",
};

constexpr char kEvaluateModuleInTryCatch[] =
    "try { evaluateModule(); } catch (e) { caught = 'yes'; caughtError = e; }\n"
    "after = 'yes';\n";

}  // namespace

// The import.meta callback. V8 threw the null its TryCatch records for the
// termination from the import.meta expression: the module caught it and ran
// on, and the termination was gone.
TEST_F(Test72HooksKeepTermination, ImportMetaCallback) {
  for (const char* body : kTerminatingBodies) {
    SCOPED_TRACE(body);
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    const std::string callback = std::string("(id, meta, wrap) => ") + body;
    SetModuleHooks(s.env, nullptr, Eval(s.env, callback.c_str()));
    unofficial_napi_module module = MakeModule(
        s.env,
        "try { import.meta; } catch (e) { globalThis.innerCaught = 'yes'; caughtError = e; }\n"
        "globalThis.innerAfter = 'yes';\n",
        nullptr);
    ASSERT_NE(module, nullptr);
    DefineGlobalFunction(s.env, "evaluateModule", EvaluateModule, module);
    ExpectRunTerminated(s.env, kEvaluateModuleInTryCatch);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);
  }
}

// A synthetic module's evaluation steps. V8 threw the null from the module's
// evaluation, which the JS that evaluated it caught.
TEST_F(Test72HooksKeepTermination, SyntheticModuleEvaluationSteps) {
  for (const char* body : kTerminatingBodies) {
    SCOPED_TRACE(body);
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    const std::string steps = std::string("(function () ") + body + ")";
    unofficial_napi_module module = MakeModule(s.env, nullptr, Eval(s.env, steps.c_str()));
    ASSERT_NE(module, nullptr);
    DefineGlobalFunction(s.env, "evaluateModule", EvaluateModule, module);
    ExpectRunTerminated(s.env, kEvaluateModuleInTryCatch);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);
  }
}

// The module hooks' import() callback. V8 threw the null from the import()
// expression.
TEST_F(Test72HooksKeepTermination, DynamicImportCallback) {
  for (const char* body : kTerminatingBodies) {
    SCOPED_TRACE(body);
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    const std::string callback =
        std::string("(id, specifier, phase, attributes, name) => ") + body;
    SetModuleHooks(s.env, Eval(s.env, callback.c_str()), nullptr);
    ExpectRunTerminated(
        s.env,
        "try { import('x').catch(() => {}); } catch (e) { caught = 'yes'; caughtError = e; }\n"
        "after = 'yes';\n");
  }
}

// napi_fatal_exception from native code V8 is terminating. V8 threw the
// error over the termination, and the JS that called the native code caught
// it and ran on.
TEST_F(Test72HooksKeepTermination, FatalExceptionWhileTerminating) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  ExpectRunTerminated(
      s.env,
      "try { terminateThenFatal(new Error('fatal')); } catch (e) { caught = 'yes'; caughtError = e; }\n"
      "after = 'yes';\n");
}

#if defined(NAPI_TEST_ENGINE_V8)
// import() with no module hooks calls the `__napi_dynamic_import` helper; only
// V8 does. V8 rejected the import()'s promise with the null its TryCatch
// records for the termination and returned it: the JS that imported ran on,
// and the termination was gone once the run returned.
TEST_F(Test72HooksKeepTermination, DefaultDynamicImportHelper) {
  for (const char* setup :
       {"globalThis.__napi_dynamic_import = (specifier, name) => { terminate(); tick(); };",
        "Object.defineProperty(globalThis, '__napi_dynamic_import',"
        "  { get() { terminate(); tick(); }, configurable: true });"}) {
    SCOPED_TRACE(setup);
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    ASSERT_EQ(RunScript(s.env, setup), napi_ok);
    ExpectRunTerminated(
        s.env,
        "try { import('x').catch(() => {}); } catch (e) { caught = 'yes'; caughtError = e; }\n"
        "after = 'yes';\n");
  }
}
#endif
