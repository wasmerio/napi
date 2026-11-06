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

// The default flags, and source-phase imports (`import source`), which V8
// parses only behind a flag.
class Test77ModuleWrapState : public FixtureTestBase {
 protected:
  static void SetUpTestSuite() {
    static constexpr char kFlags[] = "--expose-gc --js-float16array --js-source-phase-imports";
    unofficial_napi_runtime_options options{};
    options.size = sizeof(options);
    options.version = UNOFFICIAL_NAPI_RUNTIME_OPTIONS_VERSION;
    options.engine_flags = kFlags;
    options.engine_flags_length = sizeof(kFlags) - 1;
    ASSERT_EQ(unofficial_napi_configure_runtime(&options), napi_ok);
  }
  static void TearDownTestSuite() {}
};

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

unofficial_napi_module CreateSourceTextModule(napi_env env, const char* text,
                                              unofficial_napi_realm realm = nullptr,
                                              napi_value* wrapper_out = nullptr) {
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
  options.realm_or_null = realm;
  if (wrapper_out != nullptr) *wrapper_out = wrapper;
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

namespace {

// Takes the pending exception: its constructor's name, or "" when none was
// pending.
std::string TakePendingErrorName(napi_env env) {
  if (!ExceptionPending(env)) return "";
  napi_value error = nullptr;
  napi_value name = nullptr;
  if (napi_get_and_clear_last_exception(env, &error) != napi_ok) return "<not taken>";
  if (napi_get_named_property(env, error, "name", &name) != napi_ok) return "<no name>";
  return ValueText(env, name);
}

}  // namespace

// Setting an export the synthetic module does not have (yet) throws a
// ReferenceError: one of its export names before the module is
// instantiated, which makes its bindings, or a name it never had. Before,
// V8 returned napi_generic_failure and left the ReferenceError it threw to
// whatever JS was on the stack, if any, instead of N-API. QuickJS crashed
// before the module was instantiated.
TEST_F(Test77ModuleWrapState, SetExportOfABindingThatDoesNotExistThrows) {
  EnvScope s(runtime_.get());
  unofficial_napi_module module = CreateSyntheticModule(s.env, "(function() {})");
  ASSERT_NE(module, nullptr);
  for (const char* name : {"value", "missing"}) {
    SCOPED_TRACE(name);
    EXPECT_EQ(unofficial_napi_module_wrap_set_export(s.env, module, Str(s.env, name),
                                                     Str(s.env, "early")),
              napi_pending_exception);
    EXPECT_EQ(TakePendingErrorName(s.env), "ReferenceError");
  }
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, module, 0, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, module), napi_ok);
  EXPECT_EQ(unofficial_napi_module_wrap_set_export(s.env, module, Str(s.env, "missing"),
                                                   Str(s.env, "late")),
            napi_pending_exception);
  EXPECT_EQ(TakePendingErrorName(s.env), "ReferenceError");
  EXPECT_EQ(unofficial_napi_module_wrap_set_export(s.env, module, Str(s.env, "value"),
                                                   Str(s.env, "set")),
            napi_ok);
  EXPECT_FALSE(ExceptionPending(s.env));
  napi_value promise = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, module, -1, false, &promise), napi_ok);
  EXPECT_EQ(PromiseOutcome(s.env, promise), "resolved");
  napi_value ns = nullptr;
  napi_value value = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_get_namespace(s.env, module, &ns), napi_ok);
  ASSERT_EQ(napi_get_named_property(s.env, ns, "value", &value), napi_ok);
  EXPECT_EQ(ValueText(s.env, value), "set");
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);
}

// The namespace of a module that is not instantiated is refused with
// ERR_MODULE_NOT_INSTANTIATED on both engines, as the V8 provider already
// did (V8's GetModuleNamespace takes an instantiated module only). QuickJS
// built the namespace over bindings that do not exist yet and crashed.
TEST_F(Test77ModuleWrapState, NamespaceOfAModuleNotInstantiatedIsRefused) {
  EnvScope s(runtime_.get());
  for (bool synthetic : {false, true}) {
    SCOPED_TRACE(synthetic ? "synthetic" : "source text");
    unofficial_napi_module module =
        synthetic ? CreateSyntheticModule(s.env, "(function() {})")
                  : CreateSourceTextModule(s.env, "export const value = 1;");
    ASSERT_NE(module, nullptr);
    for (bool linked : {false, true}) {
      SCOPED_TRACE(linked ? "linked" : "not linked");
      if (linked) {
        ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, module, 0, nullptr), napi_ok);
      }
      napi_value ns = nullptr;
      EXPECT_EQ(unofficial_napi_module_wrap_get_namespace(s.env, module, &ns),
                napi_pending_exception);
      EXPECT_EQ(TakePendingCodeAndMessage(s.env),
                "ERR_MODULE_NOT_INSTANTIATED: Module is not instantiated");
    }
    ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, module), napi_ok);
    napi_value ns = nullptr;
    EXPECT_EQ(unofficial_napi_module_wrap_get_namespace(s.env, module, &ns), napi_ok);
    EXPECT_NE(ns, nullptr);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);
  }
}

