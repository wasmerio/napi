#ifdef NAPI_MODULE
#undef NAPI_MODULE
#endif
#include "node_api.h"

#include "test_env.h"
#include "upstream_js_test.h"

#include <cstdint>

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

// Finalizers at release. As in Node, every finalizer still pending when the
// env is released runs while the engine is alive, in the env's realm, and can
// use N-API; the instance-data finalizer runs last.

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

TEST_F(Test41InstanceData, ValueTheInstanceDataFinalizerMakesIsNeverFinalized) {
  EnvScope s(runtime_.get());
  int clock = 0;
  Late late{&clock};
  MarkEnvGlobal(s.env);
  ASSERT_EQ(napi_set_instance_data(s.env, &late, FinalizeInstanceDataMakingAValue, nullptr),
            napi_ok);

  Release(s);
  ExpectUsedNapi(late.instance_data, "instance data");
  // Nothing finalizes after the instance data, as in Node: the engine is
  // gone by the time the value is freed.
  EXPECT_EQ(late.made.runs, 0);
}
