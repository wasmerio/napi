#ifdef NAPI_MODULE
#undef NAPI_MODULE
#endif
#include "node_api.h"

#include "test_env.h"
#include "upstream_js_test.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" napi_value Init(napi_env env, napi_value exports);

class Test41InstanceData : public FixtureTestBase {};

TEST_F(Test41InstanceData, PortedCoreFlow) {
  EnvScope s(runtime_.get());
  napi_value exports = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &exports), napi_ok);
  napi_value addon = Init(s.env, exports);
  ASSERT_NE(addon, nullptr);
  ASSERT_TRUE(InstallUpstreamJsShim(s, addon));
  ASSERT_TRUE(RunUpstreamJsFileNoMustCallVerification(
      s, std::string(NAPI_TESTS_ROOT_PATH) + "/js-native-api/test_instance_data/test.js"));
}

// The release contract (unofficial_napi_release_env). The cleanup hooks run,
// then the finalizer of every value still pending, most recent first, until
// neither is left; then the instance-data finalizer; and all of it again for
// whatever those leave pending, until nothing is. Each runs while the engine
// is alive, on the env's thread, in the env's realm, and can use N-API.
// (Node up to 20 also finalized the instance data last; Node 25 interleaves
// it with the other finalizers.)

namespace {

constexpr int32_t kEnvMarker = 41;

// What one finalizer saw. `clock` orders the finalizers of one env.
struct Seen {
  explicit Seen(int* clock) : clock(clock) {}
  int* clock;
  int runs = 0;
  int order = 0;
  napi_status open_scope = napi_generic_failure;
  napi_status get_global = napi_generic_failure;
  napi_status create_object = napi_generic_failure;
  napi_status close_scope = napi_generic_failure;
  bool saw_env_global = false;
};

// Uses N-API as a finalizer may: opens a handle scope, reads the global's
// marker and makes an object.
void UseNapi(napi_env env, Seen* seen) {
  ++seen->runs;
  seen->order = ++*seen->clock;
  napi_handle_scope scope = nullptr;
  seen->open_scope = napi_open_handle_scope(env, &scope);
  napi_value global = nullptr;
  seen->get_global = napi_get_global(env, &global);
  napi_value object = nullptr;
  seen->create_object = napi_create_object(env, &object);
  napi_value marker = nullptr;
  int32_t value = 0;
  seen->saw_env_global = seen->get_global == napi_ok &&
                         napi_get_named_property(env, global, "envMarker", &marker) == napi_ok &&
                         napi_get_value_int32(env, marker, &value) == napi_ok &&
                         value == kEnvMarker;
  if (seen->open_scope == napi_ok) seen->close_scope = napi_close_handle_scope(env, scope);
}

void ExpectUsedNapi(const Seen& seen, const char* what) {
  SCOPED_TRACE(what);
  EXPECT_EQ(seen.runs, 1);
  EXPECT_EQ(seen.open_scope, napi_ok);
  EXPECT_EQ(seen.get_global, napi_ok);
  EXPECT_EQ(seen.create_object, napi_ok);
  EXPECT_EQ(seen.close_scope, napi_ok);
  EXPECT_TRUE(seen.saw_env_global);
}

void FinalizeInstanceData(napi_env env, void* data, void*) {
  UseNapi(env, static_cast<Seen*>(data));
}

void FinalizeValue(node_api_basic_env env, void*, void* hint) {
  UseNapi(const_cast<napi_env>(env), static_cast<Seen*>(hint));
}

// Sets `name` on the env's global, which keeps `value` alive until release.
void Keep(napi_env env, const char* name, napi_value value) {
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(env, global, name, value), napi_ok);
}

// Marks the env's global, so a finalizer can tell it acts in the env's realm.
void MarkEnvGlobal(napi_env env) {
  napi_value marker = nullptr;
  ASSERT_EQ(napi_create_int32(env, kEnvMarker, &marker), napi_ok);
  Keep(env, "envMarker", marker);
}

