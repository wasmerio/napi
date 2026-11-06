// The module calls made when the module is not in the state they expect: not
// instantiated yet, or in the middle of its evaluation. Each returns an
// answer or an error. The V8 provider aborted the process on several of them
// (the V8 API checks the state with a fatal ApiCheck or CHECK).
#ifdef NAPI_MODULE
#undef NAPI_MODULE
#endif
#include "node_api.h"

#include "test_env.h"

#include "unofficial_napi.h"

#include <string>

class Test77ModuleWrapState : public FixtureTestBase {};

namespace {

// Module states, as unofficial_napi_module_wrap_get_state reports them.
constexpr int32_t kUninstantiated = 0;
constexpr int32_t kInstantiated = 2;

napi_value Str(napi_env env, const char* value) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out) != napi_ok) return nullptr;
  return out;
}

unofficial_napi_module CreateSourceTextModule(napi_env env, const char* text) {
  napi_value wrapper = nullptr;
  napi_value undefined = nullptr;
  EXPECT_EQ(napi_create_object(env, &wrapper), napi_ok);
  EXPECT_EQ(napi_get_undefined(env, &undefined), napi_ok);
  const unofficial_napi_js_source source = unofficial_napi_js_source_from_text(Str(env, text));
  unofficial_napi_module_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  options.kind = unofficial_napi_module_source_text;
  options.wrapper = wrapper;
  options.url = Str(env, "state.mjs");
  options.payload.source_text.source = &source;
  options.payload.source_text.host_defined_option_id = undefined;
  unofficial_napi_module_create_result created{};
  EXPECT_EQ(unofficial_napi_module_wrap_create(env, &options, &created), napi_ok);
  return created.module;
}

// `steps` is the source of the evaluation steps function.
unofficial_napi_module CreateSyntheticModule(napi_env env, const char* steps) {
  napi_value wrapper = nullptr;
  napi_value names = nullptr;
  napi_value steps_value = nullptr;
  EXPECT_EQ(napi_create_object(env, &wrapper), napi_ok);
  EXPECT_EQ(napi_run_script(env, Str(env, "['value']"), &names), napi_ok);
  EXPECT_EQ(napi_run_script(env, Str(env, steps), &steps_value), napi_ok);
  unofficial_napi_module_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  options.kind = unofficial_napi_module_synthetic;
  options.wrapper = wrapper;
  options.url = Str(env, "synthetic");
  options.payload.synthetic.export_names = names;
  options.payload.synthetic.synthetic_evaluation_steps = steps_value;
  unofficial_napi_module_create_result created{};
  EXPECT_EQ(unofficial_napi_module_wrap_create(env, &options, &created), napi_ok);
  return created.module;
}

int32_t Status(napi_env env, unofficial_napi_module module) {
  int32_t status = -1;
  EXPECT_EQ(unofficial_napi_module_wrap_get_state(env, module, &status, nullptr, nullptr),
            napi_ok);
  return status;
}

bool ExceptionPending(napi_env env) {
  bool pending = false;
  EXPECT_EQ(napi_is_exception_pending(env, &pending), napi_ok);
  return pending;
}

}  // namespace

// A module that is not instantiated has no top-level await under way: the
// check reports it settled, as QuickJS does and as Node's check (made at exit
// on the entry module) would. V8's IsGraphAsync takes an instantiated module
// only, and the V8 provider aborted the process here.
TEST_F(Test77ModuleWrapState, CheckUnsettledTopLevelAwaitOfAModuleNotInstantiatedIsSettled) {
  EnvScope s(runtime_.get());
  for (const char* text : {"export const value = 1;", "await 0; export const value = 1;"}) {
    SCOPED_TRACE(text);
    unofficial_napi_module module = CreateSourceTextModule(s.env, text);
    ASSERT_NE(module, nullptr);
    for (bool warnings : {false, true}) {
      bool settled = false;
      EXPECT_EQ(unofficial_napi_module_wrap_check_unsettled_top_level_await(
                    s.env, module, warnings, &settled),
                napi_ok);
      EXPECT_TRUE(settled);
      EXPECT_FALSE(ExceptionPending(s.env));
    }
    // Linked, not instantiated yet.
    ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, module, 0, nullptr), napi_ok);
    bool settled = false;
    EXPECT_EQ(unofficial_napi_module_wrap_check_unsettled_top_level_await(
                  s.env, module, true, &settled),
              napi_ok);
    EXPECT_TRUE(settled);
    EXPECT_EQ(Status(s.env, module), kUninstantiated);
    // Instantiated, not evaluated: nothing is under way either.
    ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, module), napi_ok);
    settled = false;
    EXPECT_EQ(unofficial_napi_module_wrap_check_unsettled_top_level_await(
                  s.env, module, true, &settled),
              napi_ok);
    EXPECT_TRUE(settled);
    EXPECT_EQ(Status(s.env, module), kInstantiated);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);
  }

  unofficial_napi_module synthetic = CreateSyntheticModule(s.env, "(function() {})");
  ASSERT_NE(synthetic, nullptr);
  bool settled = false;
  EXPECT_EQ(unofficial_napi_module_wrap_check_unsettled_top_level_await(
                s.env, synthetic, true, &settled),
            napi_ok);
  EXPECT_TRUE(settled);
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, synthetic), napi_ok);
}