// The require() facade instantiates the module it wraps, if needed, with
// the modules that module was linked to, as instantiate does and as on
// QuickJS. When the facade cannot be made, because the module does not
// instantiate or has no default export to re-export, the call fails with
// that error pending in N-API. V8 resolved the module's own requests with
// the facade's resolver, which knows only 'original': a module that was not
// instantiated yet did not instantiate. And V8 returned
// napi_pending_exception with nothing pending: the error went to whatever
// JS was on the stack, or was lost, or (from the facade's resolver) was
// never thrown.
TEST_F(Test77ModuleWrapState, RequiredModuleFacadeInstantiatesOrLeavesItsError) {
  EnvScope s(runtime_.get());
  unofficial_napi_module dependency = CreateSourceTextModule(s.env, "export const value = 2;");
  unofficial_napi_module linked =
      CreateSourceTextModule(s.env, "import { value } from 'dependency'; export default value;");
  ASSERT_NE(dependency, nullptr);
  ASSERT_NE(linked, nullptr);
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, dependency, 0, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, linked, 1, &dependency), napi_ok);
  napi_value facade = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_create_required_module_facade(s.env, linked, &facade),
            napi_ok);
  napi_value default_value = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, facade, "default", &default_value), napi_ok);
  EXPECT_EQ(ValueText(s.env, default_value), "2");
  EXPECT_EQ(Status(s.env, linked), kEvaluated);
  EXPECT_EQ(Status(s.env, dependency), kEvaluated);
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, linked), napi_ok);
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, dependency), napi_ok);

  unofficial_napi_module unlinked =
      CreateSourceTextModule(s.env, "import 'missing'; export default 1;");
  ASSERT_NE(unlinked, nullptr);
  napi_value ns = nullptr;
  EXPECT_EQ(unofficial_napi_module_wrap_create_required_module_facade(s.env, unlinked, &ns),
            napi_pending_exception);
  EXPECT_NE(TakePendingMessage(s.env), "");
  EXPECT_EQ(Status(s.env, unlinked), kUninstantiated);
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, unlinked), napi_ok);

  unofficial_napi_module no_default = CreateSourceTextModule(s.env, "export const value = 1;");
  ASSERT_NE(no_default, nullptr);
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, no_default, 0, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, no_default), napi_ok);
  napi_value promise = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, no_default, -1, false, &promise), napi_ok);
  EXPECT_EQ(PromiseOutcome(s.env, promise), "resolved");
  ns = nullptr;
  EXPECT_EQ(unofficial_napi_module_wrap_create_required_module_facade(s.env, no_default, &ns),
            napi_pending_exception);
  EXPECT_EQ(TakePendingErrorName(s.env), "SyntaxError");
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, no_default), napi_ok);
}

// Evaluating a module that imports a synthetic module whose evaluation steps
// are running (from those steps: a CommonJS module that requires an ES module
// importing it) is refused with ERR_VM_MODULE_STATUS on both engines, as
// Node refuses such a cycle (ERR_REQUIRE_CYCLE_MODULE), whether the module is
// instantiated already or not. The same holds for the require() facade over
// it. V8 aborted the process: it evaluates the synthetic module through
// Module::Evaluate, whose CHECK rejects one that is evaluating. QuickJS ran
// the module against the synthetic module's exports as they were. Once the
// steps are done, the module evaluates.
TEST_F(Test77ModuleWrapState, EvaluatingAModuleThatImportsAnEvaluatingSyntheticModuleIsRefused) {
  for (bool instantiated : {true, false}) {
    for (Reentry call : {Reentry::kEvaluate, Reentry::kEvaluateSync, Reentry::kFacade}) {
      SCOPED_TRACE(std::string(instantiated ? "instantiated, " : "linked, ") +
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
      ASSERT_EQ(RunText(s.env, "globalThis.after = 'no'; globalThis.ran = 'no'"), "no");
      unofficial_napi_module synthetic = CreateSyntheticModule(
          s.env, "(function() { evaluateAgain(); globalThis.after = 'yes'; })");
      probe.module = CreateSourceTextModule(
          s.env, "import { value } from 'synthetic'; globalThis.ran = 'yes'; export default 1;");
      ASSERT_NE(synthetic, nullptr);
      ASSERT_NE(probe.module, nullptr);
      ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, synthetic, 0, nullptr), napi_ok);
      ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, probe.module, 1, &synthetic), napi_ok);
      if (instantiated) {
        ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, probe.module), napi_ok);
      } else {
        ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, synthetic), napi_ok);
      }

      napi_value promise = nullptr;
      ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, synthetic, -1, false, &promise),
                napi_ok);
      EXPECT_EQ(probe.calls, 1);
      EXPECT_EQ(probe.status, napi_pending_exception);
      EXPECT_EQ(probe.pending,
                "ERR_VM_MODULE_STATUS: A synthetic module it imports is evaluating");
      EXPECT_EQ(PromiseOutcome(s.env, promise), "resolved");
      EXPECT_EQ(RunText(s.env, "after"), "yes");
      EXPECT_EQ(RunText(s.env, "ran"), "no");
      EXPECT_EQ(Status(s.env, synthetic), kEvaluated);

      ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, probe.module, -1, false, &promise),
                napi_ok);
      EXPECT_EQ(PromiseOutcome(s.env, promise), "resolved");
      EXPECT_EQ(RunText(s.env, "ran"), "yes");
      EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, probe.module), napi_ok);
      EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, synthetic), napi_ok);
    }
  }
}

