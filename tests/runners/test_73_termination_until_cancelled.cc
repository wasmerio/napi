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

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

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

napi_value Terminate(napi_env env, napi_callback_info /*info*/) {
  EXPECT_EQ(unofficial_napi_terminate_execution(env), napi_ok);
  return nullptr;
}

napi_value Noop(napi_env /*env*/, napi_callback_info /*info*/) { return nullptr; }

// Terminates and calls tick(), as native code that terminates and goes on
// does. The termination fails the call; V8 acts on it when the native
// function returns.
void TerminateNow(napi_env env) {
  EXPECT_EQ(unofficial_napi_terminate_execution(env), napi_ok);
  napi_value tick = nullptr;
  napi_value ignored = nullptr;
  napi_get_named_property(env, Global(env), "tick", &tick);
  napi_call_function(env, Global(env), tick, 0, nullptr, &ignored);
}

// Calls terminateJs(), JS that terminates, then cancels, without taking
// what the call, which the termination stopped inside JS, left pending: on
// V8 the null its TryCatch recorded.
napi_value TerminateThenCancel(napi_env env, napi_callback_info /*info*/) {
  napi_value terminate_js = nullptr;
  napi_value ignored = nullptr;
  EXPECT_EQ(napi_get_named_property(env, Global(env), "terminateJs", &terminate_js), napi_ok);
  EXPECT_EQ(napi_call_function(env, Global(env), terminate_js, 0, nullptr, &ignored),
            napi_pending_exception);
  EXPECT_EQ(unofficial_napi_cancel_terminate_execution(env), napi_ok);
  return nullptr;
}

// Terminates, then sleeps long enough for a 50 ms script timeout to expire
// meanwhile.
napi_value TerminateAndSleep(napi_env env, napi_callback_info /*info*/) {
  EXPECT_EQ(unofficial_napi_terminate_execution(env), napi_ok);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  return nullptr;
}

// The status of the N-API call a native function below made last.
napi_status g_status = napi_ok;

// Runs a script with a 50 ms timeout that terminates while the timeout
// expires.
napi_value RunScriptWithTimeout(napi_env env, napi_callback_info /*info*/) {
  const unofficial_napi_js_source source = unofficial_napi_js_source_from_text(
      Str(env, "terminateAndSleep(); tick(); afterCall = 'yes';"));
  napi_value out = nullptr;
  g_status = unofficial_napi_run_script(env, nullptr, &source, Str(env, "timeout.js"), 0, 0, 50,
                                        true, false, false, nullptr, &out);
  return nullptr;
}

// N-API calls native code may make after a call into JS failed because of
// a termination, ignoring the failure. Those that enter V8 would have it
// forget the termination; those that run JS set afterCall.
constexpr const char* kAfterCalls[] = {
    "napi_create_function",
    "napi_create_promise",
    "napi_create_date",
    "napi_define_class",
    "napi_create_buffer",
    "unofficial_napi_create_private_symbol",
    "unofficial_napi_compile_function",
    "unofficial_napi_get_own_non_index_properties",
    "unofficial_napi_create_realm (sandbox)",
    "unofficial_napi_contextify_contains_module_syntax",
    "unofficial_napi_run_script",
    "napi_get_and_clear_last_exception",
};
constexpr int kAfterCallCount = sizeof(kAfterCalls) / sizeof(kAfterCalls[0]);

napi_status CallAfter(napi_env env, int kind) {
  napi_value out = nullptr;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  switch (kind) {
    case 0:
      return napi_create_function(env, "f", NAPI_AUTO_LENGTH, Noop, nullptr, &out);
    case 1: {
      napi_deferred deferred = nullptr;
      return napi_create_promise(env, &deferred, &out);
    }
    case 2:
      return napi_create_date(env, 0, &out);
    case 3:
      return napi_define_class(env, "C", NAPI_AUTO_LENGTH, Noop, nullptr, 0, nullptr, &out);
    case 4: {
      void* data = nullptr;
      return napi_create_buffer(env, 4, &data, &out);
    }
    case 5:
      return unofficial_napi_create_private_symbol(env, "p", NAPI_AUTO_LENGTH, &out);
    case 6: {
      const unofficial_napi_js_source source =
          unofficial_napi_js_source_from_text(Str(env, "return 1;"));
      return unofficial_napi_compile_function(env, nullptr, &source, Str(env, "f.js"), 0, 0,
                                              undefined, undefined, undefined, &out);
    }
    case 7: {
      napi_value object = nullptr;
      napi_create_object(env, &object);
      return unofficial_napi_get_own_non_index_properties(env, object, 0, &out);
    }
    case 8: {
      napi_value sandbox = nullptr;
      napi_create_object(env, &sandbox);
      unofficial_napi_realm_options options;
      unofficial_napi_realm_options_init(&options);
      options.sandbox_or_null = sandbox;
      unofficial_napi_realm realm = nullptr;
      return unofficial_napi_create_realm(env, &options, &realm, &out);
    }
    case 9: {
      bool result = false;
      return unofficial_napi_contextify_contains_module_syntax(
          env, Str(env, "export {}"), Str(env, "m.js"), undefined, false, &result);
    }
    case 10: {
      const unofficial_napi_js_source source =
          unofficial_napi_js_source_from_text(Str(env, "globalThis.afterCall = 'yes';"));
      return unofficial_napi_run_script(env, nullptr, &source, Str(env, "after.js"), 0, 0, -1,
                                        true, false, false, nullptr, &out);
    }
    default: {
      bool pending = false;
      napi_is_exception_pending(env, &pending);
      if (!pending) return napi_ok;
      return napi_get_and_clear_last_exception(env, &out);
    }
  }
}

