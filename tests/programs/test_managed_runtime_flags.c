#include <stdint.h>
#include <stdio.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

// Runs in a managed instance, whose guests may not set process-wide V8 flags:
// EdgeJS's fixed defaults are accepted as no-ops, anything else is refused,
// and the env is then created without the guest having started V8.

static napi_status Configure(const char* flags, uint32_t length) {
  unofficial_napi_runtime_options options = {
      .size = sizeof(options),
      .version = UNOFFICIAL_NAPI_RUNTIME_OPTIONS_VERSION,
      .engine_flags = flags,
      .engine_flags_length = length,
  };
  return unofficial_napi_configure_runtime(&options);
}

int main(void) {
  static const char defaults[] = "--js-source-phase-imports --harmony-import-attributes";
  CHECK_OR_FAIL(Configure(defaults, sizeof(defaults) - 1) == napi_ok,
                "EdgeJS's default flags were refused");
  CHECK_OR_FAIL(unofficial_napi_configure_runtime(NULL) == napi_ok,
                "an empty configuration was refused");

  static const char natives[] = "--allow-natives-syntax";
  CHECK_OR_FAIL(Configure(natives, sizeof(natives) - 1) == napi_invalid_arg,
                "a guest set a process-wide V8 flag");
  // A leading NUL must not make the flags look empty.
  static const char hidden[] = {'\0', '-', '-', 'n', 'o', '-', 'j', 'i', 't'};
  CHECK_OR_FAIL(Configure(hidden, sizeof(hidden)) == napi_invalid_arg,
                "flags behind a NUL were accepted");
  // Refused before the provider copies a guest-sized payload.
  CHECK_OR_FAIL(Configure((const char*)1, UINT32_MAX) == napi_invalid_arg,
                "an oversized flag string was accepted");

  napi_env env = NULL;
  unofficial_napi_env_owner owner = NULL;
  NAPI_CALL(env, unofficial_napi_create_env(8, NULL, &env, &owner));
  CHECK_OR_FAIL(env != NULL, "unofficial_napi_create_env returned no env");

  napi_value source = NULL;
  napi_value result = NULL;
  NAPI_CALL(env, napi_create_string_utf8(
                     env,
                     "(function () { try { eval('%IsSmi(1)'); return 'natives'; } "
                     "catch (e) { return 'plain'; } })()",
                     NAPI_AUTO_LENGTH, &source));
  NAPI_CALL(env, napi_run_script(env, source, &result));
  char text[16] = {0};
  size_t length = 0;
  NAPI_CALL(env, napi_get_value_string_utf8(env, result, text, sizeof(text), &length));
  CHECK_OR_FAIL(length == 5 && text[0] == 'p', "the refused flag took effect");

  puts("MANAGED_RUNTIME_FLAGS_OK");
  return 0;
}
