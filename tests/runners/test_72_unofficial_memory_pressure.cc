// unofficial_napi_memory_pressure: tell the engine that the embedder is short
// of memory.
//
// An embedder that trims memory while a page sits idle wants the garbage
// collected but V8's compilation cache kept, so that the next realm that runs
// the same scripts does not compile them again. unofficial_napi_collect_garbage
// (V8's LowMemoryNotification) empties that cache before it collects; before
// V8 13.7, a critical memory pressure notification collects without emptying
// it.
//
// Objects and scripts are made inside native callbacks: on V8 a value made at
// the top level of a test lives as long as the env, which would hide whether a
// collection freed it.

#include "test_env.h"

#include "unofficial_napi.h"

#include <cstdint>
#include <string>
#include <vector>

#if defined(NAPI_TEST_ENGINE_V8)
#include <v8-version.h>
#endif

class Test72UnofficialMemoryPressure : public FixtureTestBase {};

namespace {

constexpr int32_t kNone = unofficial_napi_memory_pressure_none;
constexpr int32_t kModerate = unofficial_napi_memory_pressure_moderate;
constexpr int32_t kCritical = unofficial_napi_memory_pressure_critical;

napi_status RunScript(napi_env env, const std::string& source, napi_value* result = nullptr) {
  napi_value script = nullptr;
  napi_status status = napi_create_string_utf8(env, source.data(), source.size(), &script);
  if (status != napi_ok) return status;
  napi_value ignored = nullptr;
  return napi_run_script(env, script, result != nullptr ? result : &ignored);
}

std::string RunScriptToString(napi_env env, const char* source) {
  napi_value result = nullptr;
  EXPECT_EQ(RunScript(env, source, &result), napi_ok);
  if (result == nullptr) return "<no result>";
  napi_value as_string = nullptr;
  EXPECT_EQ(napi_coerce_to_string(env, result, &as_string), napi_ok);
  char buffer[256] = {};
  size_t length = 0;
  EXPECT_EQ(napi_get_value_string_utf8(env, as_string, buffer, sizeof(buffer), &length), napi_ok);
  return std::string(buffer, length);
}

void DefineGlobalFunction(napi_env env, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn), napi_ok);
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(env, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(env, global, name, fn), napi_ok);
}

// makeGarbage(): an object that points to itself, held only by a weak
// reference. The cycle keeps QuickJS's reference counts from freeing it, so
// only a collection does.
napi_ref g_garbage = nullptr;
napi_value MakeGarbage(napi_env env, napi_callback_info /*info*/) {
  napi_value object = nullptr;
  EXPECT_EQ(napi_create_object(env, &object), napi_ok);
  EXPECT_EQ(napi_set_named_property(env, object, "self", object), napi_ok);
  EXPECT_EQ(napi_create_reference(env, object, 0, &g_garbage), napi_ok);
  return nullptr;
}

// garbageIsAlive(): whether the weak reference still finds the object.
napi_value GarbageIsAlive(napi_env env, napi_callback_info /*info*/) {
  napi_value object = nullptr;
  EXPECT_EQ(napi_get_reference_value(env, g_garbage, &object), napi_ok);
  napi_value alive = nullptr;
  EXPECT_EQ(napi_get_boolean(env, object != nullptr, &alive), napi_ok);
  return alive;
}

void InstallHelpers(napi_env env) {
  DefineGlobalFunction(env, "makeGarbage", MakeGarbage);
  DefineGlobalFunction(env, "garbageIsAlive", GarbageIsAlive);
}

void DeleteGarbageReference(napi_env env) {
  if (g_garbage == nullptr) return;
  EXPECT_EQ(napi_delete_reference(env, g_garbage), napi_ok);
  g_garbage = nullptr;
}

