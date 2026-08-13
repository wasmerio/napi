#ifdef NAPI_MODULE
#undef NAPI_MODULE
#endif
#include "node_api.h"

#include "test_env.h"
#include "upstream_js_test.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test21General : public FixtureTestBase {};

namespace {

struct EnvAttachmentProbe {
  int cleanup_calls = 0;
  int destroy_calls = 0;
};

void AttachmentCleanup(napi_env env, void* data) {
  EXPECT_NE(env, nullptr);
  auto* probe = static_cast<EnvAttachmentProbe*>(data);
  ASSERT_NE(probe, nullptr);
  ++probe->cleanup_calls;
}

void AttachmentDestroy(napi_env env, void* data) {
  EXPECT_NE(env, nullptr);
  auto* probe = static_cast<EnvAttachmentProbe*>(data);
  ASSERT_NE(probe, nullptr);
  ++probe->destroy_calls;
}

}  // namespace

TEST_F(Test21General, EnvironmentCreationOptionsAreVersioned) {
  napi_env env = nullptr;
  void* owner = nullptr;

  unofficial_napi_env_create_options options{};
  options.size = sizeof(options) - 1;
  options.version = UNOFFICIAL_NAPI_ENV_CREATE_OPTIONS_VERSION;
  EXPECT_EQ(unofficial_napi_create_env(
                NAPI_TEST_MODULE_API_VERSION, &options, &env, &owner),
            napi_invalid_arg);
  EXPECT_EQ(env, nullptr);
  EXPECT_EQ(owner, nullptr);

  options.size = sizeof(options);
  options.version += 1;
  EXPECT_EQ(unofficial_napi_create_env(
                NAPI_TEST_MODULE_API_VERSION, &options, &env, &owner),
            napi_invalid_arg);

  options.version = UNOFFICIAL_NAPI_ENV_CREATE_OPTIONS_VERSION;
  options.engine_flags_length = 1;
  EXPECT_EQ(unofficial_napi_create_env(
                NAPI_TEST_MODULE_API_VERSION, &options, &env, &owner),
            napi_invalid_arg);
}

TEST_F(Test21General, EnvironmentHooksAttachAtomicallyOnce) {
  napi_env env = nullptr;
  void* owner = nullptr;
  ASSERT_EQ(unofficial_napi_create_env(
                NAPI_TEST_MODULE_API_VERSION, nullptr, &env, &owner),
            napi_ok);
  ASSERT_NE(env, nullptr);
  ASSERT_NE(owner, nullptr);

  EnvAttachmentProbe probe;
  unofficial_napi_env_hooks hooks{};
  hooks.size = sizeof(hooks);
  hooks.version = UNOFFICIAL_NAPI_ENV_HOOKS_VERSION;
  hooks.data = &probe;
  hooks.cleanup_callback = AttachmentCleanup;
  hooks.destroy_callback = AttachmentDestroy;

  unofficial_napi_env_hooks invalid = hooks;
  invalid.version += 1;
  EXPECT_EQ(unofficial_napi_attach_env(env, &invalid), napi_invalid_arg);
  ASSERT_EQ(unofficial_napi_attach_env(env, &hooks), napi_ok);
  EXPECT_EQ(unofficial_napi_attach_env(env, &hooks), napi_invalid_arg);
  EXPECT_EQ(probe.cleanup_calls, 0);
  EXPECT_EQ(probe.destroy_calls, 0);

  ASSERT_EQ(unofficial_napi_release_env(owner, nullptr), napi_ok);
  EXPECT_EQ(probe.cleanup_calls, 1);
  EXPECT_EQ(probe.destroy_calls, 1);
}

TEST_F(Test21General, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  napi_value addon = Init(s.env, exports);
  ASSERT_NE(addon, nullptr);
  ASSERT_TRUE(InstallUpstreamJsShim(s, addon));
  ASSERT_TRUE(
      RunUpstreamJsFile(s, std::string(NAPI_TESTS_ROOT_PATH) + "/js-native-api/test_general/test.js"));
}

TEST_F(Test21General, ProviderOwnsUninitializedArrayBufferAllocation) {
  EnvScope s(runtime_.get());

  napi_value arraybuffer = nullptr;
  ASSERT_EQ(unofficial_napi_create_uninitialized_arraybuffer(
                s.env, 32, true, &arraybuffer),
            napi_ok);
  ASSERT_NE(arraybuffer, nullptr);

  bool is_arraybuffer = false;
  ASSERT_EQ(napi_is_arraybuffer(s.env, arraybuffer, &is_arraybuffer), napi_ok);
  ASSERT_TRUE(is_arraybuffer);

  void* data = nullptr;
  size_t length = 0;
  ASSERT_EQ(napi_get_arraybuffer_info(s.env, arraybuffer, &data, &length), napi_ok);
  ASSERT_EQ(length, 32u);
  ASSERT_NE(data, nullptr);
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t index = 0; index < length; ++index) {
    EXPECT_EQ(bytes[index], 0u) << "byte " << index;
  }

  napi_value empty = nullptr;
  ASSERT_EQ(unofficial_napi_create_uninitialized_arraybuffer(
                s.env, 0, false, &empty),
            napi_ok);
  ASSERT_NE(empty, nullptr);
  ASSERT_EQ(napi_get_arraybuffer_info(s.env, empty, &data, &length), napi_ok);
  EXPECT_EQ(length, 0u);
}

