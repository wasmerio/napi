#ifdef NAPI_MODULE
#undef NAPI_MODULE
#endif
#include "node_api.h"

#include "test_env.h"
#include "upstream_js_test.h"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

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

// As in Node, a view over a detached ArrayBuffer has no data and a length and
// offset of 0: a typed array, a DataView and a Buffer, and an external Buffer
// whose ArrayBuffer is detached (as the release does before its finalizer).
// A Buffer is still a Buffer.
TEST_F(Test33TypedArray, DetachedViewInfoIsEmpty) {
  ExternalBufferState state;
  EnvScope s(runtime_.get());
  napi_value array_buffer = nullptr;
  ASSERT_EQ(napi_create_arraybuffer(s.env, 16, nullptr, &array_buffer), napi_ok);
  napi_value typed_array = nullptr;
  ASSERT_EQ(napi_create_typedarray(s.env, napi_uint8_array, 8, array_buffer, 4, &typed_array),
            napi_ok);
  napi_value buffer = nullptr;
  ASSERT_EQ(node_api_create_buffer_from_arraybuffer(s.env, array_buffer, 2, 6, &buffer), napi_ok);
  napi_value data_view = nullptr;
  ASSERT_EQ(napi_create_dataview(s.env, 8, array_buffer, 8, &data_view), napi_ok);
  napi_value external = nullptr;
  ASSERT_EQ(napi_create_external_buffer(s.env, sizeof(state.bytes), state.bytes,
                                        FinalizeExternalBuffer, &state, &external),
            napi_ok);
  napi_value external_array_buffer = nullptr;
  ASSERT_EQ(napi_get_typedarray_info(s.env, external, nullptr, nullptr, nullptr,
                                     &external_array_buffer, nullptr),
            napi_ok);
  ASSERT_EQ(napi_detach_arraybuffer(s.env, array_buffer), napi_ok);
  ASSERT_EQ(napi_detach_arraybuffer(s.env, external_array_buffer), napi_ok);

  auto expect_nothing_pending = [&](const char* what) {
    bool pending = true;
    ASSERT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
    EXPECT_FALSE(pending) << what;
    napi_value exception = nullptr;
    if (pending) napi_get_and_clear_last_exception(s.env, &exception);
  };
  auto expect_same = [&](napi_value viewed, napi_value expected, const char* what) {
    bool same = false;
    ASSERT_NE(viewed, nullptr) << what;
    ASSERT_EQ(napi_strict_equals(s.env, viewed, expected, &same), napi_ok);
    EXPECT_TRUE(same) << what;
  };

  for (napi_value view : {typed_array, buffer, external}) {
    const char* what = view == typed_array ? "typed array" : view == buffer ? "buffer" : "external";
    napi_typedarray_type type = napi_int8_array;
    size_t length = 99;
    void* data = &state;
    napi_value viewed = nullptr;
    size_t offset = 99;
    EXPECT_EQ(napi_get_typedarray_info(s.env, view, &type, &length, &data, &viewed, &offset),
              napi_ok)
        << what;
    EXPECT_EQ(type, napi_uint8_array) << what;
    EXPECT_EQ(length, 0u) << what;
    EXPECT_EQ(data, nullptr) << what;
    EXPECT_EQ(offset, 0u) << what;
    expect_same(viewed, view == external ? external_array_buffer : array_buffer, what);
    expect_nothing_pending(what);
  }
  for (napi_value view : {buffer, external}) {
    const char* what = view == buffer ? "buffer" : "external";
    bool is_buffer = false;
    EXPECT_EQ(napi_is_buffer(s.env, view, &is_buffer), napi_ok) << what;
    EXPECT_TRUE(is_buffer) << what;
    size_t length = 99;
    void* data = &state;
    EXPECT_EQ(napi_get_buffer_info(s.env, view, &data, &length), napi_ok) << what;
    EXPECT_EQ(length, 0u) << what;
    EXPECT_EQ(data, nullptr) << what;
    expect_nothing_pending(what);
  }
  size_t length = 99;
  void* data = &state;
  napi_value viewed = nullptr;
  size_t offset = 99;
  EXPECT_EQ(napi_get_dataview_info(s.env, data_view, &length, &data, &viewed, &offset), napi_ok);
  EXPECT_EQ(length, 0u);
  EXPECT_EQ(data, nullptr);
  EXPECT_EQ(offset, 0u);
  expect_same(viewed, array_buffer, "data view");
  expect_nothing_pending("data view");
}

