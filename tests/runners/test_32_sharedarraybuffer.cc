#include "test_env.h"
#include "unofficial_napi.h"
#include "upstream_js_test.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test32SharedArrayBuffer : public FixtureTestBase {};

TEST_F(Test32SharedArrayBuffer, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  napi_value addon = Init(s.env, exports);
  ASSERT_NE(addon, nullptr);
  ASSERT_TRUE(InstallUpstreamJsShim(s, addon));
  ASSERT_TRUE(RunUpstreamJsFile(
      s,
      std::string(NAPI_TESTS_ROOT_PATH) + "/js-native-api/test_sharedarraybuffer/test.js"));
}

TEST_F(Test32SharedArrayBuffer, BufferLeaseSupportsExactSharedRange) {
  EnvScope s(runtime_.get());
  napi_value source = nullptr;
  napi_value shared = nullptr;
  ASSERT_EQ(napi_create_string_utf8(
                s.env,
                "const value = new SharedArrayBuffer(4); "
                "new Uint8Array(value).set([5, 6, 7, 8]); value",
                NAPI_AUTO_LENGTH,
                &source),
            napi_ok);
  ASSERT_EQ(napi_run_script(s.env, source, &shared), napi_ok);

  unofficial_napi_buffer_lease lease = nullptr;
  uint8_t* data = nullptr;
  ASSERT_EQ(unofficial_napi_acquire_buffer_lease(
                s.env,
                shared,
                1,
                2,
                unofficial_napi_buffer_access_readwrite,
                &lease,
                reinterpret_cast<void**>(&data)),
            napi_ok);
  ASSERT_NE(lease, nullptr);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data[0], 6);
  EXPECT_EQ(data[1], 7);
  data[0] = 16;
  data[1] = 17;
  ASSERT_EQ(unofficial_napi_release_buffer_lease(s.env, lease, true), napi_ok);

  napi_value verification_source = nullptr;
  napi_value verification = nullptr;
  ASSERT_EQ(napi_create_string_utf8(
                s.env,
                "new Uint8Array(value)[1] * 100 + new Uint8Array(value)[2]",
                NAPI_AUTO_LENGTH,
                &verification_source),
            napi_ok);
  ASSERT_EQ(napi_run_script(s.env, verification_source, &verification), napi_ok);
  int32_t result = 0;
  ASSERT_EQ(napi_get_value_int32(s.env, verification, &result), napi_ok);
  EXPECT_EQ(result, 1617);
}