TEST_F(Test21General, StructuredCloneOptionalTransferListDetachesArrayBuffer) {
  EnvScope s(runtime_.get());

  void* source_data = nullptr;
  napi_value source_buffer = nullptr;
  ASSERT_EQ(napi_create_arraybuffer(s.env, 8, &source_data, &source_buffer), napi_ok);
  ASSERT_NE(source_buffer, nullptr);
  ASSERT_NE(source_data, nullptr);
  static_cast<uint8_t*>(source_data)[0] = 42;

  napi_value source = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &source), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, source, "buffer", source_buffer), napi_ok);

  napi_value transfer_list = nullptr;
  ASSERT_EQ(napi_create_array_with_length(s.env, 1, &transfer_list), napi_ok);
  ASSERT_EQ(napi_set_element(s.env, transfer_list, 0, source_buffer), napi_ok);

  napi_value clone = nullptr;
  ASSERT_EQ(unofficial_napi_structured_clone(s.env, source, transfer_list, &clone), napi_ok);
  ASSERT_NE(clone, nullptr);

  napi_value source_byte_length = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, source_buffer, "byteLength", &source_byte_length),
            napi_ok);
  uint32_t source_length = 8;
  ASSERT_EQ(napi_get_value_uint32(s.env, source_byte_length, &source_length), napi_ok);
  EXPECT_EQ(source_length, 0u);

  napi_value cloned_buffer = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, clone, "buffer", &cloned_buffer), napi_ok);
  void* cloned_data = nullptr;
  size_t cloned_length = 0;
  ASSERT_EQ(napi_get_arraybuffer_info(s.env, cloned_buffer, &cloned_data, &cloned_length), napi_ok);
  ASSERT_EQ(cloned_length, 8u);
  ASSERT_NE(cloned_data, nullptr);
  EXPECT_EQ(static_cast<uint8_t*>(cloned_data)[0], 42u);
}

TEST_F(Test21General, MessageTakeConsumesOpaqueMessage) {
  EnvScope s(runtime_.get());

  napi_value source = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &source), napi_ok);
  napi_value expected = nullptr;
  ASSERT_EQ(napi_create_uint32(s.env, 42, &expected), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, source, "answer", expected), napi_ok);

  unofficial_napi_message message = nullptr;
  ASSERT_EQ(unofficial_napi_message_create(s.env, source, &message), napi_ok);
  ASSERT_NE(message, nullptr);

  napi_value result = nullptr;
  ASSERT_EQ(unofficial_napi_message_take(s.env, message, &result), napi_ok);
  ASSERT_NE(result, nullptr);
  napi_value answer = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, result, "answer", &answer), napi_ok);
  uint32_t actual = 0;
  ASSERT_EQ(napi_get_value_uint32(s.env, answer, &actual), napi_ok);
  EXPECT_EQ(actual, 42u);

  unofficial_napi_message dropped = nullptr;
  ASSERT_EQ(unofficial_napi_message_create(s.env, source, &dropped), napi_ok);
  ASSERT_NE(dropped, nullptr);
  unofficial_napi_message_drop(dropped);
}

TEST_F(Test21General, HeapSpaceStatisticsUseOneBulkSnapshot) {
  EnvScope s(runtime_.get());

  uint32_t required_count = 0;
  ASSERT_EQ(unofficial_napi_get_heap_space_statistics(
                s.env, nullptr, 0, &required_count),
            napi_ok);
  ASSERT_GT(required_count, 0u);

  std::array<unofficial_napi_heap_space_statistics, 64> statistics{};
  uint32_t snapshot_count = 0;
  ASSERT_EQ(unofficial_napi_get_heap_space_statistics(
                s.env,
                statistics.data(),
                static_cast<uint32_t>(statistics.size()),
                &snapshot_count),
            napi_ok);
  ASSERT_EQ(snapshot_count, required_count);
  ASSERT_LE(snapshot_count, statistics.size());

  for (uint32_t index = 0; index < snapshot_count; ++index) {
    EXPECT_NE(statistics[index].space_name[0], '\0') << "heap space " << index;
    EXPECT_EQ(statistics[index].space_name[
                  UNOFFICIAL_NAPI_HEAP_SPACE_NAME_MAX_LENGTH - 1],
              '\0')
        << "heap space " << index;
  }

  uint32_t partial_count = 0;
  unofficial_napi_heap_space_statistics first_space{};
  ASSERT_EQ(unofficial_napi_get_heap_space_statistics(
                s.env, &first_space, 1, &partial_count),
            napi_ok);
  EXPECT_EQ(partial_count, required_count);
  EXPECT_NE(first_space.space_name[0], '\0');

  EXPECT_EQ(unofficial_napi_get_heap_space_statistics(s.env, nullptr, 1, &partial_count),
            napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_get_heap_space_statistics(
                s.env, statistics.data(), statistics.size(), nullptr),
            napi_invalid_arg);
}

