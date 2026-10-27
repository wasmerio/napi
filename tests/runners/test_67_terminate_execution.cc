// unofficial_napi_terminate_execution / unofficial_napi_cancel_terminate_execution.
//
// EdgeJS implements process.exit() and worker.terminate() with these: the exit
// handler terminates the env, the JS stack unwinds without running catch or
// finally blocks, and the embedder takes the pending exception and cancels the
// termination before running its exit callbacks.

#include "test_env.h"

#include "unofficial_napi.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

class Test67TerminateExecution : public FixtureTestBase {};

namespace {

napi_value Terminate(napi_env env, napi_callback_info /*info*/) {
  EXPECT_EQ(unofficial_napi_terminate_execution(env), napi_ok);
  return nullptr;
}

// Calls its argument, then clears whatever exception that left pending and
// returns normally, like native code that ignores a failed call.
napi_status g_swallowed_call_status = napi_ok;
napi_value CallAndSwallow(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value fn = nullptr;
  EXPECT_EQ(napi_get_cb_info(env, info, &argc, &fn, nullptr, nullptr), napi_ok);
  napi_value global = nullptr;
  EXPECT_EQ(napi_get_global(env, &global), napi_ok);
  napi_value ignored = nullptr;
  g_swallowed_call_status = napi_call_function(env, global, fn, 0, nullptr, &ignored);
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    napi_value exception = nullptr;
    (void)napi_get_and_clear_last_exception(env, &exception);
  }
  return nullptr;
}

std::atomic<bool> g_loop_started{false};
napi_value Started(napi_env /*env*/, napi_callback_info /*info*/) {
  g_loop_started.store(true);
  return nullptr;
}

void DefineGlobalFunction(napi_env env, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn), napi_ok);
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(env, global, name, fn), napi_ok);
}

napi_status RunScript(napi_env env, const char* source, napi_value* result = nullptr) {
  napi_value script = nullptr;
  napi_status status = napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &script);
  if (status != napi_ok) return status;
  napi_value ignored = nullptr;
  return napi_run_script(env, script, result != nullptr ? result : &ignored);
}

std::string RunScriptToString(napi_env env, const char* source) {
  napi_value result = nullptr;
  EXPECT_EQ(RunScript(env, source, &result), napi_ok);
  if (result == nullptr) return "<no result>";
  napi_value as_string = nullptr;
  EXPECT_EQ(napi_coerce_to_string(env, result, &as_string), napi_ok);
  char buffer[256] = {};
  size_t length = 0;
  EXPECT_EQ(napi_get_value_string_utf8(env, as_string, buffer, sizeof(buffer), &length), napi_ok);
  return std::string(buffer, length);
}

// What EdgeJS does once the stack has unwound: take the pending exception,
// then cancel the termination.
void FinishTermination(napi_env env) {
  bool pending = false;
  ASSERT_EQ(napi_is_exception_pending(env, &pending), napi_ok);
  EXPECT_TRUE(pending);
  if (pending) {
    napi_value exception = nullptr;
    EXPECT_EQ(napi_get_and_clear_last_exception(env, &exception), napi_ok);
  }
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(env), napi_ok);
}

void InstallHelpers(napi_env env) {
  DefineGlobalFunction(env, "terminate", Terminate);
  DefineGlobalFunction(env, "callAndSwallow", CallAndSwallow);
  DefineGlobalFunction(env, "started", Started);
  ASSERT_EQ(RunScript(env, "globalThis.log = []; globalThis.mark = (name) => { log.push(name); };"),
            napi_ok);
}

std::string Log(napi_env env) {
  return RunScriptToString(env, "log.join(',')");
}

}  // namespace

TEST_F(Test67TerminateExecution, RejectsNullEnv) {
  EXPECT_EQ(unofficial_napi_terminate_execution(nullptr), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_cancel_terminate_execution(nullptr), napi_invalid_arg);
}

// The process.exit() shape: a native callback terminates the env it runs in.
TEST_F(Test67TerminateExecution, TerminateFromCallbackStopsTheScript) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);

  EXPECT_EQ(RunScript(s.env, "mark('before'); terminate(); mark('after');"),
            napi_pending_exception);
  FinishTermination(s.env);

  // Cancelling leaves the env usable.
  EXPECT_EQ(Log(s.env), "before");
  EXPECT_EQ(RunScript(s.env, "mark('resumed');"), napi_ok);
  EXPECT_EQ(Log(s.env), "before,resumed");
}