// makeFinalizedGarbage(): an object that points to itself, with a finalizer
// that counts its runs.
int g_finalized = 0;
void CountFinalize(node_api_basic_env /*env*/, void* /*data*/, void* /*hint*/) {
  ++g_finalized;
}
napi_value MakeFinalizedGarbage(napi_env env, napi_callback_info /*info*/) {
  napi_value object = nullptr;
  EXPECT_EQ(napi_create_object(env, &object), napi_ok);
  EXPECT_EQ(napi_set_named_property(env, object, "self", object), napi_ok);
  EXPECT_EQ(napi_add_finalizer(env, object, nullptr, CountFinalize, nullptr, nullptr), napi_ok);
  return nullptr;
}

// makeFinalizedGarbageCallingJs(): as makeFinalizedGarbage(), with a
// finalizer that calls setFlag(), JS that sets globalThis.finalizerRanJs.
int g_js_finalized = 0;
napi_status g_finalizer_get_status = napi_ok;
napi_status g_finalizer_call_status = napi_ok;
void CallJsFinalize(napi_env env, void* /*data*/, void* /*hint*/) {
  ++g_js_finalized;
  napi_value global = nullptr;
  napi_value set_flag = nullptr;
  napi_value ignored = nullptr;
  if (napi_get_global(env, &global) != napi_ok) return;
  g_finalizer_get_status = napi_get_named_property(env, global, "setFlag", &set_flag);
  if (g_finalizer_get_status != napi_ok) return;
  g_finalizer_call_status = napi_call_function(env, global, set_flag, 0, nullptr, &ignored);
}
napi_value MakeFinalizedGarbageCallingJs(napi_env env, napi_callback_info /*info*/) {
  napi_value object = nullptr;
  EXPECT_EQ(napi_create_object(env, &object), napi_ok);
  EXPECT_EQ(napi_set_named_property(env, object, "self", object), napi_ok);
  EXPECT_EQ(napi_add_finalizer(env, object, nullptr, CallJsFinalize, nullptr, nullptr), napi_ok);
  return nullptr;
}

void MakeGarbageCallingJs(napi_env env) {
  DefineGlobalFunction(env, "makeFinalizedGarbageCallingJs", MakeFinalizedGarbageCallingJs);
  g_js_finalized = 0;
  g_finalizer_get_status = napi_ok;
  g_finalizer_call_status = napi_ok;
  ASSERT_EQ(RunScript(env,
                      "globalThis.setFlag = () => { globalThis.finalizerRanJs = true; };"
                      "makeFinalizedGarbageCallingJs();"),
            napi_ok);
  ASSERT_EQ(g_js_finalized, 0);
}

#if defined(NAPI_TEST_ENGINE_V8)
// runBigScript(): compiles and runs g_big_script. Its top-level function is
// gone once the call returns, so only V8's compilation cache keeps the
// script's bytecode alive.
std::string g_big_script;
napi_value RunBigScript(napi_env env, napi_callback_info /*info*/) {
  EXPECT_EQ(RunScript(env, g_big_script), napi_ok);
  return nullptr;
}

// Top-level code of a few hundred KiB of bytecode; no function survives it.
std::string MakeBigScript() {
  std::string source = "{ let x = 0;\n";
  for (int i = 0; i < 20000; ++i) {
    source += "x = (x * 31 + " + std::to_string(i) + ") | 0;\n";
  }
  source += "globalThis.bigScriptResult = x; }\n";
  return source;
}

uint64_t BytecodeBytes(napi_env env) {
  unofficial_napi_heap_code_statistics stats{};
  EXPECT_EQ(unofficial_napi_get_heap_code_statistics(env, &stats), napi_ok);
  return stats.bytecode_and_metadata_size;
}
#endif

}  // namespace

