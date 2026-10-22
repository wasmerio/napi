#include "test_env.h"

#include "unofficial_napi.h"

#include <string>

#if defined(NAPI_TEST_ENGINE_V8)
#include <v8.h>
#endif

// unofficial_napi_create_trusted_script and unofficial_napi_is_executing_js.
// V8 implements both; QuickJS reports them as unsupported.
class Test66UnofficialTrustedScript : public FixtureTestBase {};

namespace {

napi_value Str(napi_env env, const char* value) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out) != napi_ok) return nullptr;
  return out;
}

#if defined(NAPI_TEST_ENGINE_V8)
struct MicrotaskRecord {
  unsigned calls = 0;
  unsigned active_calls = 0;
};

// makeTrusted(source) wraps a primitive string in a TrustedScript.
napi_value MakeTrusted(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value source = nullptr;
  napi_value result = nullptr;
  if (napi_get_cb_info(env, info, &argc, &source, nullptr, nullptr) != napi_ok || argc != 1 ||
      unofficial_napi_create_trusted_script(env, source, &result) != napi_ok) {
    napi_throw_type_error(env, nullptr, "Expected a primitive string");
    return nullptr;
  }
  return result;
}

// executing() reports unofficial_napi_is_executing_js.
napi_value Executing(napi_env env, napi_callback_info /*info*/) {
  bool active = false;
  napi_value result = nullptr;
  if (unofficial_napi_is_executing_js(env, &active) != napi_ok ||
      napi_get_boolean(env, active, &result) != napi_ok) {
    return nullptr;
  }
  return result;
}

// A native function queued as a microtask runs with no JavaScript frame on
// the stack, so only the microtask checkpoint makes the VM count as active.
napi_value RecordMicrotask(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  bool active = false;
  if (napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data) != napi_ok ||
      unofficial_napi_is_executing_js(env, &active) != napi_ok) {
    return nullptr;
  }
  auto* record = static_cast<MicrotaskRecord*>(data);
  ++record->calls;
  if (active) ++record->active_calls;
  return nullptr;
}

std::string TakeExceptionMessage(napi_env env) {
  bool pending = false;
  napi_value exception = nullptr;
  napi_value message = nullptr;
  if (napi_is_exception_pending(env, &pending) != napi_ok || !pending ||
      napi_get_and_clear_last_exception(env, &exception) != napi_ok) {
    return "";
  }
  char text[512] = {};
  size_t length = 0;
  if (napi_coerce_to_string(env, exception, &message) != napi_ok ||
      napi_get_value_string_utf8(env, message, text, sizeof(text), &length) != napi_ok) {
    return "<exception>";
  }
  return std::string(text, length);
}

// Evaluates a boolean expression in the root context.
::testing::AssertionResult IsTrue(napi_env env, const char* code) {
  napi_value result = nullptr;
  bool value = false;
  if (napi_run_script(env, Str(env, code), &result) != napi_ok) {
    return ::testing::AssertionFailure() << code << " threw " << TakeExceptionMessage(env);
  }
  if (napi_get_value_bool(env, result, &value) != napi_ok || !value) {
    return ::testing::AssertionFailure() << code << " is not true";
  }
  return ::testing::AssertionSuccess();
}

// Evaluates a boolean expression in a contextified sandbox.
::testing::AssertionResult IsTrueIn(napi_env env,
                                    napi_value sandbox,
                                    napi_value host_defined_option,
                                    const char* code) {
  const unofficial_napi_js_source source = unofficial_napi_js_source_from_text(Str(env, code));
  napi_value result = nullptr;
  bool value = false;
  if (unofficial_napi_contextify_run_script(env,
                                            sandbox,
                                            &source,
                                            Str(env, "trusted-script.js"),
                                            0,
                                            0,
                                            -1,
                                            true,
                                            false,
                                            false,
                                            host_defined_option,
                                            &result) != napi_ok) {
    return ::testing::AssertionFailure() << code << " threw " << TakeExceptionMessage(env);
  }
  if (napi_get_value_bool(env, result, &value) != napi_ok || !value) {
    return ::testing::AssertionFailure() << code << " is not true";
  }
  return ::testing::AssertionSuccess();
}