// A view's own `buffer` property, which a script can redefine, does not
// decide which ArrayBuffer a view has, nor whether it is detached.
TEST_F(Test33TypedArray, ViewInfoIgnoresARedefinedBufferProperty) {
  EnvScope s(runtime_.get());
  RunBufferScript(s.env,
                  "globalThis.backing = new ArrayBuffer(16);"
                  "globalThis.other = new ArrayBuffer(16);"
                  "globalThis.typed = new Uint8Array(backing);"
                  "Object.defineProperty(typed, 'buffer', { value: other });"
                  "globalThis.liveBacking = new ArrayBuffer(16);"
                  "globalThis.detachedOther = new ArrayBuffer(16);"
                  "globalThis.view = new DataView(liveBacking, 2, 4);"
                  "Object.defineProperty(view, 'buffer', { value: detachedOther });"
                  "0");
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  auto named = [&](const char* name) {
    napi_value value = nullptr;
    EXPECT_EQ(napi_get_named_property(s.env, global, name, &value), napi_ok);
    return value;
  };
  napi_value backing = named("backing");
  napi_value live_backing = named("liveBacking");
  ASSERT_EQ(napi_detach_arraybuffer(s.env, backing), napi_ok);
  ASSERT_EQ(napi_detach_arraybuffer(s.env, named("other")), napi_ok);
  ASSERT_EQ(napi_detach_arraybuffer(s.env, named("detachedOther")), napi_ok);

  // The typed array's own buffer is detached: empty, and that buffer.
  size_t length = 99;
  void* data = &length;
  napi_value viewed = nullptr;
  size_t offset = 99;
  ASSERT_EQ(
      napi_get_typedarray_info(s.env, named("typed"), nullptr, &length, &data, &viewed, &offset),
      napi_ok);
  EXPECT_EQ(length, 0u);
  EXPECT_EQ(data, nullptr);
  EXPECT_EQ(offset, 0u);
  bool same = false;
  ASSERT_EQ(napi_strict_equals(s.env, viewed, backing, &same), napi_ok);
  EXPECT_TRUE(same) << "the typed array's own ArrayBuffer";

  // The DataView's own buffer is alive: its bytes 2 to 6, and that buffer.
  void* live_data = nullptr;
  size_t live_length = 0;
  ASSERT_EQ(napi_get_arraybuffer_info(s.env, live_backing, &live_data, &live_length), napi_ok);
  length = 99;
  data = nullptr;
  viewed = nullptr;
  offset = 99;
  ASSERT_EQ(napi_get_dataview_info(s.env, named("view"), &length, &data, &viewed, &offset),
            napi_ok);
  EXPECT_EQ(length, 4u);
  EXPECT_EQ(offset, 2u);
  EXPECT_EQ(data, static_cast<uint8_t*>(live_data) + 2);
  same = false;
  ASSERT_EQ(napi_strict_equals(s.env, viewed, live_backing, &same), napi_ok);
  EXPECT_TRUE(same) << "the DataView's own ArrayBuffer";
}

namespace {

napi_value NamedGlobal(napi_env env, const char* name) {
  napi_value global = nullptr;
  napi_value value = nullptr;
  EXPECT_EQ(napi_get_global(env, &global), napi_ok);
  EXPECT_EQ(napi_get_named_property(env, global, name, &value), napi_ok) << name;
  return value;
}

void SetGlobal(napi_env env, const char* name, napi_value value) {
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(env, global, name, value), napi_ok) << name;
}

int32_t GlobalInt(napi_env env, const char* name) {
  int32_t out = -1;
  EXPECT_EQ(napi_get_value_int32(env, NamedGlobal(env, name), &out), napi_ok) << name;
  return out;
}

uint8_t* ArrayBufferData(napi_env env, napi_value array_buffer) {
  void* data = nullptr;
  size_t length = 0;
  EXPECT_EQ(napi_get_arraybuffer_info(env, array_buffer, &data, &length), napi_ok);
  return static_cast<uint8_t*>(data);
}

void ExpectNothingPending(napi_env env, const char* what) {
  bool pending = true;
  ASSERT_EQ(napi_is_exception_pending(env, &pending), napi_ok);
  EXPECT_FALSE(pending) << what;
  napi_value exception = nullptr;
  if (pending) napi_get_and_clear_last_exception(env, &exception);
}