// Terminates, then makes the N-API call its argument names, as native code
// that ignores the failed call does.
napi_value TerminateThen(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value kind_value = nullptr;
  EXPECT_EQ(napi_get_cb_info(env, info, &argc, &kind_value, nullptr, nullptr), napi_ok);
  int32_t kind = 0;
  EXPECT_EQ(napi_get_value_int32(env, kind_value, &kind), napi_ok);
  TerminateNow(env);
  g_status = CallAfter(env, kind);
  return nullptr;
}

// N-API calls that run JS, given what makes them run it: native code may
// make them after a call into JS failed because of a termination. Each
// payload sets afterCall if its JS runs.
constexpr const char* kJsCalls[] = {
    "unofficial_napi_run_script",
    "unofficial_napi_module_wrap_evaluate",
    "unofficial_napi_module_wrap_evaluate_sync",
    "unofficial_napi_get_own_non_index_properties (proxy)",
    "unofficial_napi_create_realm (proxy sandbox)",
    "unofficial_napi_structured_clone (getter)",
    "unofficial_napi_message_create (getter)",
    "unofficial_napi_compile_function (getter in the extensions)",
    "napi_create_buffer (Buffer getter)",
};
constexpr int kJsCallCount = sizeof(kJsCalls) / sizeof(kJsCalls[0]);

constexpr const char* kJsCallPayloads[] = {
    "undefined",
    "undefined",
    "undefined",
    "new Proxy({}, { ownKeys() { afterCall = 'yes'; return []; } })",
    "new Proxy({}, { ownKeys() { afterCall = 'yes'; return []; } })",
    "({ get x() { afterCall = 'yes'; return 1; } })",
    "({ get x() { afterCall = 'yes'; return 1; } })",
    "(() => { const a = [{}]; Object.defineProperty(a, 0, { get() { afterCall = 'yes'; return {}; } }); return a; })()",
    "(Object.defineProperty(globalThis, 'Buffer', { get() { afterCall = 'yes'; return undefined; }, configurable: true }), undefined)",
};

// The module kJsCalls's evaluations evaluate.
unofficial_napi_module g_module = nullptr;

napi_status CallRunningJs(napi_env env, int kind, napi_value payload) {
  napi_value out = nullptr;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  switch (kind) {
    case 0: {
      const unofficial_napi_js_source source =
          unofficial_napi_js_source_from_text(Str(env, "globalThis.afterCall = 'yes';"));
      return unofficial_napi_run_script(env, nullptr, &source, Str(env, "after.js"), 0, 0, -1,
                                        true, false, false, nullptr, &out);
    }
    case 1:
      return unofficial_napi_module_wrap_evaluate(env, g_module, -1, false, &out);
    case 2:
      return unofficial_napi_module_wrap_evaluate_sync(env, g_module, Str(env, "m.mjs"),
                                                       Str(env, "parent.js"), &out);
    case 3:
      return unofficial_napi_get_own_non_index_properties(env, payload, 0, &out);
    case 4: {
      unofficial_napi_realm_options options;
      unofficial_napi_realm_options_init(&options);
      options.sandbox_or_null = payload;
      unofficial_napi_realm realm = nullptr;
      return unofficial_napi_create_realm(env, &options, &realm, &out);
    }
    case 5:
      return unofficial_napi_structured_clone(env, payload, nullptr, &out);
    case 6: {
      unofficial_napi_message message = nullptr;
      const napi_status status = unofficial_napi_message_create(env, payload, &message);
      if (message != nullptr) unofficial_napi_message_drop(message);
      return status;
    }
    case 7: {
      const unofficial_napi_js_source source =
          unofficial_napi_js_source_from_text(Str(env, "return 1;"));
      return unofficial_napi_compile_function(env, nullptr, &source, Str(env, "f.js"), 0, 0,
                                              payload, undefined, undefined, &out);
    }
    default: {
      void* data = nullptr;
      return napi_create_buffer(env, 4, &data, &out);
    }
  }
}