uint64_t UsedHeapAfterGc(napi_env env) {
  for (int i = 0; i < 3; ++i) {
    if (unofficial_napi_collect_garbage(env) != napi_ok) return 0;
  }
  unofficial_napi_heap_statistics stats;
  unofficial_napi_heap_statistics_init(&stats);
  if (unofficial_napi_get_heap_statistics(env, &stats) != napi_ok ||
      (stats.valid_fields & unofficial_napi_heap_stat_used_heap_size) == 0) {
    return 0;
  }
  return stats.used_heap_size;
}

unsigned g_embedder_hook_calls = 0;

struct TokenCallbackRecord {
  unsigned calls = 0;
  napi_status status = napi_generic_failure;
  bool active = true;
};

// Context token callbacks run while the provider holds its context-record
// lock; querying VM state from one must not wait on that lock.
void QueryFromTokenCallback(napi_env env, void* /*token*/, void* data) {
  auto* record = static_cast<TokenCallbackRecord*>(data);
  ++record->calls;
  record->status = unofficial_napi_is_executing_js(env, &record->active);
}

// An embedder hook that leaves every source unchanged.
v8::ModifyCodeGenerationFromStringsResult EmbedderHook(v8::Local<v8::Context>,
                                                      v8::Local<v8::Value>,
                                                      bool) {
  ++g_embedder_hook_calls;
  v8::ModifyCodeGenerationFromStringsResult result;
  result.codegen_allowed = true;
  return result;
}

void InstallGlobal(napi_env env, const char* name, napi_callback callback, void* data) {
  napi_value global = nullptr;
  napi_value function = nullptr;
  ASSERT_EQ(napi_get_global(env, &global), napi_ok);
  ASSERT_EQ(napi_create_function(env, name, NAPI_AUTO_LENGTH, callback, data, &function),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(env, global, name, function), napi_ok);
}
#endif

}  // namespace

#if defined(NAPI_TEST_ENGINE_V8)
TEST_F(Test66UnofficialTrustedScript, DirectEvalKeepsLexicalScope) {
  EnvScope s(runtime_.get());
  InstallGlobal(s.env, "makeTrusted", MakeTrusted, nullptr);

  EXPECT_TRUE(IsTrue(s.env, "(() => { let lexical = 40; "
                            "return eval(makeTrusted('lexical + 2')) === 42; })()"));
  EXPECT_TRUE(IsTrue(s.env, "(() => { let lexical = 0; eval(makeTrusted('lexical = 17')); "
                            "return lexical === 17; })()"));
  EXPECT_TRUE(IsTrue(s.env, "(function () { eval(makeTrusted('var trustedVar = 19')); "
                            "return trustedVar === 19; })()"));
  EXPECT_TRUE(IsTrue(s.env, "(() => { const sentinel = {}; "
                            "try { eval(makeTrusted('throw sentinel')); } "
                            "catch (error) { return error === sentinel; } return false; })()"));
  EXPECT_TRUE(IsTrue(s.env, "(() => { const original = eval; "
                            "return eval(makeTrusted('original === eval')) === true; })()"));
  EXPECT_TRUE(IsTrue(s.env, "(() => { 'use strict'; let lexical = 3; "
                            "eval(makeTrusted('var strictVar = lexical')); "
                            "return typeof strictVar === 'undefined'; })()"));
  // Indirect eval evaluates the source in global scope.
  EXPECT_TRUE(IsTrue(s.env, "(() => { let lexical = 1; "
                            "return (0, eval)(makeTrusted('typeof lexical')) === 'undefined'; })()"));
  EXPECT_TRUE(IsTrue(s.env, "eval(makeTrusted('')) === undefined"));
  EXPECT_TRUE(IsTrue(s.env, "eval(makeTrusted('\"\\u{1F49C}\"')) === '\\u{1F49C}'"));
  EXPECT_TRUE(IsTrue(s.env, "eval(Object.freeze(makeTrusted('31'))) === 31"));
}