// Makes an external whose finalizer records into `seen`, kept until release.
void KeepExternal(napi_env env, const char* name, Seen* seen) {
  napi_handle_scope scope = nullptr;
  ASSERT_EQ(napi_open_handle_scope(env, &scope), napi_ok);
  napi_value external = nullptr;
  ASSERT_EQ(napi_create_external(env, nullptr, FinalizeValue, seen, &external), napi_ok);
  Keep(env, name, external);
  ASSERT_EQ(napi_close_handle_scope(env, scope), napi_ok);
}

// Releases the env before the test ends.
void Release(EnvScope& s) {
  ASSERT_EQ(unofficial_napi_release_env(s.scope, nullptr), napi_ok);
  s.scope = nullptr;
  s.env = nullptr;
}

}  // namespace

TEST_F(Test41InstanceData, InstanceDataFinalizerUsesNapiAtRelease) {
  EnvScope s(runtime_.get());
  int clock = 0;
  Seen instance_data{&clock};
  MarkEnvGlobal(s.env);
  ASSERT_EQ(napi_set_instance_data(s.env, &instance_data, FinalizeInstanceData, nullptr),
            napi_ok);

  Release(s);
  ExpectUsedNapi(instance_data, "instance data");
}

TEST_F(Test41InstanceData, FinalizersOfLiveValuesRunBeforeTheInstanceDataFinalizer) {
  EnvScope s(runtime_.get());
  int clock = 0;
  Seen instance_data{&clock}, external{&clock}, array_buffer{&clock}, buffer{&clock},
      wrap{&clock}, finalizer{&clock};
  uint8_t array_buffer_bytes[8] = {};
  uint8_t buffer_bytes[8] = {};
  MarkEnvGlobal(s.env);

  // Each value is still reachable when the env is released.
  KeepExternal(s.env, "external", &external);
  napi_value value = nullptr;
  ASSERT_EQ(napi_create_external_arraybuffer(s.env, array_buffer_bytes, sizeof(array_buffer_bytes),
                                             FinalizeValue, &array_buffer, &value),
            napi_ok);
  Keep(s.env, "arrayBuffer", value);
  ASSERT_EQ(napi_create_external_buffer(s.env, sizeof(buffer_bytes), buffer_bytes, FinalizeValue,
                                        &buffer, &value),
            napi_ok);
  Keep(s.env, "buffer", value);
  ASSERT_EQ(napi_create_object(s.env, &value), napi_ok);
  ASSERT_EQ(napi_wrap(s.env, value, nullptr, FinalizeValue, &wrap, nullptr), napi_ok);
  Keep(s.env, "wrapped", value);
  ASSERT_EQ(napi_create_object(s.env, &value), napi_ok);
  ASSERT_EQ(napi_add_finalizer(s.env, value, nullptr, FinalizeValue, &finalizer, nullptr), napi_ok);
  Keep(s.env, "finalized", value);
  ASSERT_EQ(napi_set_instance_data(s.env, &instance_data, FinalizeInstanceData, nullptr),
            napi_ok);

  Release(s);
  ExpectUsedNapi(external, "external");
  ExpectUsedNapi(array_buffer, "external array buffer");
  ExpectUsedNapi(buffer, "external buffer");
  ExpectUsedNapi(wrap, "wrap");
  ExpectUsedNapi(finalizer, "napi_add_finalizer");
  ExpectUsedNapi(instance_data, "instance data");
  for (const Seen* seen : {&external, &array_buffer, &buffer, &wrap, &finalizer}) {
    EXPECT_LT(seen->order, instance_data.order);
  }
}