struct ViewLayout {
  napi_status status = napi_generic_failure;
  size_t length = 99;  // elements for a typed array, bytes for a DataView
  size_t offset = 99;
  void* data = nullptr;
  napi_value buffer = nullptr;
};

ViewLayout TypedArrayLayout(napi_env env, napi_value view) {
  ViewLayout out;
  out.status = napi_get_typedarray_info(env, view, nullptr, &out.length, &out.data, &out.buffer,
                                        &out.offset);
  return out;
}

ViewLayout DataViewLayout(napi_env env, napi_value view) {
  ViewLayout out;
  out.status = napi_get_dataview_info(env, view, &out.length, &out.data, &out.buffer, &out.offset);
  return out;
}

ViewLayout BufferLayout(napi_env env, napi_value view) {
  ViewLayout out;
  out.status = napi_get_buffer_info(env, view, &out.data, &out.length);
  return out;
}

// A script redefines, on views and on the prototypes they inherit from, every
// property that tells a view's layout, as getters that count their calls. The
// layout N-API reports still is the view's own, and no getter runs.
void ExpectLayoutFromTheViewsSlots(napi_env env, const char* where) {
  SCOPED_TRACE(where);
  napi_value backing = RunBufferScript(env, "new ArrayBuffer(32)");
  ASSERT_NE(backing, nullptr);
  SetGlobal(env, "backing", backing);
  napi_value buffer = nullptr;
  ASSERT_EQ(node_api_create_buffer_from_arraybuffer(env, backing, 12, 8, &buffer), napi_ok);
  SetGlobal(env, "buf", buffer);
  RunBufferScript(env,
                  "globalThis.calls = 0;"
                  "globalThis.typed = new Uint16Array(backing, 4, 6);"
                  "globalThis.view = new DataView(backing, 2, 8);"
                  "globalThis.ownTyped = new Uint16Array(backing, 8, 4);"
                  "globalThis.ownView = new DataView(backing, 6, 4);"
                  "globalThis.other = new ArrayBuffer(4);"
                  "{"
                  "  const lie = (name) => ({ configurable: true,"
                  "    get() { calls++; return name === 'buffer' ? other : 1 << 30; } });"
                  "  const names = ['length', 'byteLength', 'byteOffset', 'buffer'];"
                  "  for (const target of [ownTyped, ownView, buf])"
                  "    for (const name of names) Object.defineProperty(target, name, lie(name));"
                  "  const typedArrayPrototype = Object.getPrototypeOf(Uint8Array.prototype);"
                  "  for (const target of [typedArrayPrototype, DataView.prototype])"
                  "    for (const name of names) Object.defineProperty(target, name, lie(name));"
                  "}"
                  "0");
  uint8_t* base = ArrayBufferData(env, backing);
  auto expect_layout = [&](const ViewLayout& layout, size_t length, size_t offset,
                           const char* what) {
    EXPECT_EQ(layout.status, napi_ok) << what;
    EXPECT_EQ(layout.length, length) << what;
    EXPECT_EQ(layout.data, base + offset) << what;
    if (layout.offset != 99) {
      EXPECT_EQ(layout.offset, offset) << what;
    }
    if (layout.buffer != nullptr) {
      bool same = false;
      ASSERT_EQ(napi_strict_equals(env, layout.buffer, backing, &same), napi_ok);
      EXPECT_TRUE(same) << what << ": its own ArrayBuffer";
    }
    ExpectNothingPending(env, what);
  };
  expect_layout(TypedArrayLayout(env, NamedGlobal(env, "typed")), 6, 4, "typed array");
  expect_layout(TypedArrayLayout(env, NamedGlobal(env, "ownTyped")), 4, 8, "own typed array");
  expect_layout(TypedArrayLayout(env, buffer), 8, 12, "buffer as a typed array");
  expect_layout(BufferLayout(env, buffer), 8, 12, "buffer");
  expect_layout(DataViewLayout(env, NamedGlobal(env, "view")), 8, 2, "data view");
  expect_layout(DataViewLayout(env, NamedGlobal(env, "ownView")), 4, 6, "own data view");

  // A view is not an ArrayBuffer, whatever its byteLength says.
  bool detached = true;
  EXPECT_EQ(napi_is_detached_arraybuffer(env, NamedGlobal(env, "typed"), &detached),
            napi_arraybuffer_expected);
  EXPECT_EQ(napi_is_detached_arraybuffer(env, NamedGlobal(env, "ownView"), &detached),
            napi_arraybuffer_expected);
  ExpectNothingPending(env, "napi_is_detached_arraybuffer");
  EXPECT_EQ(GlobalInt(env, "calls"), 0) << "a getter the script defined ran";
}

}  // namespace