// Calls its first argument, a JS function that terminates, then makes the
// N-API call its second argument names with its third as the payload, as
// native code that ignores the failed call does.
napi_value CallThen(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  EXPECT_EQ(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr), napi_ok);
  int32_t kind = 0;
  EXPECT_EQ(napi_get_value_int32(env, argv[1], &kind), napi_ok);
  napi_value ignored = nullptr;
  EXPECT_EQ(napi_call_function(env, Global(env), argv[0], 0, nullptr, &ignored),
            napi_pending_exception);
  g_status = CallRunningJs(env, kind, argv[2]);
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

std::string Text(napi_env env, napi_value value) {
  napi_value string = nullptr;
  char text[256] = {};
  size_t length = 0;
  if (value == nullptr || napi_coerce_to_string(env, value, &string) != napi_ok ||
      napi_get_value_string_utf8(env, string, text, sizeof(text), &length) != napi_ok) {
    return "<unreadable>";
  }
  return std::string(text, length);
}

std::string GlobalText(napi_env env, const char* name) {
  napi_value value = nullptr;
  if (napi_get_named_property(env, Global(env), name, &value) != napi_ok) return "<unreadable>";
  return Text(env, value);
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
  DefineGlobalFunction(env, "noop", Noop);
  DefineGlobalFunction(env, "terminateThen", TerminateThen);
  DefineGlobalFunction(env, "callThen", CallThen);
  DefineGlobalFunction(env, "terminateThenCancel", TerminateThenCancel);
  DefineGlobalFunction(env, "terminateAndSleep", TerminateAndSleep);
  DefineGlobalFunction(env, "runScriptWithTimeout", RunScriptWithTimeout);
  // Each catch block sets its flag first, by a plain store, at which no
  // engine looks for a termination.
  ASSERT_EQ(RunScript(env,
                      "globalThis.tick = function tick() {};"
                      "globalThis.terminateJs = () => { terminate(); tick(); };"
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

constexpr char kNothingRan[] =
    "caught=no after=no innerCaught=no innerAfter=no error=none afterCall=no";

// Runs `script` and expects it to come back terminated, with nothing it
// wrapped in try/catch caught, and nothing after that run.
void ExpectRunTerminated(napi_env env, const char* script) {
  EXPECT_EQ(RunScript(env, script), napi_pending_exception);
  EXPECT_TRUE(FinishTermination(env)) << "nothing pending after the run";
  EXPECT_EQ(Flags(env), kNothingRan);
  // Cancelling leaves the env usable.
  EXPECT_EQ(RunScript(env, "globalThis.resumed = 'yes';"), napi_ok);
  EXPECT_EQ(GlobalText(env, "resumed"), "yes");
}

// Runs `script` and expects it, or the next run, to come back terminated,
// with nothing caught: for callbacks the engine cannot report a failure
// from.
void ExpectRunOrNextTerminated(napi_env env, const char* script) {
  const napi_status run = RunScript(env, script);
  const napi_status next = RunScript(env, "globalThis.next = 'yes';");
  FinishTermination(env);
  EXPECT_TRUE(run == napi_pending_exception || next == napi_pending_exception)
      << "run " << run << ", next run " << next;
  EXPECT_EQ(GlobalText(env, "caught"), "no");
  EXPECT_EQ(GlobalText(env, "innerCaught"), "no");
  EXPECT_EQ(GlobalText(env, "caughtError"), "none");
  EXPECT_EQ(RunScript(env, "globalThis.resumed = 'yes';"), napi_ok);
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

// Native code whose call into JS a termination stopped, and which cancels the
// termination before it returns, without taking what that call left pending.
// Nothing threw that: the JS that called goes on as after any call. V8's
// TryCatch records a termination as a null exception, which V8 would
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

// A script run with a timeout that the embedder terminates while the
// timeout expires too: the termination is not the timeout's to cancel, and
// must not come back as the timeout's catchable error. QuickJS has no
// timeout, so only the termination stops the script there.
TEST_F(Test73TerminationUntilCancelled, ScriptTimeoutKeepsTheEmbeddersTermination) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  g_status = napi_ok;
  ExpectRunTerminated(
      s.env,
      "try { runScriptWithTimeout(); } catch (e) { caught = 'yes'; caughtError = e; }\n"
      "after = 'yes';\n");
  EXPECT_EQ(g_status, napi_pending_exception);
}

// The run a termination stopped leaves it in effect: the next run fails too
// until the termination is cancelled. V8 forgets a termination once the
// outermost JS frame has unwound.
TEST_F(Test73TerminationUntilCancelled, TopLevelRunKeepsTheTerminationUntilCancelled) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  napi_value mark = Eval(s.env, "() => { afterCall = 'yes'; }");
  EXPECT_EQ(RunScript(s.env, "terminate(); tick();"), napi_pending_exception);
  EXPECT_EQ(RunScript(s.env, "afterCall = 'yes';"), napi_pending_exception);
  napi_value result = nullptr;
  EXPECT_EQ(napi_call_function(s.env, Global(s.env), mark, 0, nullptr, &result),
            napi_pending_exception);
  EXPECT_TRUE(FinishTermination(s.env));
  EXPECT_EQ(GlobalText(s.env, "afterCall"), "no");
  EXPECT_EQ(RunScript(s.env, "afterCall = 'yes';"), napi_ok);
}

// Native code that goes on with N-API after its call into JS failed because
// of the termination. On V8 an N-API call that enters V8 clears the
// termination, and the trampoline then threw the null the TryCatch recorded
// for it, which the JS caught.
TEST_F(Test73TerminationUntilCancelled, NativeCallsAfterTheTermination) {
  for (int kind = 0; kind < kAfterCallCount; ++kind) {
    SCOPED_TRACE(kAfterCalls[kind]);
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    const std::string script = "try { terminateThen(" + std::to_string(kind) +
                               "); } catch (e) { caught = 'yes'; caughtError = e; }\n"
                               "after = 'yes';\n";
    ExpectRunTerminated(s.env, script.c_str());
  }
}

// Native code whose call into JS a termination failed, and which then makes
// an N-API call that runs JS. On V8 the termination is in effect when that
// call enters V8, which clears it and runs the JS.
TEST_F(Test73TerminationUntilCancelled, CallsRunningJsAfterTheTermination) {
  for (int kind = 0; kind < kJsCallCount; ++kind) {
    SCOPED_TRACE(kJsCalls[kind]);
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    g_module = MakeModule(s.env, "globalThis.afterCall = 'yes';", nullptr);
    ASSERT_NE(g_module, nullptr);
    g_status = napi_ok;
    const std::string script =
        std::string("const payload = ") + kJsCallPayloads[kind] + ";\n" +
        "try { callThen(() => { terminate(); tick(); }, " + std::to_string(kind) +
        ", payload); } catch (e) { caught = 'yes'; caughtError = e; }\n"
        "after = 'yes';\n";
    ExpectRunTerminated(s.env, script.c_str());
    EXPECT_EQ(g_status, napi_pending_exception);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, g_module), napi_ok);
  }
}

