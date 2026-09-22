#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test33TypedArray : public FixtureTestBase {};

namespace {

struct ExternalBufferState {
  uint8_t bytes[4] = {11, 22, 33, 44};
  int finalized = 0;
};

void FinalizeExternalBuffer(node_api_basic_env env, void* data, void* hint) {
  auto* state = static_cast<ExternalBufferState*>(hint);
  EXPECT_EQ(data, state->bytes);
  ++state->finalized;
}

napi_value RunBufferScript(napi_env env, const char* source) {
  napi_value script = nullptr;
  EXPECT_EQ(napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &script), napi_ok);
  napi_value result = nullptr;
  EXPECT_EQ(napi_run_script(env, script, &result), napi_ok);
  return result;
}

}  // namespace

TEST_F(Test33TypedArray, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  napi_value addon = Init(s.env, exports);
  ASSERT_NE(addon, nullptr);
  ASSERT_TRUE(InstallUpstreamJsShim(s, addon));
  ASSERT_TRUE(RunUpstreamJsFile(
      s, std::string(NAPI_TESTS_ROOT_PATH) + "/js-native-api/test_typedarray/test.js"));
}

TEST_F(Test33TypedArray, OwnedArrayBufferTransferCanResize) {
  EnvScope s(runtime_.get());
  napi_value buffer = nullptr;
  void* storage = nullptr;
  ASSERT_EQ(napi_create_arraybuffer(s.env, 4, &storage, &buffer), napi_ok);
  ASSERT_NE(storage, nullptr);
  auto* bytes = static_cast<uint8_t*>(storage);
  for (uint8_t i = 0; i < 4; ++i) bytes[i] = i + 1;
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "ownedBuffer", buffer), napi_ok);
  napi_value transferred = RunBufferScript(s.env, "ownedBuffer.transfer(8)");
  ASSERT_NE(transferred, nullptr);
  bool detached = false;
  ASSERT_EQ(napi_is_detached_arraybuffer(s.env, buffer, &detached), napi_ok);
  EXPECT_TRUE(detached);
  size_t length = 0;
  ASSERT_EQ(napi_get_arraybuffer_info(s.env, transferred, &storage, &length), napi_ok);
  ASSERT_EQ(length, 8u);
  bytes = static_cast<uint8_t*>(storage);
  for (uint8_t i = 0; i < 8; ++i) EXPECT_EQ(bytes[i], i < 4 ? i + 1 : 0);
}

TEST_F(Test33TypedArray, ExternalArrayBufferTransferAndDetachFinalizeOnce) {
  ExternalBufferState state;
  {
    EnvScope s(runtime_.get());
    napi_value buffer = nullptr;
    ASSERT_EQ(napi_create_external_arraybuffer(s.env, state.bytes, sizeof(state.bytes),
                                               FinalizeExternalBuffer, &state, &buffer),
              napi_ok);
    napi_value global = nullptr;
    ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, global, "externalBuffer", buffer), napi_ok);
    napi_value transferred = RunBufferScript(s.env, "externalBuffer.transfer()");
    ASSERT_NE(transferred, nullptr);
    EXPECT_EQ(state.finalized, 0);
    void* storage = nullptr;
    size_t length = 0;
    ASSERT_EQ(napi_get_arraybuffer_info(s.env, transferred, &storage, &length), napi_ok);
    ASSERT_EQ(length, sizeof(state.bytes));
    EXPECT_EQ(static_cast<uint8_t*>(storage)[2], 33);
    ASSERT_EQ(napi_detach_arraybuffer(s.env, transferred), napi_ok);
    ASSERT_EQ(napi_detach_arraybuffer(s.env, transferred), napi_ok);
  }
  EXPECT_EQ(state.finalized, 1);
}

#if defined(NAPI_TEST_ENGINE_QUICKJS)
TEST_F(Test33TypedArray, ExternalArrayBufferResizeFailurePreservesBackingStore) {
  ExternalBufferState state;
  {
    EnvScope s(runtime_.get());
    napi_value buffer = nullptr;
    ASSERT_EQ(napi_create_external_arraybuffer(s.env, state.bytes, sizeof(state.bytes),
                                               FinalizeExternalBuffer, &state, &buffer),
              napi_ok);
    napi_value global = nullptr;
    ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, global, "externalBuffer", buffer), napi_ok);
    napi_value failed = RunBufferScript(s.env,
        "(() => { try { externalBuffer.transfer(8); return false; } catch { return true; } })()");
    bool did_fail = false;
    ASSERT_EQ(napi_get_value_bool(s.env, failed, &did_fail), napi_ok);
    EXPECT_TRUE(did_fail);
    EXPECT_EQ(state.finalized, 0);
    void* storage = nullptr;
    size_t length = 0;
    ASSERT_EQ(napi_get_arraybuffer_info(s.env, buffer, &storage, &length), napi_ok);
    EXPECT_EQ(storage, state.bytes);
    EXPECT_EQ(length, sizeof(state.bytes));
    EXPECT_EQ(state.bytes[3], 44);
  }
  EXPECT_EQ(state.finalized, 1);
}
#endif