TEST_F(Test66UnofficialTrustedScript, SourceIgnoresAuthorStringConversion) {
  EnvScope s(runtime_.get());
  InstallGlobal(s.env, "makeTrusted", MakeTrusted, nullptr);

  EXPECT_TRUE(IsTrue(s.env, "(() => { let lexical = 23; const source = makeTrusted('lexical'); "
                            "source.toString = () => { throw 1; }; "
                            "source[Symbol.toPrimitive] = () => { throw 2; }; "
                            "return eval(source) === 23; })()"));
  EXPECT_TRUE(IsTrue(s.env, "(() => { let lexical = 29; const source = makeTrusted('lexical'); "
                            "Object.setPrototypeOf(source, { toString() { throw 1; } }); "
                            "return eval(source) === 29; })()"));
  // The Function constructor stringifies its arguments. The default prototype
  // stringifies to the source, and only for the brand; an embedder can replace
  // the prototype with its own.
  EXPECT_TRUE(IsTrue(s.env, "Function(makeTrusted('return 37'))() === 37"));
  EXPECT_TRUE(IsTrue(s.env, "Function('input', makeTrusted('return input + 1'))(40) === 41"));
  EXPECT_TRUE(IsTrue(s.env, "(() => { const source = makeTrusted('return 43'); "
                            "Object.setPrototypeOf(source, { toString() { return 'return 43'; } }); "
                            "return Function(source)() === 43; })()"));
  EXPECT_TRUE(IsTrue(s.env, "String(makeTrusted('1 + 1')) === '1 + 1'"));
  EXPECT_TRUE(IsTrue(s.env, "(() => { const toString = makeTrusted('').toString; "
                            "try { toString.call({}); } catch (error) { "
                            "return error instanceof TypeError; } return false; })()"));
}

TEST_F(Test66UnofficialTrustedScript, BrandCannotBeCopiedOrInherited) {
  EnvScope s(runtime_.get());
  InstallGlobal(s.env, "makeTrusted", MakeTrusted, nullptr);

  EXPECT_TRUE(IsTrue(s.env, "(() => { const source = makeTrusted('throw 1'); "
                            "const child = Object.create(source); return eval(child) === child; })()"));
  EXPECT_TRUE(IsTrue(s.env, "(() => { const source = makeTrusted('throw 1'); "
                            "const proxy = new Proxy(source, { get() { throw 2; } }); "
                            "return eval(proxy) === proxy; })()"));
  EXPECT_TRUE(IsTrue(s.env, "(() => { const source = makeTrusted('throw 1'), copy = {}; "
                            "for (const key of Reflect.ownKeys(source)) "
                            "Object.defineProperty(copy, key, "
                            "Object.getOwnPropertyDescriptor(source, key)); "
                            "return Reflect.ownKeys(source).length === 0 && eval(copy) === copy; })()"));
  // eval of anything else is unchanged: objects are returned without being
  // stringified, and strings are evaluated.
  EXPECT_TRUE(IsTrue(s.env, "(() => { const value = { toString() { throw 1; }, "
                            "[Symbol.toPrimitive]() { throw 2; } }; return eval(value) === value; })()"));
  EXPECT_TRUE(IsTrue(s.env, "eval('6 * 7') === 42"));
}

// The brand cannot be forged through the private-symbol API: a private symbol
// with the brand's old registry name names a different property.
TEST_F(Test66UnofficialTrustedScript, BrandCannotBeForgedWithPrivateSymbol) {
  EnvScope s(runtime_.get());
  InstallGlobal(s.env, "makeTrusted", MakeTrusted, nullptr);
  napi_value real = nullptr;
  ASSERT_EQ(unofficial_napi_create_trusted_script(s.env, Str(s.env, "1"), &real), napi_ok);

  const char* names[] = {"wasmer.napi.intrinsic.TrustedScript.source.v1", "TrustedScript source"};
  for (const char* name : names) {
    napi_value symbol = nullptr;
    napi_value forged = nullptr;
    napi_value global = nullptr;
    ASSERT_EQ(unofficial_napi_create_private_symbol(s.env, name, NAPI_AUTO_LENGTH, &symbol),
              napi_ok);
    ASSERT_EQ(napi_create_object(s.env, &forged), napi_ok);
    ASSERT_EQ(napi_set_property(s.env, forged, symbol, Str(s.env, "6 * 7")), napi_ok);
    ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, global, "forged", forged), napi_ok);
    EXPECT_TRUE(IsTrue(s.env, "eval(forged) === forged")) << name;
    // A real trusted script given the forged property is still its own source.
    ASSERT_EQ(napi_set_property(s.env, real, symbol, Str(s.env, "6 * 7")), napi_ok);
    ASSERT_EQ(napi_set_named_property(s.env, global, "real", real), napi_ok);
    EXPECT_TRUE(IsTrue(s.env, "eval(real) === 1")) << name;
  }
}