namespace {

// A value's finalizer, and the finalizer of a value it makes at release.
struct Chain {
  explicit Chain(int* clock) : first(clock), made(clock) {}
  Seen first;
  Seen made;
};

void FinalizeMakingAnother(node_api_basic_env basic_env, void*, void* hint) {
  auto env = const_cast<napi_env>(basic_env);
  auto* chain = static_cast<Chain*>(hint);
  UseNapi(env, &chain->first);
  KeepExternal(env, "made", &chain->made);
}

// The instance-data finalizer, and a value it makes at release.
struct Late {
  explicit Late(int* clock) : instance_data(clock), made(clock) {}
  Seen instance_data;
  Seen made;
};

void FinalizeInstanceDataMakingAValue(napi_env env, void* data, void*) {
  auto* late = static_cast<Late*>(data);
  UseNapi(env, &late->instance_data);
  KeepExternal(env, "made", &late->made);
}

}  // namespace

TEST_F(Test41InstanceData, ValueAFinalizerMakesAtReleaseIsFinalizedToo) {
  EnvScope s(runtime_.get());
  int clock = 0;
  Chain chain{&clock};
  Seen instance_data{&clock};
  MarkEnvGlobal(s.env);
  napi_value external = nullptr;
  ASSERT_EQ(napi_create_external(s.env, nullptr, FinalizeMakingAnother, &chain, &external),
            napi_ok);
  Keep(s.env, "external", external);
  ASSERT_EQ(napi_set_instance_data(s.env, &instance_data, FinalizeInstanceData, nullptr),
            napi_ok);

  Release(s);
  ExpectUsedNapi(chain.first, "external");
  ExpectUsedNapi(chain.made, "external its finalizer made");
  EXPECT_LT(chain.made.order, instance_data.order);
  ExpectUsedNapi(instance_data, "instance data");
}

TEST_F(Test41InstanceData, ValueTheInstanceDataFinalizerMakesIsFinalized) {
  EnvScope s(runtime_.get());
  int clock = 0;
  Late late{&clock};
  MarkEnvGlobal(s.env);
  ASSERT_EQ(napi_set_instance_data(s.env, &late, FinalizeInstanceDataMakingAValue, nullptr),
            napi_ok);

  Release(s);
  ExpectUsedNapi(late.instance_data, "instance data");
  // What the instance-data finalizer leaves pending is finalized after it, as
  // Node's FinalizeAll finalizes until its lists are empty.
  ExpectUsedNapi(late.made, "external the instance-data finalizer made");
  EXPECT_GT(late.made.order, late.instance_data.order);
}

namespace {

// What the instance-data finalizer leaves pending: a value, a cleanup hook and
// new instance data.
struct Rounds {
  explicit Rounds(int* clock) : first(clock), hook(clock), value(clock), second(clock) {}
  napi_env env = nullptr;
  Seen first;
  Seen hook;
  Seen value;
  Seen second;
  napi_status add_hook = napi_generic_failure;
  napi_status set_data = napi_generic_failure;
};

void RoundsHook(void* arg) {
  auto* rounds = static_cast<Rounds*>(arg);
  UseNapi(rounds->env, &rounds->hook);
}

void FinalizeSecondInstanceData(napi_env env, void* data, void*) {
  UseNapi(env, &static_cast<Rounds*>(data)->second);
}

void FinalizeFirstInstanceData(napi_env env, void* data, void*) {
  auto* rounds = static_cast<Rounds*>(data);
  UseNapi(env, &rounds->first);
  KeepExternal(env, "leftValue", &rounds->value);
  rounds->add_hook = napi_add_env_cleanup_hook(env, RoundsHook, rounds);
  rounds->set_data = napi_set_instance_data(env, rounds, FinalizeSecondInstanceData, nullptr);
}

}  // namespace