// N-API calls that run JS a termination stops, made by the embedder outside
// any JS: they must fail with napi_pending_exception and leave the
// termination pending, as napi_run_script does. Each payload's JS calls
// terminateJs(). Only V8's create_realm reads a sandbox's own keys (to keep
// them), so only there does a proxy sandbox run JS.
constexpr const char* kStoppedCalls[] = {
    "unofficial_napi_run_script",
    "unofficial_napi_module_wrap_evaluate",
    "unofficial_napi_module_wrap_evaluate_sync",
    "unofficial_napi_get_own_non_index_properties (proxy)",
#if defined(NAPI_TEST_ENGINE_V8)
    "unofficial_napi_create_realm (proxy sandbox)",
#endif
};
constexpr int kStoppedCallCount = sizeof(kStoppedCalls) / sizeof(kStoppedCalls[0]);

TEST_F(Test73TerminationUntilCancelled, CallsTheTerminationStopsLeaveItPending) {
  for (int kind = 0; kind < kStoppedCallCount; ++kind) {
    SCOPED_TRACE(kStoppedCalls[kind]);
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    unofficial_napi_module module = MakeModule(s.env, "terminateJs();", nullptr);
    ASSERT_NE(module, nullptr);
    napi_value proxy =
        Eval(s.env, "new Proxy({}, { ownKeys() { terminateJs(); afterCall = 'yes'; return []; } })");
    napi_value out = nullptr;
    napi_status status = napi_ok;
    switch (kind) {
      case 0: {
        const unofficial_napi_js_source source =
            unofficial_napi_js_source_from_text(Str(s.env, "terminateJs(); afterCall = 'yes';"));
        status = unofficial_napi_run_script(s.env, nullptr, &source, Str(s.env, "stopped.js"), 0,
                                            0, -1, true, false, false, nullptr, &out);
        break;
      }
      case 1:
        status = unofficial_napi_module_wrap_evaluate(s.env, module, -1, false, &out);
        break;
      case 2:
        status = unofficial_napi_module_wrap_evaluate_sync(s.env, module, Str(s.env, "m.mjs"),
                                                           Str(s.env, "parent.js"), &out);
        break;
      case 3:
        status = unofficial_napi_get_own_non_index_properties(s.env, proxy, 0, &out);
        break;
      default: {
        unofficial_napi_realm_options options;
        unofficial_napi_realm_options_init(&options);
        options.sandbox_or_null = proxy;
        unofficial_napi_realm realm = nullptr;
        status = unofficial_napi_create_realm(s.env, &options, &realm, &out);
        break;
      }
    }
    EXPECT_EQ(status, napi_pending_exception);
    bool pending = false;
    EXPECT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
    EXPECT_TRUE(pending) << "nothing pending after the call";
    // Cancelling without taking it drops it: it stands for the termination.
    EXPECT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
    EXPECT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
    EXPECT_FALSE(pending) << "the termination is still pending after the cancel";
    EXPECT_EQ(Flags(s.env), kNothingRan);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);
  }
}

