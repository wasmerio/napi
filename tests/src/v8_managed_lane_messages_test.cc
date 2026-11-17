// Managed mode, natively, in a process of its own: V8 starts managed, with
// EdgeJS's default flags, and a WebAssembly.Module may only be transferred
// within one instance's lane.
#include <cstdio>
#include <cstring>
#include <string>

#include "js_native_api.h"
#include "unofficial_napi.h"

extern "C" bool snapi_v8_use_managed_runtime();
extern "C" bool snapi_v8_runtime_is_managed();
extern "C" void* snapi_v8_lane_new(size_t max_queued_tasks,
                                   void* scope_context,
                                   void* (*enter_scope)(void*),
                                   bool (*leave_scope)(void*, void*),
                                   void (*on_overload)(void*));
extern "C" void snapi_v8_lane_stop(void* handle);
extern "C" void snapi_v8_lane_delete(void* handle);
extern "C" void* snapi_v8_lane_swap_current(void* handle);

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

struct Env {
  napi_env env = nullptr;
  unofficial_napi_env_owner owner = nullptr;
};

// Creates an env with `lane` bound, as the bridge does for a managed store.
Env CreateEnv(void* lane) {
  void* previous = snapi_v8_lane_swap_current(lane);
  Env created;
  unofficial_napi_env_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_ENV_CREATE_OPTIONS_VERSION;
  if (unofficial_napi_create_env(8, &options, &created.env, &created.owner) != napi_ok) {
    created.env = nullptr;
  }
  snapi_v8_lane_swap_current(previous);
  return created;
}

void Release(const Env& env, void* lane) {
  void* previous = snapi_v8_lane_swap_current(lane);
  unofficial_napi_release_env(env.owner, nullptr);
  snapi_v8_lane_swap_current(previous);
}

std::string Run(napi_env env, const char* script, napi_value* value_out = nullptr) {
  napi_value source = nullptr;
  napi_value result = nullptr;
  if (napi_create_string_utf8(env, script, NAPI_AUTO_LENGTH, &source) != napi_ok ||
      napi_run_script(env, source, &result) != napi_ok) {
    napi_value exception = nullptr;
    napi_get_and_clear_last_exception(env, &exception);
    return "<threw>";
  }
  if (value_out != nullptr) *value_out = result;
  char text[64] = {0};
  size_t length = 0;
  if (napi_get_value_string_utf8(env, result, text, sizeof(text), &length) != napi_ok) {
    return "<not a string>";
  }
  return std::string(text, length);
}

unofficial_napi_message ModuleMessage(napi_env env) {
  napi_value value = nullptr;
  Run(env,
      "({ module: new WebAssembly.Module(new Uint8Array([0x00, 0x61, 0x73, 0x6d, "
      "0x01, 0x00, 0x00, 0x00])) })",
      &value);
  unofficial_napi_message message = nullptr;
  if (value == nullptr || unofficial_napi_message_create(env, value, &message) != napi_ok) {
    return nullptr;
  }
  return message;
}

// "refused" if taking the message throws, else the module's [[Class]].
std::string Take(napi_env env, unofficial_napi_message message) {
  napi_value value = nullptr;
  napi_status status = unofficial_napi_message_take(env, message, &value);
  if (status == napi_pending_exception) {
    napi_value exception = nullptr;
    napi_get_and_clear_last_exception(env, &exception);
    return "refused";
  }
  if (status != napi_ok) return "status " + std::to_string(status);
  napi_value global = nullptr;
  napi_get_global(env, &global);
  napi_set_named_property(env, global, "__received", value);
  return Run(env, "Object.prototype.toString.call(__received.module)");
}

}  // namespace

int main() {
  Check(snapi_v8_use_managed_runtime(), "the runtime could not turn managed");
  // A guest's request for EdgeJS's defaults through the public entry is the
  // embedder's here; managed startup applies the defaults anyway.
  if (unofficial_napi_configure_runtime(nullptr) != napi_ok) return 1;
  Check(snapi_v8_runtime_is_managed(), "V8 did not start managed");

  // Lanes that are never run: these envs post little, and nothing waits.
  void* lane_a = snapi_v8_lane_new(4096, nullptr, nullptr, nullptr, nullptr);
  void* lane_b = snapi_v8_lane_new(4096, nullptr, nullptr, nullptr, nullptr);
  if (lane_a == nullptr || lane_b == nullptr) return 1;

  Env sender = CreateEnv(lane_a);
  if (sender.env == nullptr) return 1;
  // EdgeJS's default --js-source-phase-imports is on in managed mode.
  Check(Run(sender.env,
            "(function () { try { new Function(\"return import.source('x')\"); "
            "return 'parsed'; } catch (e) { return 'rejected'; } })()") == "parsed",
        "source phase import syntax is rejected in managed mode");
  unofficial_napi_message to_other_lane = ModuleMessage(sender.env);
  unofficial_napi_message to_same_lane = ModuleMessage(sender.env);
  unofficial_napi_message to_standalone = ModuleMessage(sender.env);
  Check(to_other_lane != nullptr && to_same_lane != nullptr && to_standalone != nullptr,
        "could not make the module messages");
  Release(sender, lane_a);

  Env other = CreateEnv(lane_b);
  Check(Take(other.env, to_other_lane) == "refused",
        "a WebAssembly.Module crossed to another instance's lane");
  Release(other, lane_b);

  Env standalone = CreateEnv(nullptr);
  Check(Take(standalone.env, to_standalone) == "refused",
        "a WebAssembly.Module crossed from a lane to a standalone env");
  Release(standalone, nullptr);

  Env same = CreateEnv(lane_a);
  Check(Take(same.env, to_same_lane) == "[object WebAssembly.Module]",
        "a WebAssembly.Module did not cross within its instance's lane");
  Release(same, lane_a);

  snapi_v8_lane_stop(lane_a);
  snapi_v8_lane_stop(lane_b);
  snapi_v8_lane_delete(lane_a);
  snapi_v8_lane_delete(lane_b);
  if (failures == 0) std::puts("MANAGED_LANE_MESSAGES_OK");
  return failures == 0 ? 0 : 1;
}