// The release repeats for what the instance-data finalizer leaves: its hook
// runs, then its value is finalized, then the instance data it set.
TEST_F(Test41InstanceData, WhatTheInstanceDataFinalizerLeavesIsReleasedInTheSameOrder) {
  EnvScope s(runtime_.get());
  int clock = 0;
  Rounds rounds{&clock};
  rounds.env = s.env;
  MarkEnvGlobal(s.env);
  ASSERT_EQ(napi_set_instance_data(s.env, &rounds, FinalizeFirstInstanceData, nullptr), napi_ok);

  Release(s);
  EXPECT_EQ(rounds.add_hook, napi_ok);
  EXPECT_EQ(rounds.set_data, napi_ok);
  ExpectUsedNapi(rounds.first, "first instance data");
  ExpectUsedNapi(rounds.hook, "hook it added");
  ExpectUsedNapi(rounds.value, "value it made");
  ExpectUsedNapi(rounds.second, "instance data it set");
  EXPECT_LT(rounds.first.order, rounds.hook.order);
  EXPECT_LT(rounds.hook.order, rounds.value.order);
  EXPECT_LT(rounds.value.order, rounds.second.order);
}

namespace {

bool IsDetached(napi_env env, napi_ref ref) {
  napi_value value = nullptr;
  bool detached = false;
  return napi_get_reference_value(env, ref, &value) == napi_ok &&
         napi_is_detached_arraybuffer(env, value, &detached) == napi_ok && detached;
}

// Memory an external buffer was made over, which its finalizer frees. With
// `buffer` set, the finalizer records whether that array buffer was detached
// when it ran.
struct Owned {
  int frees = 0;
  napi_ref buffer = nullptr;
  bool detached_when_freed = false;
};

void FreeOwned(node_api_basic_env basic_env, void* data, void* hint) {
  auto* owned = static_cast<Owned*>(hint);
  ++owned->frees;
  if (owned->buffer != nullptr) {
    auto env = const_cast<napi_env>(basic_env);
    napi_handle_scope scope = nullptr;
    if (napi_open_handle_scope(env, &scope) == napi_ok) {
      owned->detached_when_freed = IsDetached(env, owned->buffer);
      napi_close_handle_scope(env, scope);
    }
  }
  std::free(data);
}

void* NewBytes(size_t length) {
  void* bytes = std::malloc(length);
  std::memset(bytes, 7, length);
  return bytes;
}

// Moves an array buffer's memory to a new one with ArrayBuffer.prototype.transfer.
napi_value Transfer(napi_env env, napi_value array_buffer) {
  napi_value transfer = nullptr;
  napi_value moved = nullptr;
  EXPECT_EQ(napi_get_named_property(env, array_buffer, "transfer", &transfer), napi_ok);
  EXPECT_EQ(napi_call_function(env, array_buffer, transfer, 0, nullptr, &moved), napi_ok);
  return moved;
}

}  // namespace

#if defined(NAPI_TEST_ENGINE_QUICKJS)
namespace {

// What the instance-data finalizer finds of external buffers whose finalizers
// freed their memory.
struct AfterFree {
  Owned array_buffer_memory;
  Owned buffer_memory;
  napi_ref array_buffer = nullptr;
  napi_ref view = nullptr;
  napi_ref buffer_array_buffer = nullptr;
  int frees_seen = 0;
  bool array_buffer_detached = false;
  napi_status info_status = napi_generic_failure;
  void* info_data = nullptr;
  size_t info_length = SIZE_MAX;
  napi_valuetype element = napi_number;
  bool buffer_detached = false;
};

void InspectFreedBuffers(napi_env env, void* data, void*) {
  auto* after = static_cast<AfterFree*>(data);
  after->frees_seen = after->array_buffer_memory.frees + after->buffer_memory.frees;
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) return;
  after->array_buffer_detached = IsDetached(env, after->array_buffer);
  after->buffer_detached = IsDetached(env, after->buffer_array_buffer);
  napi_value value = nullptr;
  if (napi_get_reference_value(env, after->array_buffer, &value) == napi_ok)
    after->info_status =
        napi_get_arraybuffer_info(env, value, &after->info_data, &after->info_length);
  napi_value element = nullptr;
  if (napi_get_reference_value(env, after->view, &value) == napi_ok &&
      napi_get_element(env, value, 3, &element) == napi_ok)
    napi_typeof(env, element, &after->element);
  napi_close_handle_scope(env, scope);
}

}  // namespace