// A typed array's, DataView's or Buffer's length, offset and ArrayBuffer are
// the ones in its internal slots. Page scripts control their realm's
// prototypes and can redefine those properties on any view: a length or
// offset read from them would let a script make native code read or write
// out of bounds.
TEST_F(Test33TypedArray, ViewLayoutComesFromItsSlotsInAnyRealm) {
  EnvScope s(runtime_.get());
  ExpectLayoutFromTheViewsSlots(s.env, "the env's realm");

  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  unofficial_napi_realm realm = nullptr;
  ASSERT_EQ(unofficial_napi_create_realm(s.env, &options, &realm, nullptr), napi_ok);
  unofficial_napi_realm_scope scope = 0;
  ASSERT_EQ(unofficial_napi_enter_realm(s.env, realm, &scope), napi_ok);
  ExpectLayoutFromTheViewsSlots(s.env, "a realm the env entered");
  ASSERT_EQ(unofficial_napi_leave_realm(s.env, scope), napi_ok);
}

// As in Node, a view over a resizable ArrayBuffer follows the buffer's size: a
// length-tracking view reports what of the buffer is past its offset now, and
// a fixed-length view the buffer shrank under reports a length of 0 (and
// reports its length again once the buffer grows back), with no exception.
// Such an out-of-bounds view's offset is left unchecked: V8 reports the view's
// own, QuickJS 0, as it has no way to read it (its accessors report 0 or
// throw).
TEST_F(Test33TypedArray, ViewsOfAResizedBufferReportItsCurrentSize) {
  EnvScope s(runtime_.get());
  RunBufferScript(s.env,
                  "globalThis.rab = new ArrayBuffer(16, { maxByteLength: 32 });"
                  "globalThis.tracking = new Uint8Array(rab, 4);"
                  "globalThis.fixed = new Uint16Array(rab, 4, 4);"
                  "globalThis.trackingView = new DataView(rab, 2);"
                  "globalThis.fixedView = new DataView(rab, 2, 8);"
                  "0");
  napi_value rab = NamedGlobal(s.env, "rab");
  napi_value buffer = nullptr;
  ASSERT_EQ(node_api_create_buffer_from_arraybuffer(s.env, rab, 4, 8, &buffer), napi_ok);
  napi_value tracking = NamedGlobal(s.env, "tracking");
  napi_value fixed = NamedGlobal(s.env, "fixed");
  napi_value tracking_view = NamedGlobal(s.env, "trackingView");
  napi_value fixed_view = NamedGlobal(s.env, "fixedView");

  auto expect_layout = [&](const ViewLayout& layout, size_t length, const char* what) {
    EXPECT_EQ(layout.status, napi_ok) << what;
    EXPECT_EQ(layout.length, length) << what;
    if (layout.buffer != nullptr) {
      bool same = false;
      ASSERT_EQ(napi_strict_equals(s.env, layout.buffer, rab, &same), napi_ok);
      EXPECT_TRUE(same) << what << ": its own ArrayBuffer";
    }
    ExpectNothingPending(s.env, what);
  };
  // In bounds, also the data and offset.
  auto expect_in_bounds = [&](const ViewLayout& layout, size_t length, size_t offset,
                              const char* what) {
    expect_layout(layout, length, what);
    EXPECT_EQ(layout.data, ArrayBufferData(s.env, rab) + offset) << what;
    if (layout.offset != 99) {
      EXPECT_EQ(layout.offset, offset) << what;
    }
  };

  RunBufferScript(s.env, "rab.resize(8); 0");
  expect_in_bounds(TypedArrayLayout(s.env, tracking), 4, 4, "shrunk: tracking typed array");
  expect_layout(TypedArrayLayout(s.env, fixed), 0, "shrunk: fixed typed array");
  expect_layout(TypedArrayLayout(s.env, buffer), 0, "shrunk: buffer as a typed array");
  expect_layout(BufferLayout(s.env, buffer), 0, "shrunk: buffer");
  expect_in_bounds(DataViewLayout(s.env, tracking_view), 6, 2, "shrunk: tracking data view");
  expect_layout(DataViewLayout(s.env, fixed_view), 0, "shrunk: fixed data view");
  bool is_buffer = false;
  EXPECT_EQ(napi_is_buffer(s.env, buffer, &is_buffer), napi_ok);
  EXPECT_TRUE(is_buffer) << "a Buffer out of bounds is still a Buffer";

  RunBufferScript(s.env, "rab.resize(24); 0");
  expect_in_bounds(TypedArrayLayout(s.env, tracking), 20, 4, "grown: tracking typed array");
  expect_in_bounds(TypedArrayLayout(s.env, fixed), 4, 4, "grown: fixed typed array");
  expect_in_bounds(TypedArrayLayout(s.env, buffer), 8, 4, "grown: buffer as a typed array");
  expect_in_bounds(BufferLayout(s.env, buffer), 8, 4, "grown: buffer");
  expect_in_bounds(DataViewLayout(s.env, tracking_view), 22, 2, "grown: tracking data view");
  expect_in_bounds(DataViewLayout(s.env, fixed_view), 8, 2, "grown: fixed data view");
}