// No link starts while one runs, on QuickJS too: instantiate, and evaluate or
// the facade on a module that is not linked, are refused with
// ERR_VM_MODULE_LINK_FAILURE. QuickJS runs script in the middle of a link
// when it throws there (a request that does not resolve, an import the
// requested module does not export): the error gets its stack through an
// Error.prepareStackTrace getter. Evaluating the module being linked crashed
// (SIGSEGV), and a second link took the module from "linking" to "linked"
// under the first. The asking calls answer as usual, and the failed link
// leaves the modules ready to link and instantiate.
TEST_F(Test77ModuleWrapState, NoLinkStartsWhileOneRunsFromPrepareStackTrace) {
  for (bool resolves : {false, true}) {
    SCOPED_TRACE(resolves ? "missing export" : "unresolved request");
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
                      "Object.defineProperty(Error, 'prepareStackTrace', {"
                      "  configurable: true,"
                      "  get() { callsDuringInstantiation(); return undefined; },"
                      "}); 'set'"),
              "set");
    unofficial_napi_module dependency = CreateSourceTextModule(s.env, "export const value = 1;");
    probe.module = CreateSourceTextModule(
        s.env, "import { missing } from 'dependency'; export default missing;");
    probe.other = CreateSourceTextModule(s.env, "export const value = 1;");
    ASSERT_NE(dependency, nullptr);
    ASSERT_NE(probe.module, nullptr);
    ASSERT_NE(probe.other, nullptr);
    ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, dependency, 0, nullptr), napi_ok);
    if (resolves) {
      ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, probe.module, 1, &dependency), napi_ok);
    }

    EXPECT_EQ(unofficial_napi_module_wrap_instantiate(s.env, probe.module),
              napi_pending_exception);
    EXPECT_NE(TakePendingMessage(s.env), "");
#if defined(NAPI_TEST_ENGINE_QUICKJS)
    // V8 reads Error.prepareStackTrace only when a stack is read, so no
    // script runs while it instantiates here.
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

    ASSERT_EQ(RunText(s.env, "delete Error.prepareStackTrace; 'gone'"), "gone");
    ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, probe.other, 0, nullptr), napi_ok);
    unofficial_napi_module linked = CreateSourceTextModule(
        s.env, "import { value } from 'other'; export default value;");
    ASSERT_NE(linked, nullptr);
    ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, linked, 1, &probe.other), napi_ok);
    ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, linked), napi_ok);
    EXPECT_EQ(Status(s.env, linked), kInstantiated);
    EXPECT_EQ(Status(s.env, probe.other), kInstantiated);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, linked), napi_ok);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, probe.module), napi_ok);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, probe.other), napi_ok);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, dependency), napi_ok);
  }
}

namespace {

// "ok: <default export>" when the module evaluates, through instantiate and
// evaluate or through the require() facade, else the error's message.
std::string DefaultExportOf(napi_env env, unofficial_napi_module module, bool facade) {
  napi_value ns = nullptr;
  if (facade) {
    if (unofficial_napi_module_wrap_create_required_module_facade(env, module, &ns) != napi_ok) {
      return TakePendingMessage(env);
    }
  } else {
    napi_value promise = nullptr;
    if (unofficial_napi_module_wrap_instantiate(env, module) != napi_ok ||
        unofficial_napi_module_wrap_evaluate(env, module, -1, false, &promise) != napi_ok) {
      return TakePendingMessage(env);
    }
    const std::string outcome = PromiseOutcome(env, promise);
    if (outcome != "resolved") return outcome;
    if (unofficial_napi_module_wrap_get_namespace(env, module, &ns) != napi_ok) {
      return TakePendingMessage(env);
    }
  }
  napi_value value = nullptr;
  if (napi_get_named_property(env, ns, "default", &value) != napi_ok) return "<no default>";
  return "ok: " + ValueText(env, value);
}

}  // namespace