// The same native code, called from the module hooks test_72 covers: those
// see what the native function threw.
TEST_F(Test73TerminationUntilCancelled, NativeCallsAfterTheTerminationInsideModuleHooks) {
  {
    SCOPED_TRACE("import.meta");
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    SetModuleHooks(s.env, nullptr, Eval(s.env, "(id, meta, wrap) => { terminateThen(0); }"));
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
  {
    SCOPED_TRACE("synthetic steps");
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    unofficial_napi_module module =
        MakeModule(s.env, nullptr, Eval(s.env, "(function () { terminateThen(1); })"));
    ASSERT_NE(module, nullptr);
    DefineGlobalFunction(s.env, "evaluateModule", EvaluateModule, module);
    ExpectRunTerminated(s.env, kEvaluateModuleInTryCatch);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);
  }
  {
    SCOPED_TRACE("import()");
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    SetModuleHooks(
        s.env,
        Eval(s.env, "(id, specifier, phase, attributes, name) => { terminateThen(2); }"),
        nullptr);
    ExpectRunTerminated(
        s.env,
        "try { import('x').catch(() => {}); } catch (e) { caught = 'yes'; caughtError = e; }\n"
        "after = 'yes';\n");
  }
  {
    SCOPED_TRACE("promise reject callback");
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    ASSERT_EQ(unofficial_napi_set_promise_reject_callback(
                  s.env, Eval(s.env, "(type, promise, reason) => { terminateThen(3); }")),
              napi_ok);
    ExpectRunOrNextTerminated(
        s.env,
        "try { Promise.reject(1); tick(); } catch (e) { caught = 'yes'; caughtError = e; }\n"
        "after = 'yes';\n");
  }
}

// A finalizer whose JS is terminated. Nothing reports a failure from a
// finalizer: its next call into JS must fail, and the checkpoint and the
// next run.
namespace {
napi_status g_finalizer_first = napi_ok;
napi_status g_finalizer_second = napi_ok;
bool g_finalizer_ran = false;
void TerminatingFinalizer(napi_env env, void* /*data*/, void* /*hint*/) {
  g_finalizer_ran = true;
  napi_value terminate_js = nullptr;
  napi_value mark = nullptr;
  napi_value ignored = nullptr;
  napi_get_named_property(env, Global(env), "terminateJs", &terminate_js);
  napi_get_named_property(env, Global(env), "markAfterCall", &mark);
  g_finalizer_first = napi_call_function(env, Global(env), terminate_js, 0, nullptr, &ignored);
  g_finalizer_second = napi_call_function(env, Global(env), mark, 0, nullptr, &ignored);
}
}  // namespace

TEST_F(Test73TerminationUntilCancelled, FinalizerDrainedAtACheckpoint) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  ASSERT_EQ(RunScript(s.env, "globalThis.markAfterCall = () => { afterCall = 'yes'; };"),
            napi_ok);
  g_finalizer_ran = false;
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value object = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);
    ASSERT_EQ(napi_add_finalizer(s.env, object, nullptr, TerminatingFinalizer, nullptr, nullptr),
              napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }
  // V8 runs the finalizer at the checkpoint; QuickJS when it frees the
  // object, before. Either way the checkpoint after it fails.
  napi_status checkpoint = napi_ok;
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
    checkpoint = unofficial_napi_event_loop_checkpoint(
        s.env, unofficial_napi_event_loop_checkpoint_microtasks, false, nullptr);
    if (g_finalizer_ran) break;
  }
  ASSERT_TRUE(g_finalizer_ran);
  EXPECT_EQ(g_finalizer_first, napi_pending_exception);
  EXPECT_EQ(g_finalizer_second, napi_pending_exception);
  EXPECT_EQ(checkpoint, napi_pending_exception);
  EXPECT_EQ(RunScript(s.env, "afterCall = 'yes';"), napi_pending_exception);
  FinishTermination(s.env);
  EXPECT_EQ(GlobalText(s.env, "afterCall"), "no");
}

// A FinalizationRegistry cleanup callback, which the checkpoint runs (V8:
// as a foreground task it pumps; QuickJS: as a job), and whose JS is
// terminated: the checkpoint must fail.
namespace {
bool g_cleanup_ran = false;
napi_value MarkCleanup(napi_env /*env*/, napi_callback_info /*info*/) {
  g_cleanup_ran = true;
  return nullptr;
}
}  // namespace