TEST_F(Test72UnofficialMemoryPressure, RejectsNullEnvAndUnknownLevels) {
  EXPECT_EQ(unofficial_napi_memory_pressure(nullptr, kCritical), napi_invalid_arg);

  EnvScope s(runtime_.get());
  EXPECT_EQ(unofficial_napi_memory_pressure(s.env, -1), napi_invalid_arg);
  constexpr int32_t preserve = unofficial_napi_memory_pressure_preserve_code;
  const auto preserve_status = unofficial_napi_memory_pressure(s.env, preserve);
  EXPECT_TRUE(preserve_status == napi_ok || preserve_status == napi_queue_full ||
              preserve_status == napi_generic_failure);
  EXPECT_EQ(unofficial_napi_memory_pressure(s.env, preserve + 1), napi_invalid_arg);
  EXPECT_EQ(RunScriptToString(s.env, "1 + 1"), "2");
}

TEST_F(Test72UnofficialMemoryPressure, EveryLevelLeavesTheEnvRunningJs) {
  EnvScope s(runtime_.get());
  ASSERT_EQ(RunScript(s.env, "globalThis.kept = { value: 41 };"), napi_ok);

  for (int32_t level : {kNone, kModerate, kCritical, kModerate, kNone}) {
    SCOPED_TRACE(level);
    EXPECT_EQ(unofficial_napi_memory_pressure(s.env, level), napi_ok);
    EXPECT_EQ(RunScriptToString(s.env, "kept.value + 1"), "42");
    EXPECT_EQ(RunScriptToString(s.env, "[1, 2, 3].map((n) => n * 2).join()"), "2,4,6");
  }
}

// Critical pressure collects before it returns.
TEST_F(Test72UnofficialMemoryPressure, CriticalPressureCollectsGarbageNow) {
  EnvScope s(runtime_.get());
  InstallHelpers(s.env);

  ASSERT_EQ(RunScript(s.env, "makeGarbage();"), napi_ok);
  ASSERT_NE(g_garbage, nullptr);
  EXPECT_EQ(RunScriptToString(s.env, "garbageIsAlive()"), "true");

  EXPECT_EQ(unofficial_napi_memory_pressure(s.env, kNone), napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, "garbageIsAlive()"), "true");

  EXPECT_EQ(unofficial_napi_memory_pressure(s.env, kCritical), napi_ok);
  // Read the weak reference here, before any script runs. A collection that
  // V8 only requests, as it does for a call from another thread, runs at the
  // next script's interrupt check, so garbageIsAlive() would still see the
  // object gone. Reading at the top level is safe: when the object is gone,
  // no value comes back to keep it alive. When it is not, the value keeps it
  // alive for the rest of the test, so the check below fails as well.
  napi_value garbage = nullptr;
  EXPECT_EQ(napi_get_reference_value(s.env, g_garbage, &garbage), napi_ok);
  EXPECT_EQ(garbage, nullptr) << "critical pressure returned before it collected";
  EXPECT_EQ(RunScriptToString(s.env, "garbageIsAlive()"), "false");
  DeleteGarbageReference(s.env);
}

// Critical pressure finalizes what it frees as any collection does, which is
// engine-specific: V8 runs no finalizer in the call but queues it for the
// env's thread, where the next checkpoint runs it (the fixture attaches no
// enqueue hook to post it to); QuickJS runs it in the call.
TEST_F(Test72UnofficialMemoryPressure, CriticalPressureFinalizesAsAnyCollection) {
  EnvScope s(runtime_.get());
  DefineGlobalFunction(s.env, "makeFinalizedGarbage", MakeFinalizedGarbage);
  g_finalized = 0;
  ASSERT_EQ(RunScript(s.env, "makeFinalizedGarbage();"), napi_ok);
  ASSERT_EQ(g_finalized, 0);

  ASSERT_EQ(unofficial_napi_memory_pressure(s.env, kCritical), napi_ok);
#if defined(NAPI_TEST_ENGINE_V8)
  EXPECT_EQ(g_finalized, 0) << "V8 ran a finalizer inside the call";
  uint32_t state = 0;
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, false, &state),
            napi_ok);
#endif
  EXPECT_EQ(g_finalized, 1) << "the value critical pressure freed was not finalized";
}