// The require() facade instantiates a module that has a source-phase import
// (`import source`) as instantiate does: its source object is the one set on
// the module it was linked to. V8 instantiated the facade's graph with no
// source resolver and crashed (SIGSEGV) calling it. QuickJS fails to
// instantiate such a module at all, through either path, the same way.
TEST_F(Test77ModuleWrapState, RequiredModuleFacadeResolvesSourcePhaseImports) {
  EnvScope s(runtime_.get());
  unofficial_napi_module dependency = CreateSourceTextModule(s.env, "export const value = 1;");
  ASSERT_NE(dependency, nullptr);
  napi_value source_object = nullptr;
  ASSERT_EQ(napi_run_script(s.env, Str(s.env, "({ marker: 'its source' })"), &source_object),
            napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_set_module_source_object(s.env, dependency, source_object),
            napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, dependency, 0, nullptr), napi_ok);
  std::string outcomes[2];
  for (bool facade : {false, true}) {
    unofficial_napi_module module = CreateSourceTextModule(
        s.env, "import source dependency from 'dependency'; export default dependency.marker;");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, module, 1, &dependency), napi_ok);
    outcomes[facade] = DefaultExportOf(s.env, module, facade);
    EXPECT_FALSE(ExceptionPending(s.env));
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, module), napi_ok);
  }
  EXPECT_EQ(outcomes[1], outcomes[0]);
#if defined(NAPI_TEST_ENGINE_V8)
  EXPECT_EQ(outcomes[0], "ok: its source");
#endif
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, dependency), napi_ok);
}

// The require() facade gives no namespace for a module whose evaluation
// throws (the error is pending, as from evaluate_sync) or awaits (it fails
// with ERR_REQUIRE_ASYNC_MODULE), on both engines, as QuickJS did. Node
// checks that the evaluation is fulfilled before it returns the namespace.
// V8 returned napi_ok and the facade's namespace in both cases.
TEST_F(Test77ModuleWrapState, RequiredModuleFacadeOfAModuleThatThrowsOrAwaitsFails) {
  EnvScope s(runtime_.get());
  unofficial_napi_module throws =
      CreateSourceTextModule(s.env, "export default 1; throw new Error('boom');");
  unofficial_napi_module awaits =
      CreateSourceTextModule(s.env, "export default 1; await new Promise(() => {});");
  ASSERT_NE(throws, nullptr);
  ASSERT_NE(awaits, nullptr);
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, throws, 0, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, awaits, 0, nullptr), napi_ok);

  napi_value ns = nullptr;
  EXPECT_EQ(unofficial_napi_module_wrap_create_required_module_facade(s.env, throws, &ns),
            napi_pending_exception);
  EXPECT_EQ(ns, nullptr);
  EXPECT_EQ(TakePendingMessage(s.env), "boom");
  EXPECT_EQ(Status(s.env, throws), 5);

  ns = nullptr;
  EXPECT_EQ(unofficial_napi_module_wrap_create_required_module_facade(s.env, awaits, &ns),
            napi_pending_exception);
  EXPECT_EQ(ns, nullptr);
  const std::string pending = TakePendingCodeAndMessage(s.env);
  EXPECT_EQ(pending.rfind("ERR_REQUIRE_ASYNC_MODULE: require() cannot be used on an ESM graph "
                          "with top-level await",
                          0),
            0u)
      << pending;
  EXPECT_FALSE(ExceptionPending(s.env));
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, throws), napi_ok);
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, awaits), napi_ok);
}

namespace {

// Instantiates, from inside a module's evaluation, that module and then one
// that imports it; records what each call returned and threw, and the status
// each module had after it.
napi_value InstantiateDuringEvaluation(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  EXPECT_EQ(napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data), napi_ok);
  auto* probe = static_cast<InstantiationProbe*>(data);
  if (probe->entered) return nullptr;
  probe->entered = true;
  probe->outcomes.push_back(
      "self " + Outcome(env, unofficial_napi_module_wrap_instantiate(env, probe->module)) +
      ", status " + std::to_string(Status(env, probe->module)));
  probe->outcomes.push_back(
      "importer " + Outcome(env, unofficial_napi_module_wrap_instantiate(env, probe->other)) +
      ", status " + std::to_string(Status(env, probe->other)));
  return nullptr;
}

}  // namespace