TEST_F(Test73TerminationUntilCancelled, FinalizationRegistryCleanupAtACheckpoint) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  DefineGlobalFunction(s.env, "markCleanup", MarkCleanup);
  ASSERT_EQ(RunScript(s.env,
                      "globalThis.registry = new FinalizationRegistry(() => {\n"
                      "  markCleanup(); terminate(); tick(); afterCall = 'yes';\n"
                      "});\n"
                      "(() => { registry.register({}, 1); })();\n"),
            napi_ok);
  g_cleanup_ran = false;
  napi_status checkpoint = napi_ok;
  for (int i = 0; i < 20 && !g_cleanup_ran; ++i) {
    ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
    checkpoint = unofficial_napi_event_loop_checkpoint(
        s.env, unofficial_napi_event_loop_checkpoint_host_tasks, true, nullptr);
  }
  ASSERT_TRUE(g_cleanup_ran);
  EXPECT_EQ(checkpoint, napi_pending_exception);
  EXPECT_EQ(RunScript(s.env, "afterCall = 'yes';"), napi_pending_exception);
  FinishTermination(s.env);
  EXPECT_EQ(GlobalText(s.env, "afterCall"), "no");
}

// A native function that terminates and then runs a checkpoint with a
// microtask queued: the checkpoint runs no JS.
namespace {
napi_value TerminateThenCheckpoint(napi_env env, napi_callback_info /*info*/) {
  TerminateNow(env);
  g_status = unofficial_napi_event_loop_checkpoint(
      env, unofficial_napi_event_loop_checkpoint_microtasks, false, nullptr);
  return nullptr;
}
}  // namespace

TEST_F(Test73TerminationUntilCancelled, CheckpointAfterTheTermination) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  DefineGlobalFunction(s.env, "terminateThenCheckpoint", TerminateThenCheckpoint);
  g_status = napi_ok;
  ExpectRunTerminated(
      s.env,
      "Promise.resolve().then(() => { afterCall = 'yes'; });\n"
      "try { terminateThenCheckpoint(); } catch (e) { caught = 'yes'; caughtError = e; }\n"
      "after = 'yes';\n");
  EXPECT_EQ(g_status, napi_pending_exception);
}

// A cleanup hook that terminates while the env is released: the release
// completes, and the hooks and finalizers after it run no JS.
namespace {
std::vector<napi_status> g_release_runs;
void TerminatingCleanupHook(void* arg) {
  napi_env env = static_cast<napi_env>(arg);
  napi_value fn = nullptr;
  napi_value ignored = nullptr;
  napi_get_named_property(env, Global(env), "terminateJs", &fn);
  g_release_runs.push_back(napi_call_function(env, Global(env), fn, 0, nullptr, &ignored));
  napi_value made = nullptr;
  (void)napi_create_function(env, "f", NAPI_AUTO_LENGTH, Noop, nullptr, &made);
  g_release_runs.push_back(napi_run_script(env, Str(env, "afterCall = 'yes'; 1"), &made));
}
void LaterCleanupHook(void* arg) {
  napi_env env = static_cast<napi_env>(arg);
  napi_value result = nullptr;
  g_release_runs.push_back(napi_run_script(env, Str(env, "afterCall = 'yes'; 1"), &result));
}
void ReleaseFinalizer(napi_env env, void* /*data*/, void* /*hint*/) {
  napi_value result = nullptr;
  g_release_runs.push_back(napi_run_script(env, Str(env, "afterCall = 'yes'; 1"), &result));
}
}  // namespace

TEST_F(Test73TerminationUntilCancelled, TerminationRequestedInsideTheRelease) {
  g_release_runs.clear();
  {
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    napi_value object = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);
    napi_ref keep = nullptr;
    ASSERT_EQ(napi_create_reference(s.env, object, 1, &keep), napi_ok);
    ASSERT_EQ(napi_add_finalizer(s.env, object, nullptr, ReleaseFinalizer, nullptr, nullptr),
              napi_ok);
    ASSERT_EQ(napi_add_env_cleanup_hook(s.env, LaterCleanupHook, s.env), napi_ok);
    ASSERT_EQ(napi_add_env_cleanup_hook(s.env, TerminatingCleanupHook, s.env), napi_ok);
  }
  // The terminating hook's call and run, the later hook's run, the
  // finalizer's run.
  EXPECT_EQ(g_release_runs,
            std::vector<napi_status>(4, napi_pending_exception));
}

// The release of an env whose termination was never cancelled, as after
// worker.terminate(). V8's release cancels it and runs the hooks with JS
// usable; QuickJS keeps it and refuses their JS. Either way, the null the
// termination left pending must not be thrown into a hook's JS when it calls
// a native function.
namespace {
std::string g_release_result;
void ReleaseRunsJs(void* arg) {
  napi_env env = static_cast<napi_env>(arg);
  napi_value result = nullptr;
  if (napi_run_script(env,
                      Str(env,
                          "(() => { try { noop(); return 'ok'; }\n"
                          "catch (e) { return 'caught ' + String(e); } })()"),
                      &result) != napi_ok) {
    g_release_result = "refused";
    return;
  }
  g_release_result = Text(env, result);
}
}  // namespace

TEST_F(Test73TerminationUntilCancelled, ReleaseAfterAnUncancelledTermination) {
  g_release_result.clear();
  {
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    ASSERT_EQ(napi_add_env_cleanup_hook(s.env, ReleaseRunsJs, s.env), napi_ok);
    EXPECT_EQ(RunScript(s.env, "terminate(); tick();"), napi_pending_exception);
  }
  EXPECT_TRUE(g_release_result == "ok" || g_release_result == "refused") << g_release_result;
}