// As Node's CallbackInfo::Clean does, an external buffer is detached before
// its finalizer frees the memory, so code that runs later at release sees it
// detached, never the freed memory. The V8 provider does not detach them yet.
TEST_F(Test41InstanceData, ExternalBuffersAreDetachedBeforeTheirFinalizersAtRelease) {
  EnvScope s(runtime_.get());
  AfterFree after;
  napi_value array_buffer = nullptr;
  ASSERT_EQ(napi_create_external_arraybuffer(s.env, NewBytes(64), 64, FreeOwned,
                                             &after.array_buffer_memory, &array_buffer),
            napi_ok);
  napi_value view = nullptr;
  ASSERT_EQ(napi_create_typedarray(s.env, napi_uint8_array, 64, array_buffer, 0, &view), napi_ok);
  Keep(s.env, "view", view);
  napi_value buffer = nullptr;
  ASSERT_EQ(napi_create_external_buffer(s.env, 16, NewBytes(16), FreeOwned, &after.buffer_memory,
                                        &buffer),
            napi_ok);
  Keep(s.env, "buffer", buffer);
  napi_value buffer_array_buffer = nullptr;
  ASSERT_EQ(napi_get_typedarray_info(s.env, buffer, nullptr, nullptr, nullptr,
                                     &buffer_array_buffer, nullptr),
            napi_ok);
  ASSERT_EQ(napi_create_reference(s.env, array_buffer, 1, &after.array_buffer), napi_ok);
  ASSERT_EQ(napi_create_reference(s.env, view, 1, &after.view), napi_ok);
  ASSERT_EQ(napi_create_reference(s.env, buffer_array_buffer, 1, &after.buffer_array_buffer),
            napi_ok);
  after.array_buffer_memory.buffer = after.array_buffer;
  after.buffer_memory.buffer = after.buffer_array_buffer;
  ASSERT_EQ(napi_set_instance_data(s.env, &after, InspectFreedBuffers, nullptr), napi_ok);

  Release(s);
  EXPECT_EQ(after.frees_seen, 2);
  EXPECT_EQ(after.array_buffer_memory.frees, 1);
  EXPECT_EQ(after.buffer_memory.frees, 1);
  EXPECT_TRUE(after.array_buffer_memory.detached_when_freed);
  EXPECT_TRUE(after.buffer_memory.detached_when_freed);
  EXPECT_TRUE(after.array_buffer_detached);
  // A detached array buffer has no data and a byte length of 0, as in Node.
  EXPECT_EQ(after.info_status, napi_ok);
  EXPECT_EQ(after.info_data, nullptr);
  EXPECT_EQ(after.info_length, 0u);
  EXPECT_EQ(after.element, napi_undefined);
  EXPECT_TRUE(after.buffer_detached);
}
#endif

// ArrayBuffer.prototype.transfer moves an external buffer's memory to a new
// buffer. Its finalizer still runs once at release, whether the buffer it was
// made as is alive then or not.
TEST_F(Test41InstanceData, TransferredExternalArrayBufferIsFinalizedOnceAtRelease) {
  EnvScope s(runtime_.get());
  Owned kept_memory;
  Owned dropped_memory;
  napi_value array_buffer = nullptr;
  ASSERT_EQ(napi_create_external_arraybuffer(s.env, NewBytes(32), 32, FreeOwned, &kept_memory,
                                             &array_buffer),
            napi_ok);
  Keep(s.env, "original", array_buffer);
  Keep(s.env, "moved", Transfer(s.env, array_buffer));
  napi_handle_scope scope = nullptr;
  ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
  ASSERT_EQ(napi_create_external_arraybuffer(s.env, NewBytes(32), 32, FreeOwned, &dropped_memory,
                                             &array_buffer),
            napi_ok);
  Keep(s.env, "movedFromDropped", Transfer(s.env, array_buffer));
  ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);

  Release(s);
  EXPECT_EQ(kept_memory.frees, 1);
  EXPECT_EQ(dropped_memory.frees, 1);
}

