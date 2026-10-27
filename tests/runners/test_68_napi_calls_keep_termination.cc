// Regression tests: unofficial_napi_terminate_execution must stop JS even when
// native code keeps calling N-API afterwards.
//
// Like Node's NAPI_PREAMBLE, every N-API call that may run JS must fail while
// the env is terminating, without running it.
//
// In the V8 provider a native callback that kept using N-API used to lose the
// termination in three ways:
// - the callback trampoline rethrew the (null) exception TryCatch records for
//   a termination, which turned it into a catchable `null` and cancelled it;
// - napi_instanceof dropped a failed InstanceOf and returned napi_ok;
// - the next N-API call that entered V8 cleared the pending termination.
// EdgeJS workers then kept running after worker.terminate(), and
// process.exit() waited for them forever.
//
// In the QuickJS provider the termination survived, but property access and
// napi_instanceof still ran user JS (getters, Symbol.hasInstance) before
// QuickJS next polled its interrupt handler.

#include <string>

#include "test_env.h"

class Test68NapiCallsKeepTermination : public FixtureTestBase {};

namespace {

struct NativeResults {
  napi_status call_status = napi_ok;
  napi_status instanceof_status = napi_ok;
  napi_status get_status = napi_ok;
  bool exception_pending = false;
};

NativeResults g_results;

napi_value Global(napi_env env) {
  napi_value global = nullptr;
  EXPECT_EQ(napi_get_global(env, &global), napi_ok);
  return global;
}

// terminateThenCallBack(fn, obj, Ctor): terminates execution, then keeps using
// N-API the way a native binding would.
napi_value TerminateThenCallBack(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  EXPECT_EQ(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr), napi_ok);
  EXPECT_EQ(unofficial_napi_terminate_execution(env), napi_ok);

  napi_value ignored = nullptr;
  g_results.call_status = napi_call_function(env, Global(env), argv[0], 0, nullptr, &ignored);
  bool is_instance = false;
  g_results.instanceof_status = napi_instanceof(env, argv[1], argv[2], &is_instance);
  g_results.get_status = napi_get_named_property(env, Global(env), "marker", &ignored);
  EXPECT_EQ(napi_is_exception_pending(env, &g_results.exception_pending), napi_ok);
  return nullptr;
}

std::string GlobalString(napi_env env, const char* name) {
  napi_value value = nullptr;
  if (napi_get_named_property(env, Global(env), name, &value) != napi_ok) return "<error>";
  napi_value as_string = nullptr;
  if (napi_coerce_to_string(env, value, &as_string) != napi_ok) return "<error>";
  char buffer[128] = {0};
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, as_string, buffer, sizeof(buffer), &copied) != napi_ok) {
    return "<error>";
  }
  return std::string(buffer, copied);
}

}  // namespace

TEST_F(Test68NapiCallsKeepTermination, NativeCallbacksDoNotSwallowTermination) {
  EnvScope s(runtime_.get());
  g_results = NativeResults{};

  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_function(s.env, "terminateThenCallBack", NAPI_AUTO_LENGTH,
                                 TerminateThenCallBack, nullptr, &fn),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, Global(s.env), "terminateThenCallBack", fn), napi_ok);

  napi_value source = nullptr;
  ASSERT_EQ(napi_create_string_utf8(
                s.env,
                "globalThis.calledBack = false;"
                "globalThis.hasInstanceRan = false;"
                "globalThis.caught = 'nothing';"
                "globalThis.after = false;"
                "class Ctor { static [Symbol.hasInstance]() { hasInstanceRan = true; return true; } }"
                "try {"
                "  terminateThenCallBack(() => { calledBack = true; }, {}, Ctor);"
                "} catch (e) {"
                "  caught = String(e);"
                "}"
                "after = true;",
                NAPI_AUTO_LENGTH, &source),
            napi_ok);
  napi_value result = nullptr;
  EXPECT_NE(napi_run_script(s.env, source, &result), napi_ok);

  // Let the env run JS again, then look at what the script managed to do.
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  napi_value exception = nullptr;
  (void)napi_get_and_clear_last_exception(s.env, &exception);

  EXPECT_EQ(g_results.call_status, napi_pending_exception);
  EXPECT_EQ(g_results.instanceof_status, napi_pending_exception);
  EXPECT_EQ(g_results.get_status, napi_pending_exception);
  EXPECT_TRUE(g_results.exception_pending);
  EXPECT_EQ(GlobalString(s.env, "calledBack"), "false");
  EXPECT_EQ(GlobalString(s.env, "hasInstanceRan"), "false");
  // The termination must not surface as a catchable exception, and the rest
  // of the script must not run.
  EXPECT_EQ(GlobalString(s.env, "caught"), "nothing");
  EXPECT_EQ(GlobalString(s.env, "after"), "false");

  // The env keeps working once termination is cancelled.
  napi_value ok_source = nullptr;
  ASSERT_EQ(napi_create_string_utf8(s.env, "after = 'resumed'", NAPI_AUTO_LENGTH, &ok_source), napi_ok);
  EXPECT_EQ(napi_run_script(s.env, ok_source, &result), napi_ok);
  EXPECT_EQ(GlobalString(s.env, "after"), "resumed");
}
