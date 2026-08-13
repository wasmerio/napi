#ifndef NAPI_SHARED_TEST_ENV_H_
#define NAPI_SHARED_TEST_ENV_H_

#include <memory>

#include <gtest/gtest.h>

#include "unofficial_napi.h"

#ifndef NAPI_TEST_MODULE_API_VERSION
#define NAPI_TEST_MODULE_API_VERSION 10
#endif

class NapiTestRuntime {
 public:
  NapiTestRuntime() = default;
  ~NapiTestRuntime() = default;

  NapiTestRuntime(const NapiTestRuntime&) = delete;
  NapiTestRuntime& operator=(const NapiTestRuntime&) = delete;
};

class FixtureTestBase : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { runtime_ = std::make_unique<NapiTestRuntime>(); }
  static void TearDownTestSuite() { runtime_.reset(); }
  static std::unique_ptr<NapiTestRuntime> runtime_;
};

inline std::unique_ptr<NapiTestRuntime> FixtureTestBase::runtime_;

inline void InitializeTestEnvCreateOptions(
    unofficial_napi_env_create_options* options) {
  static constexpr char kDefaultFlags[] = "--expose-gc --js-float16array";
  ASSERT_NE(options, nullptr);
  *options = {};
  options->size = sizeof(*options);
  options->version = UNOFFICIAL_NAPI_ENV_CREATE_OPTIONS_VERSION;
  options->engine_flags = kDefaultFlags;
  options->engine_flags_length = sizeof(kDefaultFlags) - 1;
}

struct EnvScope {
  explicit EnvScope(NapiTestRuntime* runtime) {
    (void)runtime;
    unofficial_napi_env_create_options options{};
    InitializeTestEnvCreateOptions(&options);
    EXPECT_EQ(unofficial_napi_create_env(NAPI_TEST_MODULE_API_VERSION, &options, &env, &scope),
              napi_ok);
    EXPECT_NE(env, nullptr);
    EXPECT_NE(scope, nullptr);
  }

  ~EnvScope() {
    if (scope != nullptr) {
      EXPECT_EQ(unofficial_napi_release_env(scope, nullptr), napi_ok);
      scope = nullptr;
      env = nullptr;
    }
  }

  EnvScope(const EnvScope&) = delete;
  EnvScope& operator=(const EnvScope&) = delete;

  napi_env env = nullptr;
  void* scope = nullptr;
};

#endif  // NAPI_SHARED_TEST_ENV_H_