namespace {

// A finalizer that makes an external array buffer at release.
struct BufferChain {
  explicit BufferChain(int* clock) : first(clock), made(clock) {}
  Seen first;
  Seen made;
  uint8_t bytes[8] = {};
};

void FinalizeMakingAnArrayBuffer(node_api_basic_env basic_env, void*, void* hint) {
  auto env = const_cast<napi_env>(basic_env);
  auto* chain = static_cast<BufferChain*>(hint);
  UseNapi(env, &chain->first);
  napi_handle_scope scope = nullptr;
  ASSERT_EQ(napi_open_handle_scope(env, &scope), napi_ok);
  napi_value made = nullptr;
  ASSERT_EQ(napi_create_external_arraybuffer(env, chain->bytes, sizeof(chain->bytes),
                                             FinalizeValue, &chain->made, &made),
            napi_ok);
  Keep(env, "madeBuffer", made);
  ASSERT_EQ(napi_close_handle_scope(env, scope), napi_ok);
}

}  // namespace

TEST_F(Test41InstanceData, ExternalArrayBufferAFinalizerMakesAtReleaseIsFinalizedToo) {
  EnvScope s(runtime_.get());
  int clock = 0;
  BufferChain chain{&clock};
  Seen instance_data{&clock};
  MarkEnvGlobal(s.env);
  napi_value external = nullptr;
  ASSERT_EQ(napi_create_external(s.env, nullptr, FinalizeMakingAnArrayBuffer, &chain, &external),
            napi_ok);
  Keep(s.env, "external", external);
  ASSERT_EQ(napi_set_instance_data(s.env, &instance_data, FinalizeInstanceData, nullptr),
            napi_ok);

  Release(s);
  ExpectUsedNapi(chain.first, "external");
  ExpectUsedNapi(instance_data, "instance data");
  ExpectUsedNapi(chain.made, "array buffer its finalizer made");
  EXPECT_LT(chain.made.order, instance_data.order);
}

namespace {

// Cleanup hooks a hook and finalizers add at release, and when each ran.
struct LateHooks {
  napi_env env = nullptr;
  int clock = 0;
  int registered_hook = 0;
  int hook_hook = 0;
  int value = 0;
  int value_hook = 0;
  int instance_data = 0;
  int instance_data_hook = 0;
  napi_status hook_add = napi_generic_failure;
  napi_status value_add = napi_generic_failure;
  napi_status instance_data_add = napi_generic_failure;
};

void HookHook(void* arg) {
  auto* hooks = static_cast<LateHooks*>(arg);
  hooks->hook_hook = ++hooks->clock;
}

void RegisteredHook(void* arg) {
  auto* hooks = static_cast<LateHooks*>(arg);
  hooks->registered_hook = ++hooks->clock;
  hooks->hook_add = napi_add_env_cleanup_hook(hooks->env, HookHook, hooks);
}

void ValueHook(void* arg) {
  auto* hooks = static_cast<LateHooks*>(arg);
  hooks->value_hook = ++hooks->clock;
}

void InstanceDataHook(void* arg) {
  auto* hooks = static_cast<LateHooks*>(arg);
  hooks->instance_data_hook = ++hooks->clock;
}

void FinalizeAddingAHook(node_api_basic_env env, void*, void* hint) {
  auto* hooks = static_cast<LateHooks*>(hint);
  hooks->value = ++hooks->clock;
  hooks->value_add = napi_add_env_cleanup_hook(env, ValueHook, hooks);
}

void FinalizeInstanceDataAddingAHook(napi_env env, void* data, void*) {
  auto* hooks = static_cast<LateHooks*>(data);
  hooks->instance_data = ++hooks->clock;
  hooks->instance_data_add = napi_add_env_cleanup_hook(env, InstanceDataHook, hooks);
}

}  // namespace

