#ifndef NAPI_QUICKJS_TEST_ENV_H_
#define NAPI_QUICKJS_TEST_ENV_H_

#include <memory>
#include <gtest/gtest.h>
#include "quickjs.h"
#include "unofficial_napi.h"

// External declarations for the environment bridge
extern "C"
{
  napi_status NAPI_CDECL unofficial_napi_create_env_from_context(
      JSContext *context, int32_t module_api_version, napi_env *result);
  napi_status NAPI_CDECL unofficial_napi_destroy_env_instance_for_testing(napi_env env);
}

class QuickJSRuntime
{
public:
  QuickJSRuntime()
  {
    rt_ = JS_NewRuntime();
  }

  ~QuickJSRuntime()
  {
    JS_FreeRuntime(rt_);
  }

  JSRuntime *runtime() const { return rt_; }

private:
  JSRuntime *rt_ = nullptr;
};

class FixtureTestBase : public ::testing::Test
{
protected:
  static void SetUpTestSuite() { runtime_ = std::make_unique<QuickJSRuntime>(); }
  static void TearDownTestSuite() { runtime_.reset(); }
  static std::unique_ptr<QuickJSRuntime> runtime_;
};

inline std::unique_ptr<QuickJSRuntime> FixtureTestBase::runtime_;

struct EnvScope
{
  explicit EnvScope(QuickJSRuntime *runtime)
  {
    ctx = JS_NewContext(runtime->runtime());
    // Initialize the Node-API environment from the QuickJS context
    EXPECT_EQ(unofficial_napi_create_env_from_context(ctx, 8, &env), napi_ok);
    EXPECT_NE(env, nullptr);
  }

  ~EnvScope()
  {
    if (env != nullptr)
    {
      unofficial_napi_destroy_env_instance_for_testing(env);
      env = nullptr;
    }
  }

  JSContext *ctx;
  napi_env env = nullptr;
};

#endif // NAPI_QUICKJS_TEST_ENV_H_