// The isolate-wide hook is installed once. A hook the embedder installs
// afterwards is not overwritten by later trusted-script creation.
TEST_F(Test66UnofficialTrustedScript, KeepsEmbedderHookInstalledLater) {
  EnvScope s(runtime_.get());
  InstallGlobal(s.env, "makeTrusted", MakeTrusted, nullptr);
  EXPECT_TRUE(IsTrue(s.env, "eval(makeTrusted('2')) === 2"));

  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  ASSERT_NE(isolate, nullptr);
  g_embedder_hook_calls = 0;
  isolate->SetModifyCodeGenerationFromStringsCallback(EmbedderHook);
  // Created after the embedder's hook: eval now follows that hook, which
  // leaves the object unchanged.
  EXPECT_TRUE(IsTrue(s.env, "(() => { const source = makeTrusted('3'); "
                            "return eval(source) === source; })()"));
  EXPECT_GT(g_embedder_hook_calls, 0u);
}

TEST_F(Test66UnofficialTrustedScript, CreateValidatesArguments) {
  EnvScope s(runtime_.get());

  napi_value number = nullptr;
  napi_value object = nullptr;
  napi_value result = Str(s.env, "sentinel");
  ASSERT_EQ(napi_create_uint32(s.env, 1, &number), napi_ok);
  ASSERT_EQ(napi_run_script(s.env, Str(s.env, "new String('1')"), &object), napi_ok);
  EXPECT_EQ(unofficial_napi_create_trusted_script(s.env, number, &result), napi_string_expected);
  EXPECT_EQ(result, nullptr);
  // A String object is not a primitive string and is never coerced.
  EXPECT_EQ(unofficial_napi_create_trusted_script(s.env, object, &result), napi_string_expected);
  EXPECT_EQ(unofficial_napi_create_trusted_script(s.env, Str(s.env, "1"), nullptr),
            napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_create_trusted_script(s.env, nullptr, &result), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_create_trusted_script(nullptr, Str(s.env, "1"), &result),
            napi_invalid_arg);

  napi_value trusted = nullptr;
  napi_valuetype type = napi_undefined;
  ASSERT_EQ(unofficial_napi_create_trusted_script(s.env, Str(s.env, "1"), &trusted), napi_ok);
  ASSERT_EQ(napi_typeof(s.env, trusted, &type), napi_ok);
  EXPECT_EQ(type, napi_object);
}

// Creating trusted scripts retains nothing once they are unreachable. A new
// template per call left every instantiation in the context's template cache
// (about 750 KB after 20k calls).
TEST_F(Test66UnofficialTrustedScript, CreationDoesNotRetainMemory) {
  EnvScope s(runtime_.get());
  napi_value source = Str(s.env, "1");
  napi_value warm = nullptr;
  ASSERT_EQ(unofficial_napi_create_trusted_script(s.env, source, &warm), napi_ok);

  const uint64_t before = UsedHeapAfterGc(s.env);
  ASSERT_NE(before, 0u);
  for (int i = 0; i < 20000; ++i) {
    napi_handle_scope scope = nullptr;
    napi_value trusted = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    ASSERT_EQ(unofficial_napi_create_trusted_script(s.env, source, &trusted), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }
  const uint64_t after = UsedHeapAfterGc(s.env);
  ASSERT_NE(after, 0u);
  EXPECT_LT(after, before + 128 * 1024);
}

// A trusted script does not bypass a context that disallows code generation
// from strings, and eval of an unbranded object stays a no-op there.
TEST_F(Test66UnofficialTrustedScript, RespectsDisabledCodeGeneration) {
  EnvScope s(runtime_.get());

  napi_value sandbox = nullptr;
  napi_value host = nullptr;
  napi_value context = nullptr;
  napi_value trusted = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
  ASSERT_EQ(napi_create_symbol(s.env, Str(s.env, "trusted-script"), &host), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_make_context(s.env,
                                                    sandbox,
                                                    Str(s.env, "restricted"),
                                                    Str(s.env, "test://restricted"),
                                                    false,
                                                    true,
                                                    false,
                                                    host,
                                                    &context),
            napi_ok);
  ASSERT_EQ(unofficial_napi_create_trusted_script(s.env, Str(s.env, "throw 1"), &trusted),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, sandbox, "trusted", trusted), napi_ok);

  EXPECT_TRUE(IsTrueIn(s.env, sandbox, host,
                       "(() => { try { eval(trusted); } catch (error) { "
                       "return error instanceof EvalError; } return false; })()"));
  EXPECT_TRUE(IsTrueIn(s.env, sandbox, host,
                       "(() => { try { Function(trusted); } catch (error) { "
                       "return error instanceof EvalError; } return false; })()"));
  EXPECT_TRUE(IsTrueIn(s.env, sandbox, host,
                       "(() => { try { eval('1'); } catch (error) { "
                       "return error instanceof EvalError; } return false; })()"));
  EXPECT_TRUE(IsTrueIn(s.env, sandbox, host,
                       "(() => { const object = { toString() { throw 1; } }; "
                       "return eval(object) === object; })()"));

  // The root context still allows it.
  InstallGlobal(s.env, "makeTrusted", MakeTrusted, nullptr);
  EXPECT_TRUE(IsTrue(s.env, "eval(makeTrusted('5')) === 5"));
}