#if defined(NAPI_TEST_ENGINE_V8)
// The same native code, called from the hooks only V8 runs: the
// prepare-stack-trace callback (QuickJS never calls it), the serializer's
// host-object hook (QuickJS's serdes binding calls none of the serializer's
// hooks) and the deprecated context-token assign hook (only V8 accepts the
// context-token hooks).
TEST_F(Test73TerminationUntilCancelled, NativeCallsAfterTheTerminationInsideV8Hooks) {
  {
    SCOPED_TRACE("prepare-stack-trace callback");
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    ASSERT_EQ(unofficial_napi_set_prepare_stack_trace_callback(
                  s.env, Eval(s.env, "(global, error, trace) => { terminateThen(0); }")),
              napi_ok);
    ExpectRunTerminated(
        s.env,
        "try { new Error('x').stack; } catch (e) { caught = 'yes'; caughtError = e; }\n"
        "after = 'yes';\n");
  }
  {
    SCOPED_TRACE("serializer host-object hook");
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    napi_value binding = nullptr;
    ASSERT_EQ(unofficial_napi_create_serdes_binding(s.env, &binding), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, Global(s.env), "serdes", binding), napi_ok);
    ExpectRunTerminated(
        s.env,
        "(() => {\n"
        "const s = new serdes.Serializer();\n"
        "s._setTreatArrayBufferViewsAsHostObjects(true);\n"
        "s._writeHostObject = (value) => { terminateThen(0); };\n"
        "try { s.writeValue(new Uint8Array(2)); } catch (e) { caught = 'yes'; caughtError = e; }\n"
        "after = 'yes';\n"
        "})();");
  }
}

namespace {
napi_ref g_token_fn = nullptr;
bool g_token_armed = false;
void TerminatingTokenHook(napi_env env, void* /*token*/, void* /*data*/) {
  if (!g_token_armed) return;
  g_token_armed = false;
  napi_value fn = nullptr;
  napi_get_reference_value(env, g_token_fn, &fn);
  napi_value ignored = nullptr;
  napi_call_function(env, Global(env), fn, 0, nullptr, &ignored);
}
napi_value CreateRealm(napi_env env, napi_callback_info /*info*/) {
  unofficial_napi_realm realm = nullptr;
  napi_value global = nullptr;
  g_status = unofficial_napi_create_realm(env, nullptr, &realm, &global);
  return global;
}
void AttachTokenHooks(napi_env env, bool assign) {
  unofficial_napi_env_hooks hooks{};
  hooks.size = sizeof(hooks);
  hooks.version = UNOFFICIAL_NAPI_ENV_HOOKS_VERSION;
  if (assign) {
    hooks.context_token_assign_callback = TerminatingTokenHook;
  } else {
    hooks.context_token_unassign_callback = TerminatingTokenHook;
  }
  uint64_t accepted = 0;
  ASSERT_EQ(unofficial_napi_attach_env(env, &hooks, &accepted), napi_ok);
}
}  // namespace

napi_value CreateSandboxRealm(napi_env env, napi_callback_info /*info*/) {
  napi_value sandbox = nullptr;
  napi_create_object(env, &sandbox);
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = sandbox;
  unofficial_napi_realm realm = nullptr;
  napi_value global = nullptr;
  g_status = unofficial_napi_create_realm(env, &options, &realm, &global);
  return global;
}

TEST_F(Test73TerminationUntilCancelled, NativeCallsAfterTheTerminationInsideTheTokenAssignHook) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  AttachTokenHooks(s.env, true);
  ASSERT_EQ(napi_create_reference(s.env, Eval(s.env, "(() => { terminateThen(0); })"), 1,
                                  &g_token_fn),
            napi_ok);
  DefineGlobalFunction(s.env, "createRealm", CreateRealm);
  g_token_armed = true;
  ExpectRunTerminated(
      s.env,
      "try { createRealm(); } catch (e) { caught = 'yes'; caughtError = e; }\nafter = 'yes';\n");
  g_token_armed = false;
  napi_delete_reference(s.env, g_token_fn);
}