// While the env terminates, what critical pressure frees is finalized where
// it otherwise is, and a call the finalizer makes into JS is refused. V8's
// checkpoint fails then and runs no finalizer, so with no enqueue hook (the
// fixture attaches none) V8 runs it at the first checkpoint after the cancel,
// where its call into JS succeeds. QuickJS runs it in the call, terminating.
TEST_F(Test72UnofficialMemoryPressure, PressureWhileTerminatingFinalizesAsTheEngineDoes) {
  EnvScope s(runtime_.get());
  MakeGarbageCallingJs(s.env);

  ASSERT_EQ(unofficial_napi_terminate_execution(s.env), napi_ok);
  ASSERT_EQ(unofficial_napi_memory_pressure(s.env, kCritical), napi_ok);
#if defined(NAPI_TEST_ENGINE_V8)
  EXPECT_EQ(g_js_finalized, 0) << "V8 ran a finalizer inside the call";
  uint32_t state = 0;
  EXPECT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, false, &state),
            napi_pending_exception);
  EXPECT_EQ(g_js_finalized, 0) << "the checkpoint of a terminating env ran a finalizer";

  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, false, &state),
            napi_ok);
  EXPECT_EQ(g_js_finalized, 1) << "the first checkpoint after the cancel did not finalize";
  EXPECT_EQ(g_finalizer_call_status, napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, "String(globalThis.finalizerRanJs)"), "true");
#else
  EXPECT_EQ(g_js_finalized, 1) << "QuickJS did not finalize in the call";
  EXPECT_EQ(g_finalizer_get_status, napi_pending_exception)
      << "the finalizer's call into JS was not refused";
  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  EXPECT_EQ(RunScriptToString(s.env, "String(globalThis.finalizerRanJs)"), "undefined");
#endif
}

#if defined(NAPI_TEST_ENGINE_V8)
namespace {

struct PostedTask {
  unofficial_napi_foreground_task_callback callback;
  void* data;
  unofficial_napi_foreground_task_cleanup cleanup;
};
std::vector<PostedTask>* g_posted = nullptr;
napi_env g_posting_env = nullptr;
bool g_queue_closed = false;

// Keeps every task for the test to run on the env's thread, until the queue
// closes; then releases each task it is given.
napi_status QueueTask(void* /*target*/,
                      unofficial_napi_foreground_task_callback callback,
                      void* data,
                      unofficial_napi_foreground_task_cleanup cleanup,
                      uint64_t /*delay_millis*/) {
  if (g_queue_closed) {
    if (cleanup != nullptr) cleanup(g_posting_env, data);
    return napi_generic_failure;
  }
  g_posted->push_back({callback, data, cleanup});
  return napi_ok;
}

// Releases the queued tasks without running them, and every later one, as an
// embedder does when the env shuts down.
void CloseQueue() {
  g_queue_closed = true;
  std::vector<PostedTask> tasks;
  tasks.swap(*g_posted);
  for (const PostedTask& task : tasks) {
    if (task.cleanup != nullptr) task.cleanup(g_posting_env, task.data);
  }
}

// Closes the queue when it goes out of scope, a failed ASSERT included.
// Declared after the env, it closes the queue before the env's release, so
// that every task the hook was given is released.
struct QueueCloser {
  ~QueueCloser() { CloseQueue(); }
};

// Runs the queued tasks, and those they queue, as an event loop turn would.
size_t RunPostedTasks(napi_env env) {
  size_t ran = 0;
  for (int round = 0; round < 16 && !g_posted->empty(); ++round) {
    std::vector<PostedTask> tasks;
    tasks.swap(*g_posted);
    for (const PostedTask& task : tasks) {
      task.callback(env, task.data);
      if (task.cleanup != nullptr) task.cleanup(env, task.data);
      ++ran;
    }
  }
  return ran;
}

}  // namespace