TEST_F(Test66UnofficialTrustedScript, IsExecutingJsReportsFramesAndMicrotasks) {
  EnvScope s(runtime_.get());
  MicrotaskRecord record;
  InstallGlobal(s.env, "executing", Executing, nullptr);

  bool active = true;
  ASSERT_EQ(unofficial_napi_is_executing_js(s.env, &active), napi_ok);
  EXPECT_FALSE(active);
  EXPECT_EQ(unofficial_napi_is_executing_js(s.env, nullptr), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_is_executing_js(nullptr, &active), napi_invalid_arg);

  EXPECT_TRUE(IsTrue(s.env, "executing() === true"));
  EXPECT_TRUE(IsTrue(s.env, "Object.defineProperty({}, 'x', { get: executing }).x === true"));

  // Only JavaScript frames count: a native function called from C, with no
  // JavaScript frame below it, reports false.
  napi_value global = nullptr;
  napi_value executing = nullptr;
  napi_value reported = nullptr;
  bool reported_active = true;
  ASSERT_EQ(napi_get_global(s.env, &global), napi_ok);
  ASSERT_EQ(napi_get_named_property(s.env, global, "executing", &executing), napi_ok);
  ASSERT_EQ(napi_call_function(s.env, global, executing, 0, nullptr, &reported), napi_ok);
  ASSERT_EQ(napi_get_value_bool(s.env, reported, &reported_active), napi_ok);
  EXPECT_FALSE(reported_active);

  napi_value microtask = nullptr;
  ASSERT_EQ(napi_create_function(
                s.env, "microtask", NAPI_AUTO_LENGTH, RecordMicrotask, &record, &microtask),
            napi_ok);
  ASSERT_EQ(unofficial_napi_enqueue_microtask(s.env, microtask), napi_ok);
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, false, nullptr),
            napi_ok);
  EXPECT_EQ(record.calls, 1u);
  EXPECT_EQ(record.active_calls, 1u);
  ASSERT_EQ(unofficial_napi_is_executing_js(s.env, &active), napi_ok);
  EXPECT_FALSE(active);

  // The query neither runs JavaScript nor consumes a pending exception.
  napi_value ignored = nullptr;
  napi_value exception = nullptr;
  bool pending = false;
  bool same = false;
  ASSERT_EQ(napi_run_script(s.env, Str(s.env, "throw globalThis"), &ignored),
            napi_pending_exception);
  active = true;
  ASSERT_EQ(unofficial_napi_is_executing_js(s.env, &active), napi_ok);
  EXPECT_FALSE(active);
  ASSERT_EQ(napi_is_exception_pending(s.env, &pending), napi_ok);
  EXPECT_TRUE(pending);
  ASSERT_EQ(napi_get_and_clear_last_exception(s.env, &exception), napi_ok);
  ASSERT_EQ(napi_strict_equals(s.env, exception, global, &same), napi_ok);
  EXPECT_TRUE(same);
}

