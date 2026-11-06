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
  EXPECT_EQ(unofficial_napi_memory_pressure(s.env, kCritical + 1), napi_invalid_arg);
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