// As in Node, a hook added during the release still runs. The hooks run
// before the values are finalized, and a hook a value's finalizer adds runs
// before the instance-data finalizer.
TEST_F(Test41InstanceData, CleanupHooksAddedAtReleaseRun) {
  EnvScope s(runtime_.get());
  LateHooks hooks;
  hooks.env = s.env;
  ASSERT_EQ(napi_add_env_cleanup_hook(s.env, RegisteredHook, &hooks), napi_ok);
  napi_value external = nullptr;
  ASSERT_EQ(napi_create_external(s.env, nullptr, FinalizeAddingAHook, &hooks, &external), napi_ok);
  Keep(s.env, "external", external);
  ASSERT_EQ(napi_set_instance_data(s.env, &hooks, FinalizeInstanceDataAddingAHook, nullptr),
            napi_ok);

  Release(s);
  EXPECT_EQ(hooks.hook_add, napi_ok);
  EXPECT_EQ(hooks.value_add, napi_ok);
  EXPECT_EQ(hooks.instance_data_add, napi_ok);
  EXPECT_EQ(hooks.registered_hook, 1);
  EXPECT_EQ(hooks.hook_hook, 2);
  EXPECT_EQ(hooks.value, 3);
  EXPECT_EQ(hooks.value_hook, 4);
  EXPECT_EQ(hooks.instance_data, 5);
  EXPECT_EQ(hooks.instance_data_hook, 6);
}

namespace {

// Exceptions thrown at release by a hook, a value's finalizer and the
// instance-data finalizer, and whether the next one to run found one pending.
struct Thrown {
  napi_env env = nullptr;
  napi_status throw_status = napi_generic_failure;
  bool pending_in_value = true;
  bool pending_in_instance_data = true;
  bool pending_in_late_hook = true;
  int checks = 0;
};

void Check(napi_env env, Thrown* thrown, bool* pending) {
  if (napi_is_exception_pending(env, pending) == napi_ok) ++thrown->checks;
}

void ThrowingHook(void* arg) {
  auto* thrown = static_cast<Thrown*>(arg);
  napi_throw_error(thrown->env, nullptr, "hook");
}

void FinalizeThrowing(node_api_basic_env basic_env, void*, void* hint) {
  auto env = const_cast<napi_env>(basic_env);
  auto* thrown = static_cast<Thrown*>(hint);
  Check(env, thrown, &thrown->pending_in_value);
  thrown->throw_status = napi_throw_error(env, nullptr, "finalizer");
}

void LateHookAfterAThrow(void* arg) {
  auto* thrown = static_cast<Thrown*>(arg);
  Check(thrown->env, thrown, &thrown->pending_in_late_hook);
}

void FinalizeInstanceDataAfterAThrow(napi_env env, void* data, void*) {
  auto* thrown = static_cast<Thrown*>(data);
  Check(env, thrown, &thrown->pending_in_instance_data);
  napi_add_env_cleanup_hook(env, LateHookAfterAThrow, thrown);
  napi_throw_error(env, nullptr, "instance data");
}

}  // namespace

// Nothing can catch what a hook or finalizer throws at release, so, as Node's
// CallIntoModule does once JS can no longer run, it is dropped.
TEST_F(Test41InstanceData, AnExceptionThrownAtReleaseIsNotLeftPending) {
  EnvScope s(runtime_.get());
  Thrown thrown;
  thrown.env = s.env;
  ASSERT_EQ(napi_add_env_cleanup_hook(s.env, ThrowingHook, &thrown), napi_ok);
  napi_value external = nullptr;
  ASSERT_EQ(napi_create_external(s.env, nullptr, FinalizeThrowing, &thrown, &external), napi_ok);
  Keep(s.env, "external", external);
  ASSERT_EQ(napi_set_instance_data(s.env, &thrown, FinalizeInstanceDataAfterAThrow, nullptr),
            napi_ok);

  Release(s);
  // Node's napi_throw_error returns napi_ok; the V8 provider returns
  // napi_pending_exception. Either way the error was thrown.
  EXPECT_TRUE(thrown.throw_status == napi_ok || thrown.throw_status == napi_pending_exception)
      << thrown.throw_status;
  EXPECT_EQ(thrown.checks, 3);
  EXPECT_FALSE(thrown.pending_in_value);
  EXPECT_FALSE(thrown.pending_in_instance_data);
  EXPECT_FALSE(thrown.pending_in_late_hook);
}