// Instantiating a module from inside its own evaluation (its body, or a
// synthetic module's steps) does nothing, as on V8: the module is linked
// already and goes on evaluating. QuickJS linked it again and took it from
// "evaluating" back to "linked" (its linker asserts against such a module).
// Instantiating a module that imports one being evaluated is engine-specific:
// V8 takes the evaluating module as linked and instantiates it; QuickJS's
// linker cannot link through one (it took it to "linked" too), so the
// QuickJS provider refuses it with ERR_VM_MODULE_STATUS. Either way it
// instantiates once the evaluation is done.
TEST_F(Test77ModuleWrapState, InstantiatingAModuleWhileItEvaluates) {
  for (bool synthetic : {true, false}) {
    SCOPED_TRACE(synthetic ? "synthetic" : "source text");
    EnvScope s(runtime_.get());
    InstantiationProbe probe;
    napi_value fn = nullptr;
    napi_value global = nullptr;
    ASSERT_EQ(napi_create_function(s.env, "instantiateDuringEvaluation", NAPI_AUTO_LENGTH,
                                   InstantiateDuringEvaluation, &probe, &fn),
              napi_ok);
    ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, global, "instantiateDuringEvaluation", fn), napi_ok);
    probe.module =
        synthetic ? CreateSyntheticModule(s.env, "(function() { instantiateDuringEvaluation(); })")
                  : CreateSourceTextModule(s.env,
                                           "instantiateDuringEvaluation(); export const value = 1;");
    probe.other = CreateSourceTextModule(
        s.env, "import { value } from 'evaluating'; export default value;");
    ASSERT_NE(probe.module, nullptr);
    ASSERT_NE(probe.other, nullptr);
    ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, probe.module, 0, nullptr), napi_ok);
    ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, probe.other, 1, &probe.module), napi_ok);
    ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, probe.module), napi_ok);

    napi_value promise = nullptr;
    ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, probe.module, -1, false, &promise),
              napi_ok);
    EXPECT_EQ(PromiseOutcome(s.env, promise), "resolved");
    ASSERT_TRUE(probe.entered);
    ASSERT_EQ(probe.outcomes.size(), 2u);
    EXPECT_EQ(probe.outcomes[0], "self 0, status 3");
#if defined(NAPI_TEST_ENGINE_V8)
    EXPECT_EQ(probe.outcomes[1], "importer 0, status 2");
#else
    EXPECT_EQ(probe.outcomes[1],
              "importer 10 ERR_VM_MODULE_STATUS: A module it imports is evaluating, status 0");
#endif
    EXPECT_EQ(Status(s.env, probe.module), kEvaluated);

    ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, probe.other), napi_ok);
    ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, probe.other, -1, false, &promise),
              napi_ok);
    EXPECT_EQ(PromiseOutcome(s.env, promise), "resolved");
    EXPECT_EQ(Status(s.env, probe.other), kEvaluated);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, probe.other), napi_ok);
    EXPECT_EQ(unofficial_napi_module_wrap_destroy(s.env, probe.module), napi_ok);
  }
}

TEST_F(Test77ModuleWrapState, RelinkingAnInstantiatedModuleIsRefused) {
  EnvScope s(runtime_.get());
  auto importer = CreateSourceTextModule(s.env, "import {value} from 'dep'; export {value};");
  auto first = CreateSourceTextModule(s.env, "export const value = 'first';");
  auto second = CreateSourceTextModule(s.env, "export const value = 'second';");
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, importer, 1, &first), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_instantiate(s.env, importer), napi_ok);
  EXPECT_EQ(unofficial_napi_module_wrap_link(s.env, importer, 1, &second), napi_pending_exception);
  EXPECT_EQ(TakePendingMessage(s.env), "Module is already linked");
  napi_value ns = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_evaluate_sync(s.env, importer, nullptr, nullptr, &ns), napi_ok);
  napi_value value = nullptr;
  ASSERT_EQ(napi_get_named_property(s.env, ns, "value", &value), napi_ok);
  EXPECT_EQ(ValueText(s.env, value), "first");
}

TEST_F(Test77ModuleWrapState, DestroyedDependencyCannotBecomeAnotherModule) {
  EnvScope s(runtime_.get());
  auto importer = CreateSourceTextModule(s.env, "import {value} from 'dep'; globalThis.result = value;");
  auto dependency = CreateSourceTextModule(s.env, "export const value = 'original';");
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, importer, 1, &dependency), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_destroy(s.env, dependency), napi_ok);
  auto unrelated = CreateSourceTextModule(s.env, "globalThis.wrongModuleRan = true; export const value = 'wrong';");
  ASSERT_NE(unrelated, nullptr);
  napi_value promise = nullptr;
#if defined(NAPI_TEST_ENGINE_V8)
  EXPECT_EQ(unofficial_napi_module_wrap_evaluate(s.env, importer, -1, false, &promise), napi_pending_exception);
  EXPECT_FALSE(TakePendingMessage(s.env).empty());
#else
  // QuickJS keeps the actual linked JSModuleDef alive independently of its record.
  EXPECT_EQ(unofficial_napi_module_wrap_evaluate(s.env, importer, -1, false, &promise), napi_ok);
  EXPECT_EQ(RunText(s.env, "result"), "original");
#endif
  EXPECT_EQ(RunText(s.env, "typeof wrongModuleRan"), "undefined");
}

namespace {
struct DestroyingModule { unofficial_napi_module module = nullptr; };
napi_value DestroyAndTerminateModule(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  auto* state = static_cast<DestroyingModule*>(data);
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(env, state->module), napi_ok);
  EXPECT_EQ(unofficial_napi_terminate_execution(env), napi_ok);
  return nullptr;
}
}