// A termination request leaves a view's layout as it is (QuickJS polls its
// interrupt handler, now and then, on the calls that read it).
TEST_F(Test33TypedArray, ViewLayoutHoldsWhileTerminating) {
  EnvScope s(runtime_.get());
  RunBufferScript(s.env,
                  "globalThis.typed = new Uint16Array(new ArrayBuffer(32), 4, 6);"
                  "globalThis.view = new DataView(new ArrayBuffer(32), 2, 8);"
                  "0");
  napi_value typed = NamedGlobal(s.env, "typed");
  napi_value view = NamedGlobal(s.env, "view");
  ASSERT_EQ(unofficial_napi_terminate_execution(s.env), napi_ok);
  int wrong = 0;
  for (int i = 0; i < 20000; ++i) {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    const ViewLayout typed_layout = TypedArrayLayout(s.env, typed);
    const ViewLayout view_layout = DataViewLayout(s.env, view);
    if (typed_layout.status != napi_ok || typed_layout.length != 6 || typed_layout.offset != 4 ||
        view_layout.status != napi_ok || view_layout.length != 8 || view_layout.offset != 2) {
      ++wrong;
    }
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }
  EXPECT_EQ(wrong, 0);
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
}

// The serdes binding unofficial_napi_create_serdes_binding returns is still
// the binding while the caller makes more values.
TEST_F(Test33TypedArray, SerdesBindingOutlivesTheCallThatMadeIt) {
  EnvScope s(runtime_.get());
  napi_value serdes = nullptr;
  ASSERT_EQ(unofficial_napi_create_serdes_binding(s.env, &serdes), napi_ok);
  SetGlobal(s.env, "serdes", serdes);
  napi_value kinds =
      RunBufferScript(s.env, "typeof serdes.Serializer + ' ' + typeof serdes.Deserializer");
  char text[64] = {};
  size_t length = 0;
  ASSERT_EQ(napi_get_value_string_utf8(s.env, kinds, text, sizeof(text), &length), napi_ok);
  EXPECT_STREQ(text, "function function");
}

// The serdes binding's Serializer copies a view's own bytes: neither what a
// script redefined on DataView.prototype nor a length-tracking view's length
// from before its buffer grew.
TEST_F(Test33TypedArray, SerializerWritesAViewsOwnBytes) {
  EnvScope s(runtime_.get());
  napi_value serdes = nullptr;
  ASSERT_EQ(unofficial_napi_create_serdes_binding(s.env, &serdes), napi_ok);
  SetGlobal(s.env, "serdes", serdes);
  napi_value released = RunBufferScript(
      s.env,
      "(() => {"
      "  globalThis.calls = 0;"
      "  const backing = new ArrayBuffer(16);"
      "  new Uint8Array(backing).forEach((_, i, a) => { a[i] = i; });"
      "  const view = new DataView(backing, 2, 4);"
      "  const rab = new ArrayBuffer(4, { maxByteLength: 16 });"
      "  const tracking = new Uint8Array(rab, 1);"
      "  rab.resize(8);"
      "  new Uint8Array(rab).forEach((_, i, a) => { a[i] = 100 + i; });"
      "  const other = new ArrayBuffer(64);"
      "  for (const name of ['byteLength', 'byteOffset', 'buffer'])"
      "    Object.defineProperty(DataView.prototype, name, { configurable: true,"
      "      get() { calls++; return name === 'buffer' ? other : 1; } });"
      "  const serializer = new serdes.Serializer();"
      "  serializer.writeRawBytes(view);"
      "  serializer.writeRawBytes(tracking);"
      "  return serializer.releaseBuffer();"
      "})()");
  ASSERT_NE(released, nullptr);
  void* data = nullptr;
  size_t length = 0;
  ASSERT_EQ(napi_get_buffer_info(s.env, released, &data, &length), napi_ok);
  const std::vector<uint8_t> expected = {2, 3, 4, 5, 101, 102, 103, 104, 105, 106, 107};
  const auto* bytes = static_cast<const uint8_t*>(data);
  EXPECT_EQ(std::vector<uint8_t>(bytes, bytes + length), expected);
  EXPECT_EQ(GlobalInt(s.env, "calls"), 0) << "a getter the script defined ran";
}