namespace {

// A wrapped parent whose native object holds the only reference to a wrapped
// child. With `free_children` its finalizer deletes the reference, which
// frees the child.
struct Parent {
  napi_ref child = nullptr;
};

struct Family {
  bool free_children = false;
  int parents = 0;
  int children = 0;
};

void FinalizeChild(node_api_basic_env, void*, void* hint) {
  ++static_cast<Family*>(hint)->children;
}

void FinalizeParent(node_api_basic_env env, void* data, void* hint) {
  auto* parent = static_cast<Parent*>(data);
  auto* family = static_cast<Family*>(hint);
  ++family->parents;
  if (family->free_children) {
    napi_delete_reference(env, parent->child);
    parent->child = nullptr;
  }
}

// Releases an env holding `count` parents and their children, and returns
// how long the release took, in milliseconds.
double ReleaseFamilies(NapiTestRuntime* runtime, int count, Family* family) {
  EnvScope s(runtime);
  std::vector<Parent> parents(count);
  napi_value array = nullptr;
  EXPECT_EQ(napi_create_array(s.env, &array), napi_ok);
  Keep(s.env, "parents", array);
  // Children first: the parents, made last, are finalized first, so each
  // frees a child whose finalizer is still pending.
  for (int i = 0; i < count; ++i) {
    napi_handle_scope scope = nullptr;
    EXPECT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value child = nullptr;
    EXPECT_EQ(napi_create_object(s.env, &child), napi_ok);
    EXPECT_EQ(napi_wrap(s.env, child, nullptr, FinalizeChild, family, nullptr), napi_ok);
    EXPECT_EQ(napi_create_reference(s.env, child, 1, &parents[i].child), napi_ok);
    EXPECT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }
  for (int i = 0; i < count; ++i) {
    napi_handle_scope scope = nullptr;
    EXPECT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value parent = nullptr;
    EXPECT_EQ(napi_create_object(s.env, &parent), napi_ok);
    EXPECT_EQ(napi_wrap(s.env, parent, &parents[i], FinalizeParent, family, nullptr), napi_ok);
    EXPECT_EQ(napi_set_element(s.env, array, i, parent), napi_ok);
    EXPECT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }

  const auto start = std::chrono::steady_clock::now();
  Release(s);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::milli>(elapsed).count();
}

}  // namespace

// The release pass takes time linear in the values it finalizes, also when
// each finalizer frees another value.
TEST_F(Test41InstanceData, ReleaseFinalizesValuesThatFreeOthersInLinearTime) {
  constexpr int kCount = 40000;
  Family keeping;
  const double keeping_ms = ReleaseFamilies(runtime_.get(), kCount, &keeping);
  Family freeing;
  freeing.free_children = true;
  const double freeing_ms = ReleaseFamilies(runtime_.get(), kCount, &freeing);

  for (const Family* family : {&keeping, &freeing}) {
    EXPECT_EQ(family->parents, kCount);
    EXPECT_EQ(family->children, kCount);
  }
  // Both are linear. A pass that rescanned every value after each one freed
  // took over a hundred times as long.
  EXPECT_LT(freeing_ms, 20 * keeping_ms + 50)
      << "keeping " << keeping_ms << " ms, freeing " << freeing_ms << " ms";
}