TEST_F(Test77ModuleWrapState, DestroyDuringTerminatedEvaluationKeepsRecordAliveUntilReturn) {
  EnvScope s(runtime_.get());
  DestroyingModule state;
  napi_value global = nullptr, fn = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_create_function(s.env, "stop", NAPI_AUTO_LENGTH, DestroyAndTerminateModule, &state, &fn), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "stop", fn), napi_ok);
  for (bool facade : {false, true}) {
    state.module = CreateSourceTextModule(s.env, "stop(); export default 42;");
    napi_value result = nullptr;
    EXPECT_EQ(facade
        ? unofficial_napi_module_wrap_create_required_module_facade(s.env, state.module, &result)
        : unofficial_napi_module_wrap_evaluate(s.env, state.module, -1, false, &result), napi_pending_exception);
    EXPECT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
    EXPECT_FALSE(ExceptionPending(s.env));
    EXPECT_EQ(RunText(s.env, "'still usable'"), "still usable");
  }
}

TEST_F(Test77ModuleWrapState, PendingTopLevelAwaitHasEvaluatedStatusAndIsUnsettled) {
  EnvScope s(runtime_.get());
  auto module = CreateSourceTextModule(s.env, "await new Promise(() => {}); export default 1;");
  napi_value promise = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, module, -1, false, &promise), napi_ok);
  EXPECT_EQ(Status(s.env, module), kEvaluated);
  bool settled = true;
  ASSERT_EQ(unofficial_napi_module_wrap_check_unsettled_top_level_await(s.env, module, false, &settled), napi_ok);
  EXPECT_FALSE(settled);
  napi_value again = nullptr;
  EXPECT_EQ(unofficial_napi_module_wrap_evaluate(s.env, module, -1, false, &again), napi_ok);
}

TEST_F(Test77ModuleWrapState, FacadeDoesNotRunWhileTerminating) {
  EnvScope s(runtime_.get());
  auto module = CreateSourceTextModule(s.env, "globalThis.facadeRan = true; export default 42;");
  ASSERT_EQ(unofficial_napi_terminate_execution(s.env), napi_ok);
  napi_value ns = nullptr;
  EXPECT_EQ(unofficial_napi_module_wrap_create_required_module_facade(s.env, module, &ns), napi_pending_exception);
  EXPECT_EQ(ns, nullptr);
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  EXPECT_EQ(RunText(s.env, "typeof facadeRan"), "undefined");
}

#if defined(NAPI_TEST_ENGINE_QUICKJS)
TEST_F(Test77ModuleWrapState, ModuleErrorCodeBypassesInheritedSetter) {
  EnvScope s(runtime_.get());
  ASSERT_EQ(RunText(s.env, "Object.defineProperty(Error.prototype, 'code', {set() {throw Error('setter');}, configurable: true}); 'ok'"), "ok");
  auto module = CreateSourceTextModule(s.env, "export default 1;");
  EXPECT_EQ(unofficial_napi_module_wrap_link(s.env, module, 1, nullptr), napi_pending_exception);
  EXPECT_EQ(TakePendingMessage(s.env), "linked modules array length mismatch");
  EXPECT_FALSE(ExceptionPending(s.env));
}

#endif

TEST_F(Test77ModuleWrapState, FacadeRunsInModulesRealmAndCrossRealmLinksAreRefused) {
  EnvScope s(runtime_.get());
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  unofficial_napi_realm realm = nullptr;
  napi_value global = nullptr;
  ASSERT_EQ(unofficial_napi_create_realm(s.env, &options, &realm, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "marker", Str(s.env, "module realm")), napi_ok);
  ASSERT_EQ(RunText(s.env, "globalThis.marker = 'env realm'"), "env realm");
  auto module = CreateSourceTextModule(s.env, "export default globalThis.marker;", realm);
  napi_value ns = nullptr, value = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_create_required_module_facade(s.env, module, &ns), napi_ok);
  ASSERT_EQ(napi_get_named_property(s.env, ns, "default", &value), napi_ok);
  EXPECT_EQ(ValueText(s.env, value), "module realm");
  auto importer = CreateSourceTextModule(s.env, "import value from 'dep'; export default value;");
  EXPECT_EQ(unofficial_napi_module_wrap_link(s.env, importer, 1, &module), napi_pending_exception);
  EXPECT_EQ(TakePendingMessage(s.env), "Linked modules must use the same realm");
}

namespace {
struct FinalizedModule { unofficial_napi_module module; int* finalized; };
void DestroyFinalizedModule(napi_env env, void* data, void*) {
  auto* state = static_cast<FinalizedModule*>(data);
  EXPECT_EQ(unofficial_napi_module_wrap_destroy(env, state->module), napi_ok);
  ++*state->finalized;
  delete state;
}
}

