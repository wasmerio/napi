#include "test_env.h"
#include "upstream_js_test.h"

#include <string>

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test35Promise : public FixtureTestBase {};

namespace {

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return {};
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_string) return {};
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return {};
  std::string out(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) {
    return {};
  }
  out.resize(copied);
  return out;
}

napi_value RunScript(napi_env env, const char* source) {
  napi_value script = nullptr;
  EXPECT_EQ(napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &script), napi_ok);
  napi_value result = nullptr;
  EXPECT_EQ(napi_run_script(env, script, &result), napi_ok);
  return result;
}

std::string JsonStringify(napi_env env, const char* expression) {
  std::string source = "JSON.stringify(";
  source += expression;
  source += ")";
  return ValueToUtf8(env, RunScript(env, source.c_str()));
}

}  // namespace

TEST_F(Test35Promise, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  napi_value addon = Init(s.env, exports);
  ASSERT_NE(addon, nullptr);
  ASSERT_TRUE(InstallUpstreamJsShim(s, addon));
  ASSERT_TRUE(
      RunUpstreamJsFile(s, std::string(NAPI_TESTS_ROOT_PATH) + "/js-native-api/test_promise/test.js"));
}

TEST_F(Test35Promise, PromiseHooksObserveLifecycleEvents) {
  EnvScope s(runtime_.get());

  napi_value hooks = RunScript(
      s.env,
      "(() => {"
      "  globalThis.promiseHookEvents = [];"
      "  return ["
      "    (promise, parent) => promiseHookEvents.push(parent === undefined ? 'init:none' : 'init:parent'),"
      "    (promise) => promiseHookEvents.push('before'),"
      "    (promise) => promiseHookEvents.push('after'),"
      "    (promise) => promiseHookEvents.push('resolve')"
      "  ];"
      "})()");
  ASSERT_NE(hooks, nullptr);

  napi_value init = nullptr;
  napi_value before = nullptr;
  napi_value after = nullptr;
  napi_value resolve = nullptr;
  ASSERT_EQ(napi_get_element(s.env, hooks, 0, &init), napi_ok);
  ASSERT_EQ(napi_get_element(s.env, hooks, 1, &before), napi_ok);
  ASSERT_EQ(napi_get_element(s.env, hooks, 2, &after), napi_ok);
  ASSERT_EQ(napi_get_element(s.env, hooks, 3, &resolve), napi_ok);
  ASSERT_EQ(unofficial_napi_set_promise_hooks(s.env, init, before, after, resolve), napi_ok);

  RunScript(
      s.env,
      // Stock QuickJS emits before/after hooks for thenable resolution jobs, not for
      // ordinary already-resolved promise reactions. Keep this test on the hook path
      // the backend can observe instead of asserting V8-only reaction coverage.
      "Promise.resolve({ then(resolve) { resolve('ok'); } })"
      "  .then(() => { globalThis.promiseHookDone = true; })");
  ASSERT_EQ(unofficial_napi_process_microtasks(s.env), napi_ok);

  const std::string events = JsonStringify(s.env, "globalThis.promiseHookEvents");
  EXPECT_NE(events.find("\"init:"), std::string::npos) << events;
  EXPECT_NE(events.find("\"before\""), std::string::npos) << events;
  EXPECT_NE(events.find("\"after\""), std::string::npos) << events;
  EXPECT_NE(events.find("\"resolve\""), std::string::npos) << events;
}

TEST_F(Test35Promise, PromiseRejectCallbackUsesV8EventShape) {
  EnvScope s(runtime_.get());

  napi_value callback = RunScript(
      s.env,
      "(() => {"
      "  globalThis.promiseRejectEvents = [];"
      "  return (event, promise, reason) => {"
      "    promiseRejectEvents.push([event, reason === undefined ? 'undefined' : String(reason)]);"
      "  };"
      "})()");
  ASSERT_NE(callback, nullptr);
  ASSERT_EQ(unofficial_napi_set_promise_reject_callback(s.env, callback), napi_ok);

  RunScript(s.env, "globalThis.rejectedForHook = Promise.reject('bad')");
  ASSERT_EQ(unofficial_napi_process_microtasks(s.env), napi_ok);
  RunScript(s.env, "globalThis.rejectedForHook.catch(() => {})");
  ASSERT_EQ(unofficial_napi_process_microtasks(s.env), napi_ok);

  const std::string events = JsonStringify(s.env, "globalThis.promiseRejectEvents");
  EXPECT_NE(events.find("[0,\"bad\"]"), std::string::npos) << events;
  EXPECT_NE(events.find("[1,\"undefined\"]"), std::string::npos) << events;
}
