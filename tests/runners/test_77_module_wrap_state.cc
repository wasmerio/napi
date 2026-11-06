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
#include <vector>

class Test77ModuleWrapState : public FixtureTestBase {};

namespace {

// Module states, as unofficial_napi_module_wrap_get_state reports them.
constexpr int32_t kUninstantiated = 0;
constexpr int32_t kInstantiated = 2;
constexpr int32_t kEvaluated = 4;

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

std::string ValueText(napi_env env, napi_value value) {
  napi_value string = nullptr;
  char text[256] = {};
  size_t length = 0;
  if (value == nullptr || napi_coerce_to_string(env, value, &string) != napi_ok ||
      napi_get_value_string_utf8(env, string, text, sizeof(text), &length) != napi_ok) {
    return "<unprintable>";
  }
  return std::string(text, length);
}

std::string RunText(napi_env env, const char* source) {
  napi_value result = nullptr;
  if (napi_run_script(env, Str(env, source), &result) != napi_ok) return "<threw>";
  return ValueText(env, result);
}

// Takes the pending exception: its message, or "" when none was pending.
std::string TakePendingMessage(napi_env env) {
  if (!ExceptionPending(env)) return "";
  napi_value error = nullptr;
  napi_value message = nullptr;
  if (napi_get_and_clear_last_exception(env, &error) != napi_ok) return "<not taken>";
  if (napi_get_named_property(env, error, "message", &message) != napi_ok) return "<no message>";
  return ValueText(env, message);
}

// How `promise` settles once the microtasks have run: "resolved", or
// "rejected: <message>".
std::string PromiseOutcome(napi_env env, napi_value promise) {
  napi_value global = nullptr;
  if (promise == nullptr || napi_get_global(env, &global) != napi_ok ||
      napi_set_named_property(env, global, "settling", promise) != napi_ok) {
    return "<not observed>";
  }
  RunText(env,
      "globalThis.outcome = 'pending';"
      "Promise.resolve(settling).then(() => { outcome = 'resolved'; },"
      "  (e) => { outcome = 'rejected: ' + (e && e.message); });"
      "0");
  EXPECT_EQ(unofficial_napi_event_loop_checkpoint(
                env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr),
            napi_ok);
  return RunText(env, "outcome");
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

// Evaluating a module that is not instantiated instantiates it first, as
// QuickJS's JS_EvaluateModule does: with nothing to link it to, the module
// runs; with a request it was not linked for, the evaluation fails as
// instantiating it does and the module stays uninstantiated. V8's Evaluate
// takes an instantiated module only, and the V8 provider aborted the process
// here.
TEST_F(Test77ModuleWrapState, EvaluatingAModuleNotInstantiatedInstantiatesItFirst) {
  EnvScope s(runtime_.get());
  for (bool sync : {false, true}) {
    SCOPED_TRACE(sync ? "evaluate_sync" : "evaluate");
    ASSERT_EQ(RunText(s.env, "globalThis.runs = 0"), "0");
    unofficial_napi_module module = CreateSourceTextModule(
        s.env, "globalThis.runs++; export const value = 42;");
    ASSERT_NE(module, nullptr);
    napi_value result = nullptr;
    if (sync) {
      ASSERT_EQ(unofficial_napi_module_wrap_evaluate_sync(s.env, module, nullptr, nullptr, &result),
                napi_ok);
      napi_value value = nullptr;
      ASSERT_EQ(napi_get_named_property(s.env, result, "value", &value), napi_ok);
      EXPECT_EQ(ValueText(s.env, value), "42");
    } else {
      ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, module, -1, false, &result), napi_ok);
      EXPECT_EQ(PromiseOutcome(s.env, result), "resolved");
    }
    EXPECT_EQ(Status(s.env, module), kEvaluated);
    EXPECT_EQ(RunText(s.env, "runs"), "1");
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);

