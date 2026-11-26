// A guest function that JavaScript calls ends the process. The exit cannot
// unwind through the JavaScript frames between the guest and its caller: the
// host carries it out through them, and the process ends with the status the
// callback gave, having run nothing after it.

#include <stdio.h>
#include <stdlib.h>

#include "napi_test_helpers.h"

static napi_value exit_cb(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  // Reached through two levels of JavaScript.
  PrintSuccess("TEST_EXIT_IN_CALLBACK");
  fflush(stdout);
  exit(0);
}

static napi_value reenter_cb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value global;
  napi_value result;
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  napi_get_global(env, &global);
  napi_call_function(env, global, argv[0], 0, NULL, &result);
  // The exit unwinds this frame too.
  printf("FAIL: JavaScript returned to a guest function after the exit\n");
  fflush(stdout);
  napi_get_undefined(env, &result);
  return result;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  napi_value global, fn, script, result;
  NAPI_CALL(env, napi_get_global(env, &global));
  NAPI_CALL(env, napi_create_function(env, "exitInGuest", NAPI_AUTO_LENGTH,
                                      exit_cb, NULL, &fn));
  NAPI_CALL(env, napi_set_named_property(env, global, "exitInGuest", fn));
  NAPI_CALL(env, napi_create_function(env, "reenter", NAPI_AUTO_LENGTH,
                                      reenter_cb, NULL, &fn));
  NAPI_CALL(env, napi_set_named_property(env, global, "reenter", fn));

  NAPI_CALL(env, napi_create_string_utf8(
                     env, "try { reenter(() => exitInGuest()) } finally { 0 }",
                     NAPI_AUTO_LENGTH, &script));
  napi_run_script(env, script, &result);

  printf("FAIL: the guest went on after the exit\n");
  return 1;
}
