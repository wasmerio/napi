#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "node_api.h"
#include "unofficial_napi.h"

typedef struct {
  napi_value expected_global;
  napi_env env;
  bool saw_expected_global;
  int calls;
} callback_record;

// Values made inside a callback die when it returns, so the global is
// compared here.
static napi_value NAPI_CDECL RecordCallback(napi_env env, napi_callback_info info) {
  void* data = NULL;
  if (napi_get_cb_info(env, info, NULL, NULL, NULL, &data) != napi_ok || data == NULL) {
    return NULL;
  }
  callback_record* record = (callback_record*)data;
  record->env = env;
  record->calls++;
  napi_value global = NULL;
  if (napi_get_global(env, &global) != napi_ok ||
      napi_strict_equals(env, global, record->expected_global,
                         &record->saw_expected_global) != napi_ok) {
    return NULL;
  }
  napi_value object = NULL;
  if (napi_create_object(env, &object) != napi_ok) return NULL;
  return object;
}

static napi_status RunScript(napi_env env, const char* source, napi_value* result) {
  napi_value script = NULL;
  napi_status status = napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &script);
  if (status != napi_ok) return status;
  return napi_run_script(env, script, result);
}

static napi_status RunInt32(napi_env env, const char* source, int32_t* out) {
  napi_value result = NULL;
  napi_status status = RunScript(env, source, &result);
  if (status != napi_ok) return status;
  return napi_get_value_int32(env, result, out);
}