    unofficial_napi_module unlinked = CreateSourceTextModule(
        s.env, "import 'missing'; globalThis.runs++;");
    ASSERT_NE(unlinked, nullptr);
    result = nullptr;
    const napi_status status =
        sync ? unofficial_napi_module_wrap_evaluate_sync(s.env, unlinked, nullptr, nullptr, &result)
             : unofficial_napi_module_wrap_evaluate(s.env, unlinked, -1, false, &result);
    EXPECT_EQ(status, napi_pending_exception);
    EXPECT_NE(TakePendingMessage(s.env), "");
    EXPECT_EQ(Status(s.env, unlinked), kUninstantiated);
    EXPECT_EQ(RunText(s.env, "runs"), "1");
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, unlinked), napi_ok);
  }
}

namespace {

constexpr int32_t kEvaluating = 3;

// Takes the pending exception: "<code>: <message>", or "" when none was
// pending.
std::string TakePendingCodeAndMessage(napi_env env) {
  if (!ExceptionPending(env)) return "";
  napi_value error = nullptr;
  napi_value code = nullptr;
  napi_value message = nullptr;
  if (napi_get_and_clear_last_exception(env, &error) != napi_ok) return "<not taken>";
  if (napi_get_named_property(env, error, "code", &code) != napi_ok ||
      napi_get_named_property(env, error, "message", &message) != napi_ok) {
    return "<unreadable>";
  }
  return ValueText(env, code) + ": " + ValueText(env, message);
}

enum class Reentry { kEvaluate, kEvaluateSync, kFacade };

struct ReentryProbe {
  Reentry call = Reentry::kEvaluate;
  unofficial_napi_module module = nullptr;
  int calls = 0;
  int32_t status_inside = -1;
  napi_status status = napi_ok;
  std::string pending;
};

// Evaluates the probe's module from inside its own evaluation, and takes
// what that left pending so that the evaluation goes on.
napi_value EvaluateAgain(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  EXPECT_EQ(napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data), napi_ok);
  auto* probe = static_cast<ReentryProbe*>(data);
  probe->calls++;
  probe->status_inside = Status(env, probe->module);
  napi_value result = nullptr;
  switch (probe->call) {
    case Reentry::kEvaluate:
      probe->status = unofficial_napi_module_wrap_evaluate(env, probe->module, -1, false, &result);
      break;
    case Reentry::kEvaluateSync:
      probe->status = unofficial_napi_module_wrap_evaluate_sync(
          env, probe->module, nullptr, nullptr, &result);
      break;
    case Reentry::kFacade:
      probe->status = unofficial_napi_module_wrap_create_required_module_facade(
          env, probe->module, &result);
      break;
  }
  probe->pending = TakePendingCodeAndMessage(env);
  return nullptr;
}

}  // namespace

// Evaluating a module from inside its own evaluation (its body, or a
// synthetic module's steps) is refused with ERR_VM_MODULE_STATUS, as Node's
// vm refuses a module whose status is "evaluating", and the evaluation under
// way goes on. The same holds for the require() facade, which evaluates the
// module it wraps. V8 aborted the process (a CHECK in Module::Evaluate).
// QuickJS gave the evaluation's promise, unsettled, to evaluate, failed
// evaluate_sync with ERR_REQUIRE_ASYNC_MODULE, and made the facade over
// bindings the body had not reached yet.
TEST_F(Test77ModuleWrapState, EvaluatingAModuleWhileItEvaluatesIsRefused) {
  for (bool synthetic : {false, true}) {
    for (Reentry call : {Reentry::kEvaluate, Reentry::kEvaluateSync, Reentry::kFacade}) {
      SCOPED_TRACE(std::string(synthetic ? "synthetic, " : "source text, ") +
                   (call == Reentry::kEvaluate       ? "evaluate"
                    : call == Reentry::kEvaluateSync ? "evaluate_sync"
                                                     : "facade"));
      EnvScope s(runtime_.get());
      ReentryProbe probe;
      probe.call = call;
      napi_value fn = nullptr;
      napi_value global = nullptr;
      ASSERT_EQ(napi_create_function(s.env, "evaluateAgain", NAPI_AUTO_LENGTH, EvaluateAgain,
                                     &probe, &fn),
                napi_ok);
      ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
      ASSERT_EQ(napi_set_named_property(s.env, global, "evaluateAgain", fn), napi_ok);
      ASSERT_EQ(RunText(s.env, "globalThis.after = 'no'"), "no");
      probe.module =
          synthetic ? CreateSyntheticModule(
                          s.env, "(function() { evaluateAgain(); globalThis.after = 'yes'; })")
                    : CreateSourceTextModule(s.env,
                                             "evaluateAgain(); globalThis.after = 'yes';\n"
                                             "export default 1;");
      ASSERT_NE(probe.module, nullptr);
      ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, probe.module, 0, nullptr), napi_ok);
      ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, probe.module), napi_ok);

      napi_value promise = nullptr;
      ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, probe.module, -1, false, &promise),
                napi_ok);
      EXPECT_EQ(probe.calls, 1);
      EXPECT_EQ(probe.status_inside, kEvaluating);
      EXPECT_EQ(probe.status, napi_pending_exception);
      EXPECT_EQ(probe.pending, "ERR_VM_MODULE_STATUS: Module status must not be evaluating");
      EXPECT_EQ(PromiseOutcome(s.env, promise), "resolved");
      EXPECT_EQ(RunText(s.env, "after"), "yes");
      EXPECT_EQ(Status(s.env, probe.module), kEvaluated);
      EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, probe.module), napi_ok);
    }
  }
}