TEST_F(Test66UnofficialTrustedScript, IsExecutingJsFromContextTokenCallback) {
  EnvScope s(runtime_.get());
  TokenCallbackRecord record;
  unofficial_napi_env_hooks hooks{};
  hooks.size = sizeof(hooks);
  hooks.version = UNOFFICIAL_NAPI_ENV_HOOKS_VERSION;
  hooks.data = &record;
  hooks.context_token_assign_callback = QueryFromTokenCallback;
  uint64_t accepted = 0;
  ASSERT_EQ(unofficial_napi_attach_env(s.env, &hooks, &accepted), napi_ok);

  napi_value sandbox = nullptr;
  napi_value host = nullptr;
  napi_value context = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
  ASSERT_EQ(napi_create_symbol(s.env, Str(s.env, "trusted-script"), &host), napi_ok);
  ASSERT_EQ(unofficial_napi_contextify_make_context(s.env,
                                                    sandbox,
                                                    Str(s.env, "token"),
                                                    Str(s.env, "test://token"),
                                                    true,
                                                    true,
                                                    true,
                                                    host,
                                                    &context),
            napi_ok);
  EXPECT_EQ(record.calls, 1u);
  EXPECT_EQ(record.status, napi_ok);
  EXPECT_FALSE(record.active);
}

// A native callback from the root context still sees JavaScript frames and
// microtask checkpoints that belong to another context.
TEST_F(Test66UnofficialTrustedScript, IsExecutingJsSeesOtherContexts) {
  EnvScope s(runtime_.get());
  MicrotaskRecord record;

  napi_value sandbox = nullptr;
  napi_value host = nullptr;
  napi_value context = nullptr;
  napi_value executing = nullptr;
  napi_value microtask = nullptr;
  ASSERT_EQ(napi_create_object(s.env, &sandbox), napi_ok);
  ASSERT_EQ(napi_create_symbol(s.env, Str(s.env, "trusted-script"), &host), napi_ok);
  // Own microtask queue: run_script drains it after evaluation.
  ASSERT_EQ(unofficial_napi_contextify_make_context(s.env,
                                                    sandbox,
                                                    Str(s.env, "other"),
                                                    Str(s.env, "test://other"),
                                                    true,
                                                    true,
                                                    true,
                                                    host,
                                                    &context),
            napi_ok);
  ASSERT_EQ(napi_create_function(
                s.env, "executing", NAPI_AUTO_LENGTH, Executing, nullptr, &executing),
            napi_ok);
  ASSERT_EQ(napi_create_function(
                s.env, "microtask", NAPI_AUTO_LENGTH, RecordMicrotask, &record, &microtask),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, sandbox, "executing", executing), napi_ok);
  ASSERT_EQ(napi_set_named_property(s.env, sandbox, "microtask", microtask), napi_ok);

  EXPECT_TRUE(IsTrueIn(s.env, sandbox, host, "executing() === true"));
  // A reaction job is queued in its handler's realm. Binding the context's
  // own Function.prototype.call queues the root callback in this context's
  // queue, and the bound builtin adds no JavaScript frame.
  EXPECT_TRUE(IsTrueIn(s.env, sandbox, host,
                       "Promise.resolve().then(Function.prototype.call.bind(microtask)), true"));
  EXPECT_EQ(record.calls, 1u);
  EXPECT_EQ(record.active_calls, 1u);

  bool active = true;
  ASSERT_EQ(unofficial_napi_is_executing_js(s.env, &active), napi_ok);
  EXPECT_FALSE(active);
}
#endif

#if defined(NAPI_TEST_ENGINE_QUICKJS)
TEST_F(Test66UnofficialTrustedScript, Unsupported) {
  EnvScope s(runtime_.get());

  napi_value result = Str(s.env, "sentinel");
  EXPECT_EQ(unofficial_napi_create_trusted_script(s.env, Str(s.env, "1"), &result),
            napi_generic_failure);
  EXPECT_EQ(result, nullptr);
  EXPECT_EQ(unofficial_napi_create_trusted_script(s.env, Str(s.env, "1"), nullptr),
            napi_invalid_arg);

  bool active = true;
  EXPECT_EQ(unofficial_napi_is_executing_js(s.env, &active), napi_generic_failure);
  EXPECT_FALSE(active);
  EXPECT_EQ(unofficial_napi_is_executing_js(s.env, nullptr), napi_invalid_arg);
}
#endif