#if defined(NAPI_TEST_ENGINE_V8)
TEST_F(Test21General, CpuProfileResultIsEnvironmentOwnedString) {
  EnvScope s(runtime_.get());

  unofficial_napi_cpu_profile_start_result start_result =
      unofficial_napi_cpu_profile_start_too_many;
  uint32_t profile_id = 0;
  ASSERT_EQ(unofficial_napi_start_cpu_profile(
                s.env, &start_result, &profile_id),
            napi_ok);
  ASSERT_EQ(start_result, unofficial_napi_cpu_profile_start_ok);

  ASSERT_TRUE(RunScript(s,
                        "let total = 0; for (let i = 0; i < 10000; ++i) total += i;",
                        "cpu-profile-work.js"));

  bool found = false;
  napi_value json = nullptr;
  ASSERT_EQ(unofficial_napi_stop_cpu_profile(
                s.env, profile_id, &found, &json),
            napi_ok);
  ASSERT_TRUE(found);
  ASSERT_NE(json, nullptr);

  napi_valuetype type = napi_undefined;
  ASSERT_EQ(napi_typeof(s.env, json, &type), napi_ok);
  ASSERT_EQ(type, napi_string);
  size_t length = 0;
  ASSERT_EQ(napi_get_value_string_utf8(s.env, json, nullptr, 0, &length), napi_ok);
  EXPECT_GT(length, 0u);
}
#endif

#ifdef NAPI_TEST_ENGINE_QUICKJS
TEST_F(Test21General, GlobalBufferPrototypeDetection) {
  EnvScope s(runtime_.get());
  ASSERT_TRUE(RunScript(s, R"JS(
class FastBuffer extends Uint8Array {}
globalThis.FastBuffer = FastBuffer;
globalThis.bufferFromString = new FastBuffer([97, 98, 99]);
const backing = new ArrayBuffer(8);
new Uint8Array(backing).set([1, 2, 3], 2);
globalThis.slicedBuffer = new FastBuffer(backing, 2, 3);
globalThis.emptyBuffer = new FastBuffer();
globalThis.plainUint8Array = new Uint8Array([4, 5, 6]);
globalThis.plainArrayBuffer = new ArrayBuffer(4);
)JS",
                        "buffer-branding-setup"));

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);

  napi_value fast_buffer = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, global, "FastBuffer", &fast_buffer), napi_ok);
  napi_value fast_buffer_prototype = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, fast_buffer, "prototype", &fast_buffer_prototype),
            napi_ok);

  napi_value initially_unbranded = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, global, "bufferFromString", &initially_unbranded),
            napi_ok);
  bool initially_is_buffer = true;
  ASSERT_EQ(napi_is_buffer(s.env, initially_unbranded, &initially_is_buffer), napi_ok);
  EXPECT_FALSE(initially_is_buffer);

  ASSERT_TRUE(RunScript(s, R"JS(
function Buffer() {}
Buffer.prototype = FastBuffer.prototype;
globalThis.Buffer = Buffer;
)JS",
                        "buffer-global-setup"));

  auto expect_buffer_bytes = [&](const char* name, const uint8_t* expected, size_t length) {
    napi_value value = nullptr;
    ASSERT_EQ(napi_get_named_property(s.env, global, name, &value), napi_ok) << name;
    bool is_buffer = false;
    ASSERT_EQ(napi_is_buffer(s.env, value, &is_buffer), napi_ok) << name;
    EXPECT_TRUE(is_buffer) << name;

    void* raw = nullptr;
    size_t raw_length = 0;
    ASSERT_EQ(napi_get_buffer_info(s.env, value, &raw, &raw_length), napi_ok) << name;
    ASSERT_EQ(raw_length, length) << name;
    if (length > 0) {
      ASSERT_NE(raw, nullptr) << name;
      EXPECT_EQ(std::memcmp(raw, expected, length), 0) << name;
    }
  };

  const uint8_t abc[] = {97, 98, 99};
  expect_buffer_bytes("bufferFromString", abc, sizeof(abc));
  const uint8_t sliced[] = {1, 2, 3};
  expect_buffer_bytes("slicedBuffer", sliced, sizeof(sliced));
  expect_buffer_bytes("emptyBuffer", nullptr, 0);

  bool prototype_is_buffer = true;
  ASSERT_EQ(napi_is_buffer(s.env, fast_buffer_prototype, &prototype_is_buffer), napi_ok);
  EXPECT_FALSE(prototype_is_buffer);

  napi_value plain_uint8 = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, global, "plainUint8Array", &plain_uint8), napi_ok);
  bool plain_uint8_is_buffer = true;
  ASSERT_EQ(napi_is_buffer(s.env, plain_uint8, &plain_uint8_is_buffer), napi_ok);
  EXPECT_FALSE(plain_uint8_is_buffer);
  void* raw = nullptr;
  size_t raw_length = 0;
  EXPECT_EQ(napi_get_buffer_info(s.env, plain_uint8, &raw, &raw_length), napi_invalid_arg);

  napi_value plain_arraybuffer = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, global, "plainArrayBuffer", &plain_arraybuffer),
            napi_ok);
  bool plain_arraybuffer_is_buffer = true;
  ASSERT_EQ(napi_is_buffer(s.env, plain_arraybuffer, &plain_arraybuffer_is_buffer), napi_ok);
  EXPECT_FALSE(plain_arraybuffer_is_buffer);

  ASSERT_TRUE(RunScript(s, R"JS(
function FakeBuffer() {}
FakeBuffer.prototype = Uint8Array.prototype;
globalThis.Buffer = FakeBuffer;
globalThis.fakePlainUint8Array = new Uint8Array([9, 10, 11]);
)JS",
                        "buffer-global-replacement"));
  expect_buffer_bytes("bufferFromString", abc, sizeof(abc));
  napi_value fake_plain_uint8 = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, global, "fakePlainUint8Array", &fake_plain_uint8),
            napi_ok);
  bool fake_plain_uint8_is_buffer = true;
  ASSERT_EQ(napi_is_buffer(s.env, fake_plain_uint8, &fake_plain_uint8_is_buffer), napi_ok);
  EXPECT_FALSE(fake_plain_uint8_is_buffer);
}