namespace {

struct InstantiationProbe {
  unofficial_napi_module module = nullptr;
  unofficial_napi_module other = nullptr;
  bool entered = false;
  std::vector<std::string> outcomes;
};

std::string Outcome(napi_env env, napi_status status) {
  const std::string pending = TakePendingCodeAndMessage(env);
  return std::to_string(status) + (pending.empty() ? "" : " " + pending);
}

// Makes the calls that would start an instantiation, and two that would not,
// on the module being instantiated and on another one; records what each
// returned and threw.
napi_value CallsDuringInstantiation(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  EXPECT_EQ(napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data), napi_ok);
  auto* probe = static_cast<InstantiationProbe*>(data);
  // The errors thrown below run the setter too.
  if (probe->entered) return nullptr;
  probe->entered = true;
  napi_value result = nullptr;
  bool settled = false;
  auto& out = probe->outcomes;
  out.push_back("instantiate " +
                Outcome(env, unofficial_napi_module_wrap_instantiate(env, probe->module)));
  out.push_back("instantiate other " +
                Outcome(env, unofficial_napi_module_wrap_instantiate(env, probe->other)));
  out.push_back("evaluate " + Outcome(env, unofficial_napi_module_wrap_evaluate(
                                               env, probe->module, -1, false, &result)));
  out.push_back("evaluate_sync " + Outcome(env, unofficial_napi_module_wrap_evaluate_sync(
                                                    env, probe->module, nullptr, nullptr, &result)));
  out.push_back("facade " + Outcome(env, unofficial_napi_module_wrap_create_required_module_facade(
                                             env, probe->module, &result)));
  out.push_back("check " + Outcome(env, unofficial_napi_module_wrap_check_unsettled_top_level_await(
                                            env, probe->module, true, &settled)) +
                (settled ? " settled" : " unsettled"));
  out.push_back("namespace " +
                Outcome(env, unofficial_napi_module_wrap_get_namespace(env, probe->module, &result)));
  return nullptr;
}

}  // namespace

// No instantiation starts while one runs: instantiate, and evaluate or the
// facade on a module that is not instantiated, are refused with
// ERR_VM_MODULE_LINK_FAILURE. V8 runs script in the middle of an
// instantiation when a request does not resolve: the error it throws gets its
// `code` with a plain set, which runs an inherited setter. A second
// instantiation from there walked modules the first had only half prepared:
// instantiate and the facade crashed (SIGBUS), and evaluate aborted
// ("Expected instantiated module"). The asking calls answer as usual, and the
// failed instantiation leaves the module ready to link and instantiate.
TEST_F(Test77ModuleWrapState, NoInstantiationStartsWhileOneRuns) {
  EnvScope s(runtime_.get());
  InstantiationProbe probe;
  napi_value fn = nullptr;
  napi_value global = nullptr;
  ASSERT_EQ(napi_create_function(s.env, "callsDuringInstantiation", NAPI_AUTO_LENGTH,
                                 CallsDuringInstantiation, &probe, &fn),
            napi_ok);
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "callsDuringInstantiation", fn), napi_ok);
  ASSERT_EQ(RunText(s.env,
                    "Object.defineProperty(Error.prototype, 'code', {"
                    "  configurable: true,"
                    "  set(value) {"
                    "    callsDuringInstantiation();"
                    "    Object.defineProperty(this, 'code', {"
                    "      value, writable: true, configurable: true });"
                    "  },"
                    "}); 'set'"),
            "set");
  probe.module = CreateSourceTextModule(s.env, "import 'dependency'; export default 1;");
  probe.other = CreateSourceTextModule(s.env, "export const value = 1;");
  ASSERT_NE(probe.module, nullptr);
  ASSERT_NE(probe.other, nullptr);

  // Not linked: the request does not resolve.
  EXPECT_EQ(unofficial_napi_module_wrap_instantiate(s.env, probe.module), napi_pending_exception);
  EXPECT_NE(TakePendingMessage(s.env), "");