TEST_F(Test77ModuleWrapState, ModuleWrappersAndTheirDependencyCyclesCanBeCollected) {
  int finalized = 0;
  EnvScope s(runtime_.get());
  for (bool cycle : {false, true}) {
    SCOPED_TRACE(cycle);
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value a_wrapper = nullptr, b_wrapper = nullptr;
    auto a = CreateSourceTextModule(s.env, "import 'b'; export const a = 1;", nullptr, &a_wrapper);
    auto b = CreateSourceTextModule(s.env, cycle ? "import 'a'; export const b = 2;" : "export const b = 2;", nullptr, &b_wrapper);
    ASSERT_EQ(napi_add_finalizer(s.env, a_wrapper, new FinalizedModule{a, &finalized}, DestroyFinalizedModule, nullptr, nullptr), napi_ok);
    ASSERT_EQ(napi_add_finalizer(s.env, b_wrapper, new FinalizedModule{b, &finalized}, DestroyFinalizedModule, nullptr, nullptr), napi_ok);
    ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, a, 1, &b), napi_ok);
    ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, b, cycle ? 1 : 0, cycle ? &a : nullptr), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
    for (int attempt = 0; attempt < 4 && finalized < (cycle ? 4 : 2); ++attempt) {
      ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
      ASSERT_EQ(unofficial_napi_event_loop_checkpoint(s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr), napi_ok);
    }
    EXPECT_EQ(finalized, cycle ? 4 : 2);
  }
}

TEST_F(Test77ModuleWrapState, LiveImporterRetainsItsDependencyWrapper) {
  int finalized = 0;
  EnvScope s(runtime_.get());
  auto importer = CreateSourceTextModule(s.env, "import {value} from 'dep'; export {value};");
  napi_handle_scope scope = nullptr;
  ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
  napi_value wrapper = nullptr;
  auto dependency = CreateSourceTextModule(s.env, "export const value = 42;", nullptr, &wrapper);
  ASSERT_EQ(napi_add_finalizer(s.env, wrapper, new FinalizedModule{dependency, &finalized}, DestroyFinalizedModule, nullptr, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, importer, 1, &dependency), napi_ok);
  ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr), napi_ok);
  EXPECT_EQ(finalized, 0);
  napi_value ns = nullptr, value = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_evaluate_sync(s.env, importer, nullptr, nullptr, &ns), napi_ok);
  ASSERT_EQ(napi_get_named_property(s.env, ns, "value", &value), napi_ok);
  EXPECT_EQ(ValueText(s.env, value), "42");
}

namespace {
napi_value TerminateModule(napi_env env, napi_callback_info) {
  EXPECT_EQ(unofficial_napi_terminate_execution(env), napi_ok);
  return nullptr;
}
}

TEST_F(Test77ModuleWrapState, CancelledModuleErrorIsCatchableAndCheckpointExceptionCanBeTaken) {
  EnvScope s(runtime_.get());
  napi_value global = nullptr, stop = nullptr;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_create_function(s.env, "stop", NAPI_AUTO_LENGTH, TerminateModule, nullptr, &stop), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "stop", stop), napi_ok);
  auto module = CreateSourceTextModule(s.env, "stop(); export default 1;");
  napi_value promise = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, module, -1, false, &promise), napi_pending_exception);
  napi_value taken = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &taken), napi_ok);
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "taken", taken), napi_ok);
  EXPECT_EQ(RunText(s.env, "(() => {try {throw taken;} catch(e) {return 'caught';}})()"), "caught");
  napi_value error = nullptr;
  ASSERT_EQ(unofficial_napi_module_wrap_get_state(s.env, module, nullptr, &error, nullptr), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, global, "moduleError", error), napi_ok);
  EXPECT_EQ(RunText(s.env, "(() => {try {throw moduleError;} catch(e) {return 'caught';}})()"), "caught");
#if defined(NAPI_TEST_ENGINE_QUICKJS)
  ASSERT_EQ(unofficial_napi_module_wrap_evaluate(s.env, module, -1, false, &promise), napi_ok);
  EXPECT_EQ(PromiseOutcome(s.env, promise).substr(0, 8), "rejected");
#endif
  EXPECT_FALSE(ExceptionPending(s.env));
}

#if defined(NAPI_TEST_ENGINE_V8)
TEST_F(Test77ModuleWrapState, CachedDataPropagatesBufferExceptionsAndRefusesTermination) {
  EnvScope s(runtime_.get());
  auto module = CreateSourceTextModule(s.env, "export default 1;");
  ASSERT_EQ(RunText(s.env, "globalThis.Buffer = function Buffer() {}; Buffer.from = () => {throw Error('buffer failure');}; 'ok'"), "ok");
  napi_value cache = nullptr;
  EXPECT_EQ(unofficial_napi_module_wrap_create_cached_data(s.env, module, &cache), napi_pending_exception);
  EXPECT_EQ(TakePendingMessage(s.env), "buffer failure");
  ASSERT_EQ(RunText(s.env, "Object.defineProperty(globalThis, 'Buffer', {get() {globalThis.bufferGetterRan = true;}, configurable: true}); 'ok'"), "ok");
  ASSERT_EQ(unofficial_napi_terminate_execution(s.env), napi_ok);
  EXPECT_EQ(unofficial_napi_module_wrap_create_cached_data(s.env, module, &cache), napi_pending_exception);
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  EXPECT_EQ(RunText(s.env, "typeof bufferGetterRan"), "undefined");
}
#endif