// With an enqueue hook, V8 posts the drain of the finalizers as a task, and
// the task runs them also while the env terminates: the finalizer's call into
// JS is refused then, and it does not run again after the cancel.
TEST_F(Test72UnofficialMemoryPressure, V8PressureWhileTerminatingPostsTheFinalizers) {
  std::vector<PostedTask> posted;
  g_posted = &posted;
  g_queue_closed = false;
  EnvScope s(runtime_.get());
  g_posting_env = s.env;
  QueueCloser closer;
  unofficial_napi_env_hooks hooks{};
  hooks.size = sizeof(hooks);
  hooks.version = UNOFFICIAL_NAPI_ENV_HOOKS_VERSION;
  hooks.enqueue_foreground_task_callback = QueueTask;
  uint64_t accepted = 0;
  ASSERT_EQ(unofficial_napi_attach_env(s.env, &hooks, &accepted), napi_ok);
  ASSERT_NE(accepted & unofficial_napi_env_hook_enqueue_foreground_task, 0u);
  MakeGarbageCallingJs(s.env);
  RunPostedTasks(s.env);

  ASSERT_EQ(unofficial_napi_terminate_execution(s.env), napi_ok);
  ASSERT_EQ(unofficial_napi_memory_pressure(s.env, kCritical), napi_ok);
  EXPECT_EQ(g_js_finalized, 0) << "V8 ran a finalizer inside the call";
  EXPECT_GT(RunPostedTasks(s.env), 0u) << "the drain was not posted";
  EXPECT_EQ(g_js_finalized, 1) << "the posted task did not finalize while terminating";
  EXPECT_EQ(g_finalizer_get_status, napi_pending_exception)
      << "the finalizer's call into JS was not refused";

  ASSERT_EQ(unofficial_napi_cancel_terminate_execution(s.env), napi_ok);
  uint32_t state = 0;
  ASSERT_EQ(unofficial_napi_event_loop_checkpoint(
                s.env, unofficial_napi_event_loop_checkpoint_microtasks, false, &state),
            napi_ok);
  RunPostedTasks(s.env);
  EXPECT_EQ(g_js_finalized, 1);
  EXPECT_EQ(RunScriptToString(s.env, "String(globalThis.finalizerRanJs)"), "undefined");
}
#endif

#if defined(NAPI_TEST_ENGINE_V8)
// The point of the call on V8: critical pressure keeps a script that ran
// earlier compiled, and running it again uses that bytecode, where
// collect_garbage drops it. V8 13.7 and later empty the compilation cache on
// memory pressure as well.
TEST_F(Test72UnofficialMemoryPressure, V8CriticalPressureKeepsCompiledScripts) {
#if V8_MAJOR_VERSION > 13 || (V8_MAJOR_VERSION == 13 && V8_MINOR_VERSION >= 7)
  GTEST_SKIP() << "V8 13.7 and later empty the compilation cache on memory pressure";
#endif
  EnvScope s(runtime_.get());
  DefineGlobalFunction(s.env, "runBigScript", RunBigScript);
  g_big_script = MakeBigScript();

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  const uint64_t before = BytecodeBytes(s.env);
  ASSERT_EQ(RunScript(s.env, "runBigScript();"), napi_ok);
  const uint64_t compiled = BytecodeBytes(s.env);
  ASSERT_GT(compiled, before + 100 * 1024)
      << "the script did not compile to the expected amount of bytecode";
  const uint64_t script_bytes = compiled - before;

  ASSERT_EQ(unofficial_napi_memory_pressure(s.env, kCritical), napi_ok);
  const uint64_t after_pressure = BytecodeBytes(s.env);
  EXPECT_GT(after_pressure, compiled - script_bytes / 10)
      << "critical pressure dropped the compiled script: before=" << before
      << " compiled=" << compiled << " after=" << after_pressure;

  // Running the same source again finds it in the cache: no second copy of
  // its bytecode.
  ASSERT_EQ(RunScript(s.env, "runBigScript();"), napi_ok);
  const uint64_t after_rerun = BytecodeBytes(s.env);
  EXPECT_LT(after_rerun, after_pressure + script_bytes / 10)
      << "the script compiled again after critical pressure: compiled=" << compiled
      << " after=" << after_pressure << " rerun=" << after_rerun;

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  const uint64_t after_collect = BytecodeBytes(s.env);
  EXPECT_LT(after_collect, before + script_bytes / 10)
      << "collect_garbage kept the compiled script: before=" << before
      << " compiled=" << compiled << " after=" << after_collect;

  EXPECT_EQ(RunScriptToString(s.env, "typeof bigScriptResult"), "number");
  g_big_script.clear();
}
#endif

