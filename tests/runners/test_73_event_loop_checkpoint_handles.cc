// Regression test: unofficial_napi_event_loop_checkpoint frees the handles it
// makes.
//
// An embedder may run checkpoints from its event loop with no handle scope of
// its own open. The innermost scope is then the env's root scope, which stays
// open for the env's whole life. The V8 checkpoint opened no scope, so every
// call left handles there: the env's context, taken for its microtask queue,
// and the promise of each pending dynamic import. A long-running embedder grew
// the root scope by a few handles per loop turn, without bound.
//
// V8 provider only: the test counts handles with
// v8::HandleScope::NumberOfHandles.

#include <v8.h>

#include "test_env.h"

class Test73EventLoopCheckpointHandles : public FixtureTestBase {};

namespace {

// Runs a script in a handle scope of its own. It leaves nothing in the
// caller's scope apart from the result, when one is asked for. Only then is
// the scope escapable: an escapable scope takes its escape slot in the
// caller's scope even when nothing is escaped.
testing::AssertionResult RunJs(napi_env env,
                               const char* source_text,
                               napi_value* result_out = nullptr) {
  napi_handle_scope scope = nullptr;
  napi_escapable_handle_scope escapable_scope = nullptr;
  const napi_status opened = result_out != nullptr
                                 ? napi_open_escapable_handle_scope(env, &escapable_scope)
                                 : napi_open_handle_scope(env, &scope);
  if (opened != napi_ok) {
    return testing::AssertionFailure() << "could not open a handle scope";
  }
  napi_value source = nullptr;
  napi_value result = nullptr;
  const bool ran =
      napi_create_string_utf8(env, source_text, NAPI_AUTO_LENGTH, &source) == napi_ok &&
      napi_run_script(env, source, &result) == napi_ok;
  if (ran && result_out != nullptr) {
    napi_escape_handle(env, escapable_scope, result, result_out);
  }
  if (!ran) {
    // Cleared before the scope closes, so its handle is freed with the scope.
    bool pending = false;
    napi_value ignored = nullptr;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_get_and_clear_last_exception(env, &ignored);
    }
  }
  if (escapable_scope != nullptr) {
    napi_close_escapable_handle_scope(env, escapable_scope);
  } else {
    napi_close_handle_scope(env, scope);
  }
  if (ran) return testing::AssertionSuccess();
  return testing::AssertionFailure() << "script failed: " << source_text;
}

// Reads an int global in a handle scope of its own, so that nothing stays in
// the env's root scope.
int32_t GlobalInt(napi_env env, const char* name) {
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) return -1;
  napi_value result = nullptr;
  int32_t value = -1;
  if (!RunJs(env, name, &result) || napi_get_value_int32(env, result, &value) != napi_ok) {
    value = -1;
  }
  napi_close_handle_scope(env, scope);
  return value;
}

}  // namespace

TEST_F(Test73EventLoopCheckpointHandles, CheckpointsOutsideAnyScopeLeaveNoHandles) {
  EnvScope s(runtime_.get());
  ASSERT_NE(s.env, nullptr);

  // The provider enters the env's isolate on the thread that creates the env
  // and keeps it entered until the env is released.
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  ASSERT_NE(isolate, nullptr);

  // A value made with no scope open lands in the env's root scope, and the
  // count sees it.
  {
    const int before = v8::HandleScope::NumberOfHandles(isolate);
    napi_value probe = nullptr;
    ASSERT_EQ(napi_create_object(s.env, &probe), napi_ok);
    ASSERT_GT(v8::HandleScope::NumberOfHandles(isolate), before);
  }

  // A dynamic import that never settles keeps provider work pending, so every
  // checkpoint below looks at its promise. The provider keeps its own
  // reference to the hook.
  {
    napi_handle_scope scope = nullptr;
    ASSERT_EQ(napi_open_handle_scope(s.env, &scope), napi_ok);
    napi_value import_hook = nullptr;
    ASSERT_TRUE(RunJs(s.env,
                      "(function importModuleDynamically() {"
                      "  return new Promise(() => {});"
                      "})",
                      &import_hook));
    const unofficial_napi_module_hooks hooks = {
        sizeof(unofficial_napi_module_hooks),
        UNOFFICIAL_NAPI_MODULE_HOOKS_VERSION,
        import_hook,
        nullptr,
    };
    ASSERT_EQ(unofficial_napi_module_wrap_set_hooks(s.env, &hooks), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(s.env, scope), napi_ok);
  }
  // From here on the test's own calls leave nothing in the root scope, so it
  // grows only by what the checkpoints leave there.
  const int start = v8::HandleScope::NumberOfHandles(isolate);
  ASSERT_TRUE(RunJs(s.env,
                    "globalThis.ticks = 0;"
                    "globalThis.pendingImport = import('never-settles');"));

  constexpr int kCheckpoints = 64;
  int leaked = 0;
  int turns_without_task = 0;
  for (int turn = 0; turn < kCheckpoints; ++turn) {
    ASSERT_TRUE(RunJs(s.env, "Promise.resolve().then(() => { globalThis.ticks++; });"));
    // The microtask waits for the checkpoint.
    ASSERT_EQ(GlobalInt(s.env, "globalThis.ticks"), turn);
    const auto mode = (turn % 2 == 0) ? unofficial_napi_event_loop_checkpoint_microtasks
                                      : unofficial_napi_event_loop_checkpoint_host_tasks;
    const int before = v8::HandleScope::NumberOfHandles(isolate);
    uint32_t state = 0;
    ASSERT_EQ(unofficial_napi_event_loop_checkpoint(s.env, mode, false, &state), napi_ok);
    leaked += v8::HandleScope::NumberOfHandles(isolate) - before;
    // A checkpoint that ran a platform task reports pending provider work
    // whatever else is pending, so only the others show the import.
    if ((state & unofficial_napi_event_loop_checkpoint_state_host_tasks_admitted) == 0) {
      ++turns_without_task;
      EXPECT_EQ(state, unofficial_napi_event_loop_checkpoint_state_pending_provider_work)
          << "the pending dynamic import is not tracked at turn " << turn;
    }
  }
  ASSERT_GT(turns_without_task, 0) << "every checkpoint ran a platform task";

  // Every checkpoint ran its microtask.
  EXPECT_EQ(GlobalInt(s.env, "globalThis.ticks"), kCheckpoints);
  EXPECT_EQ(leaked, 0) << kCheckpoints << " checkpoints left " << leaked
                       << " handles in the env's root scope";
  const int grown = v8::HandleScope::NumberOfHandles(isolate) - start;
  EXPECT_EQ(grown, 0) << "the env's root scope grew by " << grown << " handles over the loop, "
                      << leaked << " of them left by the checkpoints";
}
