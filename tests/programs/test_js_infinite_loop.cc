// Runs JavaScript that never returns on its own, so a host can prove it is
// able to stop the isolate. Driven by tests/v8_host_control.rs, and
// deliberately absent from tests/programs/manifest.json: that harness also
// runs each program natively, where nothing would ever stop the loop.
//
// The marker is printed (and flushed) before entering JS so the host can tell
// "the loop was stopped" apart from "the guest never got that far".

#include <stdio.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != nullptr, "napi_wasm_init_env returned NULL");

  napi_value script;
  NAPI_CALL(env, napi_create_string_utf8(env, "while (true) {}",
                                         NAPI_AUTO_LENGTH, &script));

  printf("JS_LOOP_ENTERED\n");
  fflush(stdout);

  napi_value result;
  napi_status status = napi_run_script(env, script, &result);

  // Reaching this at all means the host stopped the loop. A terminated
  // isolate reports a pending exception rather than success.
  printf("JS_LOOP_LEFT status=%d\n", (int)status);
  fflush(stdout);

  // Now behave like a guest that doesn't want to stop: clear the termination
  // and try to run more JS. A host-requested stop is sticky, so both of these
  // have to keep failing.
  printf("CANCEL_STATUS=%d\n",
         (int)unofficial_napi_cancel_terminate_execution(env));

  napi_value resumed_script;
  napi_value resumed_result;
  napi_status resumed =
      napi_create_string_utf8(env, "1 + 1", NAPI_AUTO_LENGTH, &resumed_script);
  if (resumed == napi_ok) {
    resumed = napi_run_script(env, resumed_script, &resumed_result);
  }
  printf("RESUME_STATUS=%d\n", (int)resumed);
  fflush(stdout);

  return 0;
}