static bool StrictEquals(napi_env env, napi_value a, napi_value b) {
  bool same = false;
  return napi_strict_equals(env, a, b, &same) == napi_ok && same;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  // A realm has its own global.
  napi_env realm = NULL;
  napi_value realm_global = NULL;
  NAPI_CALL(env, unofficial_napi_create_realm(env, NULL, &realm, &realm_global));
  CHECK_OR_FAIL(realm != NULL && realm != env, "create_realm did not return a new env");
  napi_value env_global = NULL;
  napi_value realm_own_global = NULL;
  NAPI_CALL(env, napi_get_global(env, &env_global));
  NAPI_CALL(realm, napi_get_global(realm, &realm_own_global));
  CHECK_OR_FAIL(StrictEquals(env, realm_own_global, realm_global),
                "the realm env's global is not the one create_realm returned");
  CHECK_OR_FAIL(!StrictEquals(env, env_global, realm_global),
                "the realm shares its parent's global");
  printf("realm has its own global\n");

  // Values made through one env are usable through the other.
  napi_value shared = NULL;
  NAPI_CALL(env, napi_create_object(env, &shared));
  NAPI_CALL(realm, napi_set_named_property(realm, realm_global, "shared", shared));
  int32_t x = 0;
  NAPI_CALL(realm, RunInt32(realm, "shared.x = 7", &x));
  napi_value x_value = NULL;
  NAPI_CALL(env, napi_get_named_property(env, shared, "x", &x_value));
  NAPI_CALL(env, napi_get_value_int32(env, x_value, &x));
  CHECK_OR_FAIL(x == 7, "a value did not cross envs");
  printf("values cross envs: x=%d\n", x);

  // A native function made through the realm env runs in that realm, also
  // when the parent's script calls it.
  callback_record record = {realm_global, NULL, false, 0};
  napi_value fn = NULL;
  NAPI_CALL(realm, napi_create_function(
                       realm, "probe", NAPI_AUTO_LENGTH, RecordCallback, &record, &fn));
  NAPI_CALL(env, napi_set_named_property(env, env_global, "probe", fn));
  napi_value probe_result = NULL;
  NAPI_CALL(env, RunScript(env, "probe()", &probe_result));
  CHECK_OR_FAIL(record.calls == 1, "the callback did not run");
  CHECK_OR_FAIL(record.env == realm, "the callback did not get the realm env");
  CHECK_OR_FAIL(record.saw_expected_global, "the callback did not see the realm's global");
  printf("callback saw the realm env and the realm global\n");

  // get_realm maps globals back to their envs.
  napi_env found = NULL;
  NAPI_CALL(env, unofficial_napi_get_realm(env, realm_global, &found));
  CHECK_OR_FAIL(found == realm, "get_realm(realm global) did not return the realm env");
  found = NULL;
  NAPI_CALL(realm, unofficial_napi_get_realm(realm, env_global, &found));
  CHECK_OR_FAIL(found == env, "get_realm(parent global) did not return the parent env");
  printf("get_realm finds both envs\n");

  // A sandbox realm reads and writes the sandbox.
  napi_value sandbox = NULL;
  napi_value five = NULL;
  NAPI_CALL(env, napi_create_object(env, &sandbox));
  NAPI_CALL(env, napi_create_int32(env, 5, &five));
  NAPI_CALL(env, napi_set_named_property(env, sandbox, "input", five));
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = sandbox;
  napi_env sandbox_realm = NULL;
  napi_value sandbox_global = NULL;
  NAPI_CALL(env, unofficial_napi_create_realm(env, &options, &sandbox_realm, &sandbox_global));
  CHECK_OR_FAIL(StrictEquals(env, sandbox_global, sandbox),
                "create_realm did not return the sandbox as the global");
  napi_value source_text = NULL;
  NAPI_CALL(env, napi_create_string_utf8(
                     env, "output = input * 2; input", NAPI_AUTO_LENGTH, &source_text));
  napi_value filename = NULL;
  NAPI_CALL(env, napi_create_string_utf8(env, "sandbox.js", NAPI_AUTO_LENGTH, &filename));
  const unofficial_napi_js_source source = unofficial_napi_js_source_from_text(source_text);
  napi_value run_result = NULL;
  NAPI_CALL(sandbox_realm, unofficial_napi_contextify_run_script(sandbox_realm,
                                                                 NULL,
                                                                 &source,
                                                                 filename,
                                                                 0,
                                                                 0,
                                                                 -1,
                                                                 true,
                                                                 false,
                                                                 false,
                                                                 NULL,
                                                                 &run_result));
  int32_t input = 0;
  NAPI_CALL(env, napi_get_value_int32(env, run_result, &input));
  napi_value output_value = NULL;
  int32_t output = 0;
  NAPI_CALL(env, napi_get_named_property(env, sandbox, "output", &output_value));
  NAPI_CALL(env, napi_get_value_int32(env, output_value, &output));
  CHECK_OR_FAIL(input == 5 && output == 10, "the sandbox realm did not use the sandbox");
  printf("sandbox realm: input=%d output=%d\n", input, output);

  // make_context makes a realm env that get_realm finds.
  napi_value context_sandbox = NULL;
  napi_value three = NULL;
  NAPI_CALL(env, napi_create_object(env, &context_sandbox));
  NAPI_CALL(env, napi_create_int32(env, 3, &three));
  NAPI_CALL(env, napi_set_named_property(env, context_sandbox, "x", three));
  napi_value context_name = NULL;
  napi_value contextified = NULL;
  NAPI_CALL(env, napi_create_string_utf8(env, "ctx", NAPI_AUTO_LENGTH, &context_name));
  NAPI_CALL(env, unofficial_napi_contextify_make_context(env,
                                                         context_sandbox,
                                                         context_name,
                                                         NULL,
                                                         true,
                                                         true,
                                                         false,
                                                         NULL,
                                                         &contextified));
  napi_env context_realm = NULL;
  NAPI_CALL(env, unofficial_napi_get_realm(env, context_sandbox, &context_realm));
  CHECK_OR_FAIL(context_realm != NULL && context_realm != env,
                "get_realm did not find make_context's realm env");
  napi_env context_realm_again = NULL;
  NAPI_CALL(env, unofficial_napi_get_realm(env, context_sandbox, &context_realm_again));
  CHECK_OR_FAIL(context_realm_again == context_realm,
                "get_realm returned a different env for the same realm");
  int32_t x_plus_one = 0;
  NAPI_CALL(context_realm, RunInt32(context_realm, "x + 1", &x_plus_one));
  CHECK_OR_FAIL(x_plus_one == 4, "make_context's realm env did not run in its realm");
  printf("make_context realm env: x+1=%d\n", x_plus_one);

  // Releasing an env with live realm envs, nested ones included, releases
  // them with it.
  napi_env env2 = NULL;
  unofficial_napi_env_owner env2_scope = NULL;
  NAPI_CALL(env, unofficial_napi_create_env(8, NULL, &env2, &env2_scope));
  napi_env env2_realm = NULL;
  napi_env env2_nested = NULL;
  NAPI_CALL(env2, unofficial_napi_create_realm(env2, NULL, &env2_realm, NULL));
  NAPI_CALL(env2_realm, unofficial_napi_create_realm(env2_realm, NULL, &env2_nested, NULL));
  int32_t nested_value = 0;
  NAPI_CALL(env2_nested, RunInt32(env2_nested, "globalThis.v = 12", &nested_value));
  CHECK_OR_FAIL(nested_value == 12, "the nested realm did not run");
  NAPI_CALL(env, unofficial_napi_release_env(env2_scope, NULL));
#ifdef __wasm__
  // Guest env ids of released realm envs no longer resolve. (Natively the
  // handles are freed pointers and must not be used.)
  napi_value stale_global = NULL;
  CHECK_OR_FAIL(napi_get_global(env2_realm, &stale_global) != napi_ok &&
                    napi_get_global(env2_nested, &stale_global) != napi_ok,
                "a released realm env still resolves");
#endif
  printf("released an env with live realm envs\n");

  return PrintSuccess("TEST_REALM");
}