TEST_F(Test21General, NativeCreatedBuffersAdoptGlobalBufferPrototype) {
  EnvScope s(runtime_.get());
  ASSERT_TRUE(RunScript(s, R"JS(
class FastBuffer extends Uint8Array {}
globalThis.FastBuffer = FastBuffer;
)JS",
                        "native-buffer-setup"));

  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  napi_value fast_buffer = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, global, "FastBuffer", &fast_buffer), napi_ok);

  auto expect_native_buffer_bytes = [&](napi_value value, const uint8_t* expected, size_t length) {
    bool is_buffer = false;
    ASSERT_EQ(napi_is_buffer(s.env, value, &is_buffer), napi_ok);
    EXPECT_TRUE(is_buffer);

    void* raw = nullptr;
    size_t raw_length = 0;
    ASSERT_EQ(napi_get_buffer_info(s.env, value, &raw, &raw_length), napi_ok);
    ASSERT_EQ(raw_length, length);
    if (length > 0) {
      ASSERT_NE(raw, nullptr);
      EXPECT_EQ(std::memcmp(raw, expected, length), 0);
    }
  };

  const uint8_t before_bytes[] = {7, 8, 9};
  napi_value native_before = nullptr;
  ASSERT_EQ(napi_create_buffer_copy(s.env,
                                    sizeof(before_bytes),
                                    before_bytes,
                                    nullptr,
                                    &native_before),
            napi_ok);
  expect_native_buffer_bytes(native_before, before_bytes, sizeof(before_bytes));

  ASSERT_TRUE(RunScript(s, R"JS(
function Buffer() {}
Buffer.prototype = FastBuffer.prototype;
globalThis.Buffer = Buffer;
)JS",
                        "native-buffer-global-setup"));
  expect_native_buffer_bytes(native_before, before_bytes, sizeof(before_bytes));
  bool native_before_instanceof = false;
  ASSERT_EQ(napi_instanceof(s.env, native_before, fast_buffer, &native_before_instanceof),
            napi_ok);
  EXPECT_TRUE(native_before_instanceof);

  const uint8_t after_bytes[] = {10, 11, 12};
  napi_value native_after = nullptr;
  ASSERT_EQ(napi_create_buffer_copy(s.env,
                                    sizeof(after_bytes),
                                    after_bytes,
                                    nullptr,
                                    &native_after),
            napi_ok);
  expect_native_buffer_bytes(native_after, after_bytes, sizeof(after_bytes));
  bool native_after_instanceof = false;
  ASSERT_EQ(napi_instanceof(s.env, native_after, fast_buffer, &native_after_instanceof),
            napi_ok);
  EXPECT_TRUE(native_after_instanceof);
}
#endif
