#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test29BigInt : public FixtureTestBase {};

TEST_F(Test29BigInt, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  napi_value addon = Init(s.env, exports);
  ASSERT_NE(addon, nullptr);
  ASSERT_TRUE(InstallUpstreamJsShim(s, addon));
  ASSERT_TRUE(
      RunUpstreamJsFile(s, std::string(NAPI_TESTS_ROOT_PATH) + "/js-native-api/test_bigint/test.js"));
}

TEST_F(Test29BigInt, MultiwordQueryAndShortBuffersStayWithinCapacity) {
  EnvScope s(runtime_.get());
  const uint64_t expected[] = {0x1122334455667788ULL, 0x99aabbccddeeff00ULL,
                               0x123456789abcdef0ULL};
  napi_value bigint = nullptr;
  ASSERT_EQ(napi_create_bigint_words(s.env, 1, 3, expected, &bigint), napi_ok);

  int sign = 0;
  size_t count = 0;
  ASSERT_EQ(napi_get_value_bigint_words(s.env, bigint, &sign, &count, nullptr),
            napi_ok);
  EXPECT_EQ(sign, 1);
  EXPECT_EQ(count, 3u);

  constexpr uint64_t sentinel = 0xdeadbeefcafebabeULL;
  uint64_t guarded[] = {sentinel, 0, sentinel};
  count = 1;
  sign = 0;
  ASSERT_EQ(napi_get_value_bigint_words(s.env, bigint, &sign, &count,
                                       &guarded[1]),
            napi_ok);
  EXPECT_EQ(sign, 1);
  EXPECT_EQ(count, 3u);  // Required size, not the one word written.
  EXPECT_EQ(guarded[0], sentinel);
  EXPECT_EQ(guarded[1], expected[0]);
  EXPECT_EQ(guarded[2], sentinel);

  uint64_t actual[3] = {};
  count = 3;
  ASSERT_EQ(napi_get_value_bigint_words(s.env, bigint, &sign, &count, actual),
            napi_ok);
  EXPECT_EQ(count, 3u);
  for (size_t i = 0; i < 3; ++i) EXPECT_EQ(actual[i], expected[i]);
}