#if defined(NAPI_TEST_ENGINE_QUICKJS)
namespace {

// Counts the runs of the finalizer of a realm's host-defined option id, which
// the realm's record holds until it is freed.
int g_host_id_finalized = 0;
void CountHostIdFinalize(napi_env /*env*/, void* /*data*/, void* /*hint*/) {
  ++g_host_id_finalized;
}

// Makes a realm that nothing keeps, whose host-defined option id has a
// finalizer, and returns a weak reference to its global.
napi_ref MakeUnreachableRealm(napi_env env) {
  napi_handle_scope scope = nullptr;
  EXPECT_EQ(napi_open_handle_scope(env, &scope), napi_ok);
  napi_value host_id = nullptr;
  EXPECT_EQ(napi_create_object(env, &host_id), napi_ok);
  EXPECT_EQ(napi_add_finalizer(env, host_id, nullptr, CountHostIdFinalize, nullptr, nullptr),
            napi_ok);
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.host_defined_option_id = host_id;
  unofficial_napi_realm realm = nullptr;
  napi_value global = nullptr;
  EXPECT_EQ(unofficial_napi_create_realm(env, &options, UNOFFICIAL_NAPI_REALM_MODE_FRESH, 0, nullptr, &realm, &global, nullptr), napi_ok);
  napi_ref weak = nullptr;
  EXPECT_EQ(napi_create_reference(env, global, 0, &weak), napi_ok);
  EXPECT_EQ(napi_close_handle_scope(env, scope), napi_ok);
  return weak;
}

bool Collected(napi_env env, napi_ref weak) {
  napi_handle_scope scope = nullptr;
  EXPECT_EQ(napi_open_handle_scope(env, &scope), napi_ok);
  napi_value value = nullptr;
  EXPECT_EQ(napi_get_reference_value(env, weak, &value), napi_ok);
  EXPECT_EQ(napi_close_handle_scope(env, scope), napi_ok);
  return value == nullptr;
}

}  // namespace

// QuickJS frees the record of a collected realm, and with it the realm's
// host-defined option id, only when it next makes a realm: neither critical
// pressure nor unofficial_napi_collect_garbage frees it.
TEST_F(Test72UnofficialMemoryPressure, QuickJSFreesACollectedRealmsRecordAtTheNextRealm) {
  EnvScope s(runtime_.get());
  g_host_id_finalized = 0;
  napi_ref weak = MakeUnreachableRealm(s.env);
  ASSERT_NE(weak, nullptr);
  EXPECT_FALSE(Collected(s.env, weak));

  ASSERT_EQ(unofficial_napi_memory_pressure(s.env, kCritical), napi_ok);
  EXPECT_TRUE(Collected(s.env, weak)) << "critical pressure did not collect the realm";
  EXPECT_EQ(g_host_id_finalized, 0) << "memory pressure freed the collected realm's record";

  ASSERT_EQ(unofficial_napi_collect_garbage(s.env), napi_ok);
  EXPECT_EQ(g_host_id_finalized, 0) << "collect_garbage freed the collected realm's record";

  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    unofficial_napi_realm realm = nullptr;
    napi_value global = nullptr;
    ASSERT_EQ(unofficial_napi_create_realm(s.env, nullptr, UNOFFICIAL_NAPI_REALM_MODE_FRESH, 0, nullptr, &realm, &global, nullptr), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }
  EXPECT_EQ(g_host_id_finalized, 1) << "the next realm did not free the collected realm's record";
  ASSERT_EQ(napi_delete_reference(s.env, weak), napi_ok);
}
#endif
