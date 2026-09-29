#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value Init(napi_env env, napi_value exports);
extern "C" void napi_v8_test_fail_next_external_buffer_after_transfer();
extern "C" napi_status NAPI_CDECL napi_create_external_buffer(
    napi_env env, size_t length, void* data, node_api_basic_finalize finalize_cb,
    void* finalize_hint, napi_value* result);

namespace {
void CountExternalBufferFinalizer(node_api_basic_env, void*, void* hint) {
  ++*static_cast<int*>(hint);
}
}  // namespace

class Test31DataView : public FixtureTestBase {};

TEST_F(Test31DataView, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  napi_value addon = Init(s.env, exports);
  ASSERT_NE(addon, nullptr);
  ASSERT_TRUE(InstallUpstreamJsShim(s, addon));
  ASSERT_TRUE(RunUpstreamJsFile(
      s, std::string(NAPI_TESTS_ROOT_PATH) + "/js-native-api/test_dataview/test.js"));
}

TEST_F(Test31DataView, CreateArrayBufferWithNullDataOutParam) {
  EnvScope s(runtime_.get());

  napi_value arraybuffer = nullptr;
  ASSERT_EQ(napi_create_arraybuffer(s.env, 12, nullptr, &arraybuffer), napi_ok);
  ASSERT_NE(arraybuffer, nullptr);

  bool is_arraybuffer = false;
  ASSERT_EQ(napi_is_arraybuffer(s.env, arraybuffer, &is_arraybuffer), napi_ok);
  ASSERT_TRUE(is_arraybuffer);

  void* data = nullptr;
  size_t byte_length = 0;
  ASSERT_EQ(napi_get_arraybuffer_info(s.env, arraybuffer, &data, &byte_length),
            napi_ok);
  ASSERT_NE(data, nullptr);
  ASSERT_EQ(byte_length, 12u);
}

TEST_F(Test31DataView, ExternalBufferFailureStillTransfersFinalizerOwnership) {
  EnvScope s(runtime_.get());
  uint8_t bytes[8] = {};
  int finalizer_calls = 0;
  napi_value buffer = nullptr;
  napi_v8_test_fail_next_external_buffer_after_transfer();
  EXPECT_EQ(napi_create_external_buffer(s.env, sizeof(bytes), bytes,
                                        CountExternalBufferFinalizer,
                                        &finalizer_calls, &buffer),
            napi_generic_failure);
  EXPECT_EQ(buffer, nullptr);
  EXPECT_EQ(finalizer_calls, 1);
}
