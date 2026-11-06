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

class Test75HooksKeepTermination : public FixtureTestBase {};

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

// Throws an error with a code through napi_throw_error, napi_throw_type_error,
// napi_throw_range_error or node_api_throw_syntax_error, as its argument says.
napi_value ThrowWithCode(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value kind_value = nullptr;
  EXPECT_EQ(napi_get_cb_info(env, info, &argc, &kind_value, nullptr, nullptr), napi_ok);
  int32_t kind = 0;
  EXPECT_EQ(napi_get_value_int32(env, kind_value, &kind), napi_ok);
  switch (kind) {
    case 0: napi_throw_error(env, "ERR_TEST", "error"); break;
    case 1: napi_throw_type_error(env, "ERR_TEST", "type error"); break;
    case 2: napi_throw_range_error(env, "ERR_TEST", "range error"); break;
    default: node_api_throw_syntax_error(env, "ERR_TEST", "syntax error"); break;
  }
  return nullptr;
}

#if defined(NAPI_TEST_ENGINE_V8)
// Instantiates the module its data names; what that left pending is the
// trampoline's to throw.
napi_value InstantiateModule(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data) != napi_ok) return nullptr;
  unofficial_napi_module_wrap_instantiate(env, static_cast<unofficial_napi_module>(data));
  return nullptr;
}
#endif

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
  DefineGlobalFunction(env, "throwWithCode", ThrowWithCode);
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
TEST_F(Test75HooksKeepTermination, ImportMetaCallback) {
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
TEST_F(Test75HooksKeepTermination, SyntheticModuleEvaluationSteps) {
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
TEST_F(Test75HooksKeepTermination, DynamicImportCallback) {
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
TEST_F(Test75HooksKeepTermination, FatalExceptionWhileTerminating) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  ExpectRunTerminated(
      s.env,
      "try { terminateThenFatal(new Error('fatal')); } catch (e) { caught = 'yes'; caughtError = e; }\n"
      "after = 'yes';\n");
}

// The promise reject callback returns nothing, so the JS that rejected goes
// on until the engine next looks for a termination, and the run may come
// back before it does. The termination must still not be catchable, nor
// lost: that run or the next one stops. V8 dropped it once the run returned,
// and the env ran on as if it had never been asked to stop.
TEST_F(Test75HooksKeepTermination, PromiseRejectCallback) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  ASSERT_EQ(unofficial_napi_set_promise_reject_callback(
                s.env, Eval(s.env, "(type, promise, reason) => { terminate(); tick(); }")),
            napi_ok);
  const napi_status run = RunScript(
      s.env,
      "try { Promise.reject(1); } catch (e) { caught = 'yes'; caughtError = e; }\n"
      "after = 'yes';\n");
  const napi_status next = RunScript(s.env, "globalThis.next = 'yes';");
  FinishTermination(s.env);
  EXPECT_TRUE(run == napi_pending_exception || next == napi_pending_exception)
      << "run " << run << ", next run " << next;
  EXPECT_EQ(GlobalText(s.env, "caught"), "no");
  EXPECT_EQ(GlobalText(s.env, "caughtError"), "none");
  EXPECT_EQ(RunScript(s.env, "globalThis.resumed = 'yes';"), napi_ok);
}

// The promise hooks return nothing too: the same holds for them, in the run
// that made the promise, or in the microtask checkpoint that runs its
// reactions.
TEST_F(Test75HooksKeepTermination, PromiseHooks) {
  constexpr const char* kHookNames[] = {"init", "before", "after", "resolve"};
  for (int which = 0; which < 4; ++which) {
    SCOPED_TRACE(kHookNames[which]);
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    napi_value hooks[4] = {nullptr, nullptr, nullptr, nullptr};
    hooks[which] = Eval(s.env,
                        "globalThis.armed = false;"
                        "(...args) => { if (armed) { terminate(); tick(); } }");
    ASSERT_EQ(unofficial_napi_set_promise_hooks(s.env, hooks[0], hooks[1], hooks[2], hooks[3]),
              napi_ok);
    const napi_status run = RunScript(
        s.env,
        "try {\n"
        "  armed = true;\n"
        "  Promise.resolve(1).then(() => {\n"
        "    try { tick(); } catch (e) { innerCaught = 'yes'; caughtError = e; }\n"
        "  });\n"
        "} catch (e) { caught = 'yes'; caughtError = e; }\n"
        "after = 'yes';\n");
    const napi_status checkpoint = unofficial_napi_event_loop_checkpoint(
        s.env, unofficial_napi_event_loop_checkpoint_microtasks, false, nullptr);
    const napi_status next = RunScript(s.env, "globalThis.next = 'yes';");
    FinishTermination(s.env);
    EXPECT_TRUE(run == napi_pending_exception || checkpoint == napi_pending_exception ||
                next == napi_pending_exception)
        << "run " << run << ", checkpoint " << checkpoint << ", next run " << next;
    EXPECT_EQ(GlobalText(s.env, "caught"), "no");
    EXPECT_EQ(GlobalText(s.env, "innerCaught"), "no");
    EXPECT_EQ(GlobalText(s.env, "caughtError"), "none");
    ASSERT_EQ(RunScript(s.env, "armed = false;"), napi_ok);
  }
}

// Errors the providers make with a code get it set as Node sets it, which runs
// an inherited `code` setter. When V8 terminated the setter, V8's
// napi_throw_error and its siblings, the serdes binding's constructors and
// the module loader's link errors threw their error over the termination.
TEST_F(Test75HooksKeepTermination, ErrorCodeSetter) {
  constexpr char kCodeSetter[] =
      "Object.defineProperty(Error.prototype, 'code',"
      "  { set(value) { terminate(); tick(); }, configurable: true });";
  for (int kind = 0; kind < 4; ++kind) {
    SCOPED_TRACE(kind);
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    ASSERT_EQ(RunScript(s.env, kCodeSetter), napi_ok);
    const std::string script = "try { throwWithCode(" + std::to_string(kind) +
                               "); } catch (e) { caught = 'yes'; caughtError = e; }\n"
                               "after = 'yes';\n";
    ExpectRunTerminated(s.env, script.c_str());
  }
  {
    SCOPED_TRACE("serdes constructor without new");
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    napi_value binding = nullptr;
    ASSERT_EQ(unofficial_napi_create_serdes_binding(s.env, &binding), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, Global(s.env), "serdes", binding), napi_ok);
    ASSERT_EQ(RunScript(s.env, kCodeSetter), napi_ok);
    ExpectRunTerminated(
        s.env,
        "try { serdes.Serializer(); } catch (e) { caught = 'yes'; caughtError = e; }\n"
        "after = 'yes';\n");
  }
#if defined(NAPI_TEST_ENGINE_V8)
  // QuickJS reports a module that is not linked through its own loader, a
  // ReferenceError with no code, so no setter runs there.
  {
    SCOPED_TRACE("instantiating a module that is not linked");
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    napi_value wrapper = nullptr;
    napi_value undefined = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &wrapper), napi_ok);
    ASSERT_EQ(napi_get_undefined(s.env, &undefined), napi_ok);
    const unofficial_napi_js_source source =
        unofficial_napi_js_source_from_text(Str(s.env, "import 'dep';"));
    unofficial_napi_module_create_options options{};
    options.size = sizeof(options);
    options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
    options.kind = unofficial_napi_module_source_text;
    options.wrapper = wrapper;
    options.url = Str(s.env, "unlinked.mjs");
    options.payload.source_text.source = &source;
    options.payload.source_text.host_defined_option_id = undefined;
    unofficial_napi_module_create_result created{};
    ASSERT_EQ(unofficial_napi_module_wrap_create(s.env, &options, &created), napi_ok);
    DefineGlobalFunction(s.env, "instantiateModule", InstantiateModule, created.module);
    ASSERT_EQ(RunScript(s.env, kCodeSetter), napi_ok);
    ExpectRunTerminated(
        s.env,
        "try { instantiateModule(); } catch (e) { caught = 'yes'; caughtError = e; }\n"
        "after = 'yes';\n");
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, created.module), napi_ok);
  }