TEST_F(Test77ModuleWrapState, FacadeRejectionIsHandled) {
  EnvScope s(runtime_.get());
  napi_value callback = nullptr;
  ASSERT_EQ(napi_run_script(s.env, Str(s.env,
      "globalThis.rejections = []; (type, promise, reason) => rejections.push(type)"), &callback), napi_ok);
  ASSERT_EQ(unofficial_napi_set_promise_reject_callback(s.env, callback), napi_ok);
  auto module = CreateSourceTextModule(s.env, "throw Error('expected'); export default 1;");
  napi_value ns = nullptr;
  EXPECT_EQ(unofficial_napi_module_wrap_create_required_module_facade(s.env, module, &ns), napi_pending_exception);
  EXPECT_EQ(TakePendingMessage(s.env), "expected");
  // Each notification that a rejection lacked a handler must be balanced.
  EXPECT_EQ(RunText(s.env, "rejections.reduce((balance, type) => balance + (type === 0 ? 1 : type === 1 ? -1 : 0), 0)"), "0");
}

#if defined(NAPI_TEST_ENGINE_QUICKJS)
TEST_F(Test77ModuleWrapState, ModuleCreationDuringTerminationRunsNoBindingOrSetters) {
  EnvScope s(runtime_.get());
  napi_value wrapper = nullptr;
  ASSERT_EQ(napi_run_script(s.env, Str(s.env,
      "globalThis.creationRan = false; globalThis.internalBinding = () => {creationRan = true; return {};};"
      "({set sourceURL(v) {creationRan = true;}, set sourceMapURL(v) {creationRan = true;}})"), &wrapper), napi_ok);
  auto source = unofficial_napi_js_source_from_text(Str(s.env, "export default 1;"));
  unofficial_napi_module_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  options.kind = unofficial_napi_module_source_text;
  options.wrapper = wrapper;
  options.url = Str(s.env, "terminated.mjs");
  options.payload.source_text.source = &source;
  unofficial_napi_module_create_result result{};
  ASSERT_EQ(unofficial_napi_terminate_execution(s.env), napi_ok);
  EXPECT_EQ(unofficial_napi_module_wrap_create(s.env, &options, &result), napi_pending_exception);
  EXPECT_EQ(result.module, nullptr);
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  EXPECT_EQ(RunText(s.env, "creationRan"), "false");
}
#endif

TEST_F(Test77ModuleWrapState, DestroyingImporterReleasesItsDependencyWrappers) {
  int finalized = 0;
  EnvScope s(runtime_.get());
  auto importer = CreateSourceTextModule(s.env, "import 'dep';");
  napi_handle_scope scope = nullptr;
  ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
  napi_value wrapper = nullptr;
  auto dependency = CreateSourceTextModule(s.env, "export default 1;", nullptr, &wrapper);
  ASSERT_EQ(napi_add_finalizer(s.env, wrapper, new FinalizedModule{dependency, &finalized}, DestroyFinalizedModule, nullptr, nullptr), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_link(s.env, importer, 1, &dependency), napi_ok);
  ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  ASSERT_EQ(unofficial_napi_module_wrap_destroy(s.env, importer), napi_ok);
  for (int attempt = 0; attempt < 4 && finalized == 0; ++attempt) {
    ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
    ASSERT_EQ(unofficial_napi_event_loop_checkpoint(s.env, unofficial_napi_event_loop_checkpoint_microtasks, true, nullptr), napi_ok);
  }
  EXPECT_EQ(finalized, 1);
}

TEST_F(Test77ModuleWrapState, RetainingDependenciesDoesNotCallArrayPrototypeSetters) {
  EnvScope s(runtime_.get());
  auto importer = CreateSourceTextModule(s.env, "import 'dep';");
  auto dependency = CreateSourceTextModule(s.env, "export default 1;");
  ASSERT_EQ(RunText(s.env, "globalThis.setterRan = false; Object.defineProperty(Array.prototype, '0', {set() {setterRan = true;}, configurable: true}); 'ok'"), "ok");
  EXPECT_EQ(unofficial_napi_module_wrap_link(s.env, importer, 1, &dependency), napi_ok);
  EXPECT_EQ(RunText(s.env, "delete Array.prototype[0]; String(setterRan)"), "false");
}