#if defined(NAPI_TEST_ENGINE_V8)
  // QuickJS reports the request through its own loader, an error with no
  // code, so no setter runs while it instantiates.
  EXPECT_TRUE(probe.entered);
#endif
  if (probe.entered) {
    const std::string refused =
        "10 ERR_VM_MODULE_LINK_FAILURE: A module instantiation is under way";
    EXPECT_EQ(probe.outcomes, (std::vector<std::string>{
                                  "instantiate " + refused,
                                  "instantiate other " + refused,
                                  "evaluate " + refused,
                                  "evaluate_sync " + refused,
                                  "facade " + refused,
                                  "check 0 settled",
                                  "namespace 10 ERR_MODULE_NOT_INSTANTIATED: "
                                  "Module is not instantiated",
                              }));
  }
  EXPECT_EQ(Status(s.env, probe.module), kUninstantiated);
  EXPECT_EQ(Status(s.env, probe.other), kUninstantiated);

  ASSERT_EQ(RunText(s.env, "delete Error.prototype.code; 'gone'"), "gone");
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, probe.other, 0, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, probe.module, 1, &probe.other), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, probe.module), napi_ok);
  EXPECT_EQ(Status(s.env, probe.module), kInstantiated);
  EXPECT_EQ(Status(s.env, probe.other), kInstantiated);
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, probe.module), napi_ok);
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, probe.other), napi_ok);
}

// set_export sets a synthetic module's export and takes no other module: for
// a source text module it returns napi_invalid_arg, as the host-JS provider
// does, and the module's exports stay its own. V8 aborted the process (an
// ApiCheck in SetSyntheticModuleExport). QuickJS crashed before the module
// was instantiated, and after that overwrote the module's binding.
TEST_F(Test77ModuleWrapState, SetExportTakesOnlySyntheticModules) {
  EnvScope s(runtime_.get());
  unofficial_napi_module module = CreateSourceTextModule(s.env, "export let value = 'one';");
  ASSERT_NE(module, nullptr);
  for (const char* name : {"value", "missing"}) {
    SCOPED_TRACE(name);
    EXPECT_EQ(unofficial_napi_module_wrap_set_export(s.env, module, Str(s.env, name),
                                                     Str(s.env, "two")),
              napi_invalid_arg);
    EXPECT_FALSE(ExceptionPending(s.env));
  }
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, module, 0, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, module), napi_ok);
  napi_value promise = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, module, -1, false, &promise), napi_ok);
  EXPECT_EQ(PromiseOutcome(s.env, promise), "resolved");
  for (const char* name : {"value", "missing"}) {
    SCOPED_TRACE(name);
    EXPECT_EQ(unofficial_napi_module_wrap_set_export(s.env, module, Str(s.env, name),
                                                     Str(s.env, "two")),
              napi_invalid_arg);
    EXPECT_FALSE(ExceptionPending(s.env));
  }
  napi_value ns = nullptr;
  napi_value value = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_get_namespace(s.env, module, &ns), napi_ok);
  ASSERT_EQ(napi_get_named_property(s.env, ns, "value", &value), napi_ok);
  EXPECT_EQ(ValueText(s.env, value), "one");
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);
}