#endif
}

#if defined(NAPI_TEST_ENGINE_V8)
// The prepare-stack-trace callback; QuickJS ignores the one set here.
TEST_F(Test75HooksKeepTermination, PrepareStackTraceCallback) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  ASSERT_EQ(unofficial_napi_set_prepare_stack_trace_callback(
                s.env, Eval(s.env, "(global, error, trace) => { terminate(); tick(); }")),
            napi_ok);
  ExpectRunTerminated(
      s.env,
      "try { new Error('x').stack; } catch (e) { caught = 'yes'; caughtError = e; }\n"
      "after = 'yes';\n");
}

// What a module hook threw is formatted as it is taken from N-API, which calls
// the source-map callback; only V8's provider calls that callback. A hook that
// asks for the termination and throws before V8 acts on it has V8 act on it in
// that callback, the first JS to run after. V8 still threw the hook's error
// over the termination, and the module caught it.
TEST_F(Test75HooksKeepTermination, SourceMapCallbackWhileTakingAHookError) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  ASSERT_EQ(RunScript(s.env, "globalThis.mapped = 0;"), napi_ok);
  ASSERT_EQ(unofficial_napi_configure_source_maps(
                s.env, true, Eval(s.env, "(name, line, column) => { mapped++; }")),
            napi_ok);
  // The hook comes from a named script, so its error's message names one.
  const unofficial_napi_js_source hook_source = unofficial_napi_js_source_from_text(
      Str(s.env, "(id, meta, wrap) => { terminate(); throw new Error('meta'); }"));
  napi_value hook = nullptr;
  ASSERT_EQ(unofficial_napi_run_script(s.env, nullptr, &hook_source, Str(s.env, "hook.js"), 0, 0,
                                       -1, true, false, false, nullptr, &hook),
            napi_ok);
  SetModuleHooks(s.env, nullptr, hook);
  unofficial_napi_module module = MakeModule(
      s.env,
      "try { import.meta; } catch (e) { globalThis.innerCaught = 'yes'; caughtError = e; }\n"
      "globalThis.innerAfter = 'yes';\n",
      nullptr);
  ASSERT_NE(module, nullptr);
  DefineGlobalFunction(s.env, "evaluateModule", EvaluateModule, module);
  ExpectRunTerminated(s.env, kEvaluateModuleInTryCatch);
  // V8 acted on the termination as it entered the callback, which never ran.
  EXPECT_EQ(GlobalText(s.env, "mapped"), "0");
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);
}