// unofficial_napi_create_realm first runs the unassign hooks of the realms
// collected since, then makes the realm. When a hook's JS is terminated, it
// must fail without making the realm: the V8 calls that make it would clear
// the termination.
// During the release, the context-token hooks run with what is pending put
// aside, and it is put back after. An unassign hook that cancels the
// termination drops what the termination left pending: put back, the null
// would be thrown into the JS the cleanup hook runs next.
namespace {
bool g_cancel_armed = false;
void CancellingTokenHook(napi_env env, void* /*token*/, void* /*data*/) {
  if (!g_cancel_armed) return;
  g_cancel_armed = false;
  EXPECT_EQ(unofficial_napi_cancel_terminate_execution(env), napi_ok);
}
void CleanupHookCancelledByAnUnassignHook(void* arg) {
  napi_env env = static_cast<napi_env>(arg);
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(env, &scope), napi_ok);
    unofficial_napi_realm realm = nullptr;
    napi_value global = nullptr;
    ASSERT_EQ(unofficial_napi_create_realm(env, nullptr, &realm, &global), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(env, scope), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_terminate_execution(env), napi_ok);
  napi_value result = nullptr;
  EXPECT_EQ(napi_run_script(env, Str(env, "1"), &result), napi_pending_exception);
  g_cancel_armed = true;
  ASSERT_EQ(unofficial_napi_collect_garbage(env), napi_ok);
  EXPECT_FALSE(g_cancel_armed) << "the unassign hook did not run";
  ReleaseRunsJs(env);
}
}  // namespace

TEST_F(Test73TerminationUntilCancelled, UnassignHookCancelsDuringTheRelease) {
  g_release_result.clear();
  {
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    unofficial_napi_env_hooks hooks{};
    hooks.size = sizeof(hooks);
    hooks.version = UNOFFICIAL_NAPI_ENV_HOOKS_VERSION;
    hooks.context_token_unassign_callback = CancellingTokenHook;
    uint64_t accepted = 0;
    ASSERT_EQ(unofficial_napi_attach_env(s.env, &hooks, &accepted), napi_ok);
    ASSERT_EQ(napi_add_env_cleanup_hook(s.env, CleanupHookCancelledByAnUnassignHook, s.env),
              napi_ok);
  }
  EXPECT_EQ(g_release_result, "ok");
}

TEST_F(Test73TerminationUntilCancelled, CreateRealmAfterATerminatedUnassignHook) {
  for (bool with_sandbox : {true, false}) {
    SCOPED_TRACE(with_sandbox ? "with a sandbox" : "with a fresh global");
    EnvScope s(runtime_.get());
    InstallHelpers(s.env);
    AttachTokenHooks(s.env, false);
    ASSERT_EQ(napi_create_reference(s.env, Eval(s.env, "(() => { terminate(); tick(); })"), 1,
                                    &g_token_fn),
              napi_ok);
    {
      napi_handle_scope scope = nullptr;
      ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
      unofficial_napi_realm realm = nullptr;
      napi_value global = nullptr;
      ASSERT_EQ(unofficial_napi_create_realm(s.env, nullptr, &realm, &global), napi_ok);
      ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
    }
    DefineGlobalFunction(s.env, "createRealm", with_sandbox ? CreateSandboxRealm : CreateRealm);
    g_token_armed = true;
    g_status = napi_ok;
    ExpectRunTerminated(
        s.env,
        "gc(); gc();\n"
        "try { createRealm(); } catch (e) { caught = 'yes'; caughtError = e; }\nafter = 'yes';\n");
    EXPECT_FALSE(g_token_armed) << "the unassign hook did not run";
    EXPECT_EQ(g_status, napi_pending_exception);
    g_token_armed = false;
    napi_delete_reference(s.env, g_token_fn);
  }
}
#endif

// An interrupt callback whose JS is terminated, while a run loops: the
// interrupted JS stops. V8 runs the callback at a stack check (the first
// tick()'s entry) and acts on the termination at the next one, so the loop
// body may run once; QuickJS runs the callback inside requestInterrupt().
namespace {
napi_ref g_interrupt_fn = nullptr;
void InterruptCallback(napi_env env, void* /*data*/) {
  napi_value fn = nullptr;
  napi_get_reference_value(env, g_interrupt_fn, &fn);
  napi_value ignored = nullptr;
  napi_call_function(env, Global(env), fn, 0, nullptr, &ignored);
}
napi_value RequestInterrupt(napi_env env, napi_callback_info /*info*/) {
  EXPECT_EQ(unofficial_napi_request_interrupt(env, InterruptCallback, nullptr), napi_ok);
  return nullptr;
}
}  // namespace

TEST_F(Test73TerminationUntilCancelled, InterruptCallbackTerminated) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  ASSERT_EQ(napi_create_reference(s.env, Eval(s.env, "(() => { terminate(); tick(); })"), 1,
                                  &g_interrupt_fn),
            napi_ok);
  DefineGlobalFunction(s.env, "requestInterrupt", RequestInterrupt);
  ASSERT_EQ(RunScript(s.env, "globalThis.calls = 0;"), napi_ok);
  ExpectRunTerminated(
      s.env,
      "try { requestInterrupt(); for (let i = 0; i < 100000; i++) { tick(); calls++; } }\n"
      "catch (e) { caught = 'yes'; caughtError = e; }\n"
      "after = 'yes';\n");
  const std::string calls = GlobalText(s.env, "calls");
  EXPECT_TRUE(calls == "0" || calls == "1") << "calls " << calls;
  napi_delete_reference(s.env, g_interrupt_fn);
}