TEST_F(Test67TerminateExecution, CatchAndFinallyBlocksDoNotRun) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);

  EXPECT_EQ(RunScript(s.env,
                      "function inner() {\n"
                      "  try { mark('inner'); terminate(); mark('inner-after'); }\n"
                      "  catch (e) { mark('inner-catch'); }\n"
                      "  finally { mark('inner-finally'); }\n"
                      "}\n"
                      "try { inner(); mark('outer-after'); }\n"
                      "catch (e) { mark('outer-catch'); }\n"
                      "finally { mark('outer-finally'); }\n"
                      "mark('after');\n"),
            napi_pending_exception);
  FinishTermination(s.env);
  EXPECT_EQ(Log(s.env), "inner");
}

// The worker.terminate() shape: another thread stops JS that never calls
// back into native code.
TEST_F(Test67TerminateExecution, TerminateFromAnotherThreadStopsABusyLoop) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  g_loop_started.store(false);

  std::atomic<bool> loop_left{false};
  napi_env env = s.env;
  std::thread terminator([env, &loop_left]() {
    while (!g_loop_started.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(unofficial_napi_terminate_execution(env), napi_ok);
    // Nothing else can stop the loop, so fail loudly rather than hang.
    for (int i = 0; i < 10000 && !loop_left.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!loop_left.load()) {
      std::fprintf(stderr, "terminate_execution did not stop while (true) {}\n");
      std::_Exit(1);
    }
  });

  napi_status status = RunScript(s.env,
                                 "try { started(); while (true) {} }\n"
                                 "catch (e) { mark('catch'); }\n"
                                 "finally { mark('finally'); }\n"
                                 "mark('after');\n");
  loop_left.store(true);
  terminator.join();

  EXPECT_EQ(status, napi_pending_exception);
  FinishTermination(s.env);
  EXPECT_EQ(Log(s.env), "");
  EXPECT_EQ(RunScriptToString(s.env, "1 + 1"), "2");
}

// Native code between two JS frames cannot swallow a termination: the outer
// JS must not resume once the native callback returns.
TEST_F(Test67TerminateExecution, NativeCodeCannotSwallowTheTermination) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);
  g_swallowed_call_status = napi_ok;

  EXPECT_EQ(RunScript(s.env,
                      "try { callAndSwallow(() => { terminate(); mark('inner-after'); }); "
                      "mark('outer-after'); }\n"
                      "catch (e) { mark('catch'); }\n"),
            napi_pending_exception);
  EXPECT_EQ(g_swallowed_call_status, napi_pending_exception);
  FinishTermination(s.env);
  EXPECT_EQ(Log(s.env), "");
}

// A termination requested while no JS runs stops the next call into JS.
TEST_F(Test67TerminateExecution, TerminateBeforeACallStopsThatCall) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  napi_value mark = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, global, "mark", &mark), napi_ok);
  napi_value name = nullptr;
  ASSERT_EQ(napi_create_string_utf8(s.env, "called", NAPI_AUTO_LENGTH, &name), napi_ok);

  ASSERT_EQ(unofficial_napi_terminate_execution(s.env), napi_ok);
  napi_value result = nullptr;
  EXPECT_EQ(napi_call_function(s.env, global, mark, 1, &name, &result), napi_pending_exception);
  FinishTermination(s.env);
  EXPECT_EQ(Log(s.env), "");

  EXPECT_EQ(napi_call_function(s.env, global, mark, 1, &name, &result), napi_ok);
  EXPECT_EQ(Log(s.env), "called");
}

#if defined(NAPI_TEST_ENGINE_QUICKJS)
// V8 acts on a termination at its next stack check, so a plain store right
// after the terminating callback may still run there. QuickJS stops as soon
// as the callback returns.
TEST_F(Test67TerminateExecution, QuickJSStopsAsSoonAsTheCallbackReturns) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);

  EXPECT_EQ(RunScript(s.env, "terminate(); globalThis.reached = true;"), napi_pending_exception);
  FinishTermination(s.env);
  EXPECT_EQ(RunScriptToString(s.env, "typeof globalThis.reached"), "undefined");
}

TEST_F(Test67TerminateExecution, QuickJSTerminatesInsideAMicrotask) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);

  EXPECT_EQ(RunScript(s.env,
                      "(async () => {\n"
                      "  await null;\n"
                      "  mark('resumed');\n"
                      "  try { terminate(); mark('after'); } finally { mark('finally'); }\n"
                      "})();\n"
                      "Promise.resolve().then(() => mark('next-microtask'));\n"
                      "mark('sync');\n"),
            napi_ok);
  uint32_t state = 0;
  EXPECT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, false, &state),
            napi_pending_exception);
  FinishTermination(s.env);
  EXPECT_EQ(Log(s.env), "sync,resumed");

  // Microtasks queued before the termination still run once it is cancelled.
  EXPECT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, false, &state),
            napi_ok);
  EXPECT_EQ(Log(s.env), "sync,resumed,next-microtask");
}
#endif