// import() with no module hooks calls the `__napi_dynamic_import` helper; only
// V8 does. V8 rejected the import()'s promise with the null its TryCatch
// records for the termination and returned it: the JS that imported ran on,
// and the termination was gone once the run returned.
TEST_F(Test75HooksKeepTermination, DefaultDynamicImportHelper) {
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

// The serializer's and deserializer's hooks; only V8's serdes binding calls
// them (QuickJS's writes and reads values itself). A `_getDataCloneError`
// getter V8 terminated made V8 throw the clone error over the termination,
// and the JS that wrote the value caught it.
TEST_F(Test75HooksKeepTermination, SerdesHooks) {
  for (const char* script : {
           "const s = new serdes.Serializer();\n"
           "Object.defineProperty(s, '_getDataCloneError', { get() { terminate(); tick(); } });\n"
           "try { s.writeValue(() => {}); } catch (e) { caught = 'yes'; caughtError = e; }\n"
           "after = 'yes';\n",
           "const s = new serdes.Serializer();\n"
           "s._getDataCloneError = (message) => { terminate(); tick(); };\n"
           "try { s.writeValue(() => {}); } catch (e) { caught = 'yes'; caughtError = e; }\n"
           "after = 'yes';\n",
           "const s = new serdes.Serializer();\n"
           "s._setTreatArrayBufferViewsAsHostObjects(true);\n"
           "s._writeHostObject = (value) => { terminate(); tick(); };\n"
           "try { s.writeValue(new Uint8Array(2)); } catch (e) { caught = 'yes'; caughtError = e; }\n"
           "after = 'yes';\n",
           "const s = new serdes.Serializer();\n"
           "Object.defineProperty(s, '_getSharedArrayBufferId',\n"
           "  { get() { terminate(); tick(); } });\n"
           "try { s.writeValue(new SharedArrayBuffer(2)); }\n"
           "catch (e) { caught = 'yes'; caughtError = e; }\n"
           "after = 'yes';\n",
           "const s = new serdes.Serializer();\n"
           "s._getSharedArrayBufferId = (buffer) => { terminate(); tick(); };\n"
           "try { s.writeValue(new SharedArrayBuffer(2)); }\n"
           "catch (e) { caught = 'yes'; caughtError = e; }\n"
           "after = 'yes';\n",
           "const s = new serdes.Serializer();\n"
           "s._setTreatArrayBufferViewsAsHostObjects(true);\n"
           "s._writeHostObject = function (value) { this.writeUint32(7); };\n"
           "s.writeHeader();\n"
           "s.writeValue(new Uint8Array(2));\n"
           "const d = new serdes.Deserializer(s.releaseBuffer());\n"
           "d._readHostObject = () => { terminate(); tick(); };\n"
           "d.readHeader();\n"
           "try { d.readValue(); } catch (e) { caught = 'yes'; caughtError = e; }\n"
           "after = 'yes';\n",
       }) {
    SCOPED_TRACE(script);
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    napi_value binding = nullptr;
    ASSERT_EQ(unofficial_napi_create_serdes_binding(s.env, &binding), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, Global(s.env), "serdes", binding), napi_ok);
    const std::string wrapped = std::string("(() => {\n") + script + "})();";
    ExpectRunTerminated(s.env, wrapped.c_str());
  }
}
#endif