// napi_is_detached_arraybuffer, like the other info calls over a detached or
// out-of-bounds buffer, leaves an exception that was pending as it was and
// adds none.
TEST_F(Test33TypedArray, InfoCallsKeepThePendingException) {
  EnvScope s(runtime_.get());
  RunBufferScript(s.env,
                  "globalThis.detached = new ArrayBuffer(8);"
                  "globalThis.overDetached = new Uint8Array(detached, 2, 4);"
                  "globalThis.viewOverDetached = new DataView(detached, 2, 4);"
                  "globalThis.rab = new ArrayBuffer(16, { maxByteLength: 16 });"
                  "globalThis.outOfBounds = new Uint8Array(rab, 4, 8);"
                  "globalThis.viewOutOfBounds = new DataView(rab, 4, 8);"
                  "rab.resize(4);"
                  "0");
  napi_value detached = NamedGlobal(s.env, "detached");
  napi_value buffer = nullptr;
  ASSERT_EQ(node_api_create_buffer_from_arraybuffer(s.env, detached, 0, 8, &buffer), napi_ok);
  ASSERT_EQ(napi_detach_arraybuffer(s.env, detached), napi_ok);
  napi_value over_detached = NamedGlobal(s.env, "overDetached");
  napi_value view_over_detached = NamedGlobal(s.env, "viewOverDetached");
  napi_value out_of_bounds = NamedGlobal(s.env, "outOfBounds");
  napi_value view_out_of_bounds = NamedGlobal(s.env, "viewOutOfBounds");

  bool is_detached = false;
  void* data = nullptr;
  size_t length = 99;
  const std::pair<const char*, std::function<napi_status()>> calls[] = {
      {"napi_is_detached_arraybuffer",
       [&] {
         is_detached = false;
         napi_status status = napi_is_detached_arraybuffer(s.env, detached, &is_detached);
         EXPECT_TRUE(is_detached);
         return status;
       }},
      {"napi_get_arraybuffer_info",
       [&] { return napi_get_arraybuffer_info(s.env, detached, &data, &length); }},
      {"typed array over a detached buffer",
       [&] { return TypedArrayLayout(s.env, over_detached).status; }},
      {"typed array out of bounds", [&] { return TypedArrayLayout(s.env, out_of_bounds).status; }},
      {"buffer over a detached buffer, as a typed array",
       [&] { return TypedArrayLayout(s.env, buffer).status; }},
      {"buffer over a detached buffer", [&] { return BufferLayout(s.env, buffer).status; }},
      {"data view over a detached buffer",
       [&] { return DataViewLayout(s.env, view_over_detached).status; }},
      {"data view out of bounds",
       [&] { return DataViewLayout(s.env, view_out_of_bounds).status; }},
  };
  for (const auto& [what, call] : calls) {
    SCOPED_TRACE(what);
    // With nothing pending, nothing is pending afterwards.
    EXPECT_EQ(call(), napi_ok);
    ExpectNothingPending(s.env, "with nothing pending before");

    // With an exception pending, it is the one pending afterwards.
    napi_throw_error(s.env, nullptr, "earlier");
    EXPECT_EQ(call(), napi_ok);
    napi_value exception = nullptr;
    EXPECT_EQ(napi_get_and_clear_last_exception(s.env, &exception), napi_ok)
        << "the earlier exception is no longer pending";
    char text[64] = {};
    size_t text_length = 0;
    napi_value message = nullptr;
    if (exception != nullptr &&
        napi_get_named_property(s.env, exception, "message", &message) == napi_ok) {
      napi_get_value_string_utf8(s.env, message, text, sizeof(text), &text_length);
    }
    EXPECT_STREQ(text, "earlier");
    ExpectNothingPending(s.env, "after the earlier exception was taken");
  }
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
