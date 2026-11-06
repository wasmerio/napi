#include "test_env.h"
#include "upstream_js_test.h"

extern "C" napi_value Init(napi_env env, napi_value exports);
// node_api.h declares it from NAPI_VERSION 10; this target builds at 8.
extern "C" napi_status NAPI_CDECL node_api_create_buffer_from_arraybuffer(
    napi_env env, napi_value arraybuffer, size_t byte_offset, size_t byte_length,
    napi_value* result);

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

namespace {

void CountFinalizer(node_api_basic_env, void*, void* hint) {
  ++*static_cast<int*>(hint);
}

// An external array buffer whose finalizer detaches the buffer again, then
// makes an external, kept on the global.
struct SelfDetaching {
  uint8_t bytes[8] = {};
  napi_ref buffer = nullptr;
  int finalized = 0;
  napi_status detach_status = napi_generic_failure;
  bool detached = false;
  napi_status made_status = napi_generic_failure;
  int made_finalized = 0;
};

void FinalizeDetachingItself(node_api_basic_env basic_env, void* data, void* hint) {
  auto env = const_cast<napi_env>(basic_env);
  auto* state = static_cast<SelfDetaching*>(hint);
  EXPECT_EQ(data, state->bytes);
  ++state->finalized;
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) return;
  napi_value buffer = nullptr;
  if (napi_get_reference_value(env, state->buffer, &buffer) == napi_ok && buffer != nullptr) {
    state->detach_status = napi_detach_arraybuffer(env, buffer);
    napi_is_detached_arraybuffer(env, buffer, &state->detached);
  }
  napi_value external = nullptr;
  napi_value global = nullptr;
  if (napi_create_external(env, nullptr, CountFinalizer, &state->made_finalized, &external) ==
          napi_ok &&
      napi_get_global(env, &global) == napi_ok) {
    state->made_status = napi_set_named_property(env, global, "madeInFinalizer", external);
  }
  napi_close_handle_scope(env, scope);
}

}  // namespace

// The finalizer of an external array buffer may detach that buffer again
// (napi_detach_arraybuffer runs the finalizer before the buffer is marked
// detached on some engines). It still runs once, and the provider's
// bookkeeping stays intact for the values made after it.
TEST_F(Test33TypedArray, ExternalArrayBufferItsFinalizerDetachesIsFinalizedOnce) {
  SelfDetaching state;
  int others = 0;
  {
    EnvScope s(runtime_.get());
    napi_value buffer = nullptr;
    ASSERT_EQ(napi_create_external_arraybuffer(s.env, state.bytes, sizeof(state.bytes),
                                               FinalizeDetachingItself, &state, &buffer),
              napi_ok);
    ASSERT_EQ(napi_create_reference(s.env, buffer, 1, &state.buffer), napi_ok);
    ASSERT_EQ(napi_detach_arraybuffer(s.env, buffer), napi_ok);
    // A provider that defers the finalizer runs it here.
    ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                  s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
              napi_ok);
    EXPECT_EQ(state.finalized, 1);
    EXPECT_EQ(state.detach_status, napi_ok);
    EXPECT_TRUE(state.detached);
    EXPECT_EQ(state.made_status, napi_ok);
    ASSERT_EQ(napi_delete_reference(s.env, state.buffer), napi_ok);

    // Values made afterwards each get their own finalizer record.
    napi_value global = nullptr;
    ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
    for (const char* name : {"first", "second", "third"}) {
      napi_value external = nullptr;
      ASSERT_EQ(napi_create_external(s.env, nullptr, CountFinalizer, &others, &external), napi_ok);
      ASSERT_EQ(napi_set_named_property(s.env, global, name, external), napi_ok);
    }
  }
  EXPECT_EQ(state.finalized, 1);
  EXPECT_EQ(state.made_finalized, 1);
  EXPECT_EQ(others, 3);
}

// As in Node, a detached ArrayBuffer has no data and a byte length of 0.
TEST_F(Test33TypedArray, DetachedArrayBufferInfoIsEmpty) {
  ExternalBufferState state;
  EnvScope s(runtime_.get());
  napi_value owned = nullptr;
  ASSERT_EQ(napi_create_arraybuffer(s.env, 16, nullptr, &owned), napi_ok);
  napi_value external = nullptr;
  ASSERT_EQ(napi_create_external_arraybuffer(s.env, state.bytes, sizeof(state.bytes),
                                             FinalizeExternalBuffer, &state, &external),
            napi_ok);
  for (napi_value buffer : {owned, external}) {
    ASSERT_EQ(napi_detach_arraybuffer(s.env, buffer), napi_ok);
    void* data = &state;
    size_t length = 1;
    EXPECT_EQ(napi_get_arraybuffer_info(s.env, buffer, &data, &length), napi_ok);
    EXPECT_EQ(data, nullptr);
    EXPECT_EQ(length, 0u);
    bool pending = true;
    ASSERT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
    EXPECT_FALSE(pending);
  }
  // Not an ArrayBuffer at all is still an invalid argument.
  napi_value object = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &object), napi_ok);
  void* data = nullptr;
  size_t length = 0;
  EXPECT_EQ(napi_get_arraybuffer_info(s.env, object, &data, &length), napi_invalid_arg);
}

// As in Node, node_api_create_buffer_from_arraybuffer makes a Buffer that
// views the ArrayBuffer itself, so detaching the ArrayBuffer detaches the
// Buffer too.
TEST_F(Test33TypedArray, BufferFromAnArrayBufferViewsIt) {
  EnvScope s(runtime_.get());
  napi_value array_buffer = nullptr;
  void* storage = nullptr;
  ASSERT_EQ(napi_create_arraybuffer(s.env, 16, &storage, &array_buffer), napi_ok);
  napi_value buffer = nullptr;
  ASSERT_EQ(node_api_create_buffer_from_arraybuffer(s.env, array_buffer, 4, 8, &buffer), napi_ok);
  napi_value viewed = nullptr;
  size_t length = 0;
  size_t offset = 0;
  ASSERT_EQ(napi_get_typedarray_info(s.env, buffer, nullptr, &length, nullptr, &viewed, &offset),
            napi_ok);
  bool same = false;
  ASSERT_EQ(napi_strict_equals(s.env, viewed, array_buffer, &same), napi_ok);
  EXPECT_TRUE(same);
  EXPECT_EQ(offset, 4u);
  EXPECT_EQ(length, 8u);

  ASSERT_EQ(napi_detach_arraybuffer(s.env, array_buffer), napi_ok);
  napi_value element = nullptr;
  ASSERT_EQ(napi_get_element(s.env, buffer, 0, &element), napi_ok);
  napi_valuetype type = napi_number;
  ASSERT_EQ(napi_typeof(s.env, element, &type), napi_ok);
  EXPECT_EQ(type, napi_undefined);
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
