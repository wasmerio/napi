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

// Runs `source` in `realm`, or in the env's own realm when it is NULL.
static napi_status RunIn(napi_env env,
                         unofficial_napi_realm realm,
                         const char* source,
                         napi_value* result) {
  napi_value text = NULL;
  napi_value filename = NULL;
  napi_status status = napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &text);
  if (status != napi_ok) return status;
  status = napi_create_string_utf8(env, "realm.js", NAPI_AUTO_LENGTH, &filename);
  if (status != napi_ok) return status;
  const unofficial_napi_js_source script = unofficial_napi_js_source_from_text(text);
  return unofficial_napi_run_script(
      env, realm, &script, filename, 0, 0, -1, true, false, false, NULL, result);
}

static napi_status RunInInt32(napi_env env,
                              unofficial_napi_realm realm,
                              const char* source,
                              int32_t* out) {
  napi_value result = NULL;
  napi_status status = RunIn(env, realm, source, &result);
  if (status != napi_ok) return status;
  return napi_get_value_int32(env, result, out);
}

static bool StrictEquals(napi_env env, napi_value a, napi_value b) {
  bool same = false;
  return napi_strict_equals(env, a, b, &same) == napi_ok && same;
}

static napi_value Str(napi_env env, const char* value) {
  napi_value out = NULL;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out) != napi_ok) return NULL;
  return out;
}

static napi_status ReadInt32Export(napi_env env,
                                   unofficial_napi_module module,
                                   const char* name,
                                   int32_t* out) {
  napi_value ns = NULL;
  napi_value value = NULL;
  napi_status status = unofficial_napi_module_wrap_get_namespace(env, module, &ns);
  if (status != napi_ok) return status;
  status = napi_get_named_property(env, ns, name, &value);
  if (status != napi_ok) return status;
  return napi_get_value_int32(env, value, out);
}

// Links, instantiates and evaluates a module with no imports.
static napi_status EvaluateModule(napi_env env, unofficial_napi_module module) {
  napi_status status = unofficial_napi_module_wrap_link(env, module, 0, NULL);
  if (status != napi_ok) return status;
  status = unofficial_napi_module_wrap_instantiate(env, module);
  if (status != napi_ok) return status;
  napi_value result = NULL;
  return unofficial_napi_module_wrap_evaluate(env, module, -1, false, &result);
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  // A realm has its own global.
  unofficial_napi_realm realm = NULL;
  napi_value realm_global = NULL;
  NAPI_CALL(env, unofficial_napi_create_realm(env, NULL, &realm, &realm_global));
  CHECK_OR_FAIL(realm != NULL, "create_realm did not return a realm");
  napi_value env_global = NULL;
  napi_value realm_own_global = NULL;
  NAPI_CALL(env, napi_get_global(env, &env_global));
  NAPI_CALL(env, RunIn(env, realm, "globalThis", &realm_own_global));
  CHECK_OR_FAIL(StrictEquals(env, realm_own_global, realm_global),
                "the realm's global is not the one create_realm returned");
  CHECK_OR_FAIL(!StrictEquals(env, env_global, realm_global),
                "the realm shares the env's global");
  printf("realm has its own global\n");

  // Values cross realms.
  napi_value shared = NULL;
  NAPI_CALL(env, napi_create_object(env, &shared));
  NAPI_CALL(env, napi_set_named_property(env, realm_global, "shared", shared));
  int32_t x = 0;
  NAPI_CALL(env, RunInInt32(env, realm, "shared.x = 7", &x));
  napi_value x_value = NULL;
  NAPI_CALL(env, napi_get_named_property(env, shared, "x", &x_value));
  NAPI_CALL(env, napi_get_value_int32(env, x_value, &x));
  CHECK_OR_FAIL(x == 7, "a value did not cross realms");
  printf("values cross realms: x=%d\n", x);

  // A native function belongs to the env's realm, also when code in another
  // realm calls it.
  callback_record record = {env_global, NULL, false, 0};
  napi_value fn = NULL;
  NAPI_CALL(env, napi_create_function(
                     env, "probe", NAPI_AUTO_LENGTH, RecordCallback, &record, &fn));
  NAPI_CALL(env, napi_set_named_property(env, realm_global, "probe", fn));
  napi_value probe_result = NULL;
  NAPI_CALL(env, RunIn(env, realm, "probe()", &probe_result));
  CHECK_OR_FAIL(record.calls == 1, "the callback did not run");
  CHECK_OR_FAIL(record.env == env, "the callback did not get its env");
  CHECK_OR_FAIL(record.saw_expected_global, "the callback did not see the env's global");
  printf("native function stays in the env's realm\n");

  // get_realm finds a realm by its global; the env's own global is not one.
  unofficial_napi_realm found = NULL;
  NAPI_CALL(env, unofficial_napi_get_realm(env, realm_global, &found));
  CHECK_OR_FAIL(found == realm, "get_realm(realm global) did not return the realm");
  found = realm;
  NAPI_CALL(env, unofficial_napi_get_realm(env, env_global, &found));
  CHECK_OR_FAIL(found == NULL, "get_realm(env global) returned a realm");
  printf("get_realm finds the realm by its global\n");

  // A sandbox realm reads and writes the sandbox.
  napi_value sandbox = NULL;
  napi_value five = NULL;
  NAPI_CALL(env, napi_create_object(env, &sandbox));
  NAPI_CALL(env, napi_create_int32(env, 5, &five));
  NAPI_CALL(env, napi_set_named_property(env, sandbox, "input", five));
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = sandbox;
  unofficial_napi_realm sandbox_realm = NULL;
  napi_value sandbox_global = NULL;
  NAPI_CALL(env, unofficial_napi_create_realm(env, &options, &sandbox_realm, &sandbox_global));
  CHECK_OR_FAIL(StrictEquals(env, sandbox_global, sandbox),
                "create_realm did not return the sandbox as the global");
  found = NULL;
  NAPI_CALL(env, unofficial_napi_get_realm(env, sandbox, &found));
  CHECK_OR_FAIL(found == sandbox_realm, "get_realm(sandbox) did not return its realm");
  int32_t input = 0;
  NAPI_CALL(env, RunInInt32(env, sandbox_realm, "output = input * 2; input", &input));
  napi_value output_value = NULL;
  int32_t output = 0;
  NAPI_CALL(env, napi_get_named_property(env, sandbox, "output", &output_value));
  NAPI_CALL(env, napi_get_value_int32(env, output_value, &output));
  CHECK_OR_FAIL(input == 5 && output == 10, "the sandbox realm did not use the sandbox");
  printf("sandbox realm: input=%d output=%d\n", input, output);

  // A script, a compiled function, a module and bytecode, all for a sandbox
  // realm, see the sandbox (x = 3).
  napi_value context_sandbox = NULL;
  napi_value three = NULL;
  NAPI_CALL(env, napi_create_object(env, &context_sandbox));
  NAPI_CALL(env, napi_create_int32(env, 3, &three));
  NAPI_CALL(env, napi_set_named_property(env, context_sandbox, "x", three));
  options.sandbox_or_null = context_sandbox;
  unofficial_napi_realm context_realm = NULL;
  NAPI_CALL(env, unofficial_napi_create_realm(env, &options, &context_realm, NULL));
  napi_value undefined = NULL;
  NAPI_CALL(env, napi_get_undefined(env, &undefined));
  int32_t ran = 0;
  NAPI_CALL(env, RunInInt32(env, context_realm, "x + 1", &ran));
  const unofficial_napi_js_source function_source =
      unofficial_napi_js_source_from_text(Str(env, "return x * 10;"));
  napi_value compiled = NULL;
  napi_value value = NULL;
  int32_t called = 0;
  NAPI_CALL(env, unofficial_napi_compile_function(env, context_realm, &function_source,
                                                  Str(env, "f.js"), 0, 0, undefined, undefined,
                                                  undefined, &compiled));
  NAPI_CALL(env, napi_get_named_property(env, compiled, "function", &fn));
  NAPI_CALL(env, napi_call_function(env, undefined, fn, 0, NULL, &value));
  NAPI_CALL(env, napi_get_value_int32(env, value, &called));
  napi_value wrapper = NULL;
  NAPI_CALL(env, napi_create_object(env, &wrapper));
  const unofficial_napi_js_source module_source =
      unofficial_napi_js_source_from_text(Str(env, "export const y = x + 2;"));
  unofficial_napi_module_create_options module_options = {0};
  module_options.size = sizeof(module_options);
  module_options.version = UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION;
  module_options.kind = unofficial_napi_module_source_text;
  module_options.wrapper = wrapper;
  module_options.url = Str(env, "file:///x.mjs");
  module_options.realm_or_null = context_realm;
  module_options.payload.source_text.source = &module_source;
  module_options.payload.source_text.host_defined_option_id = undefined;
  unofficial_napi_module_create_result created = {0};
  NAPI_CALL(env, unofficial_napi_module_wrap_create(env, &module_options, &created));
  int32_t exported = 0;
  NAPI_CALL(env, EvaluateModule(env, created.module));
  NAPI_CALL(env, ReadInt32Export(env, created.module, "y", &exported));
  NAPI_CALL(env, unofficial_napi_module_wrap_destroy(env, created.module));
  unofficial_napi_bytecode_open_options open_options = {0};
  open_options.size = sizeof(open_options);
  open_options.version = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION;
  open_options.realm_or_null = context_realm;
  open_options.source_text = Str(env, "x * 2");
  open_options.filename = Str(env, "b.js");
  open_options.shape = unofficial_napi_bytecode_shape_script;
  open_options.params_or_undefined = undefined;
  open_options.host_defined_option_id = undefined;
  unofficial_napi_bytecode_open_result opened = {0};
  NAPI_CALL(env, unofficial_napi_bytecode_open(env, &open_options, &opened));
  const unofficial_napi_js_source bytecode_source =
      unofficial_napi_js_source_from_bytecode(opened.bytecode);
  int32_t from_bytecode = 0;
  NAPI_CALL(env, unofficial_napi_run_script(env, context_realm, &bytecode_source,
                                            Str(env, "b.js"), 0, 0, -1, true, false, false,
                                            NULL, &value));
  NAPI_CALL(env, napi_get_value_int32(env, value, &from_bytecode));
  NAPI_CALL(env, unofficial_napi_bytecode_release(env, opened.bytecode));
  CHECK_OR_FAIL(ran == 4 && called == 30 && exported == 5 && from_bytecode == 6,
                "code did not run in its sandbox's realm");
  printf("vm results: run=%d function=%d module=%d bytecode=%d\n", ran, called, exported,
         from_bytecode);

  // Realms nothing refers to any more are collected.
  enum { kDropped = 6 };
  napi_ref dropped[kDropped];
  for (int i = 0; i < kDropped; ++i) {
    napi_handle_scope scope = NULL;
    NAPI_CALL(env, napi_open_handle_scope(env, &scope));
    napi_value dropped_sandbox = NULL;
    if (i % 2 == 0) NAPI_CALL(env, napi_create_object(env, &dropped_sandbox));
    options.sandbox_or_null = dropped_sandbox;
    unofficial_napi_realm dropped_realm = NULL;
    napi_value dropped_global = NULL;
    NAPI_CALL(env, unofficial_napi_create_realm(env, &options, &dropped_realm, &dropped_global));
    napi_value ignored = NULL;
    NAPI_CALL(env, RunIn(env, dropped_realm, "globalThis.payload = new Array(1024).fill(1)",
                         &ignored));
    NAPI_CALL(env, napi_create_reference(env, dropped_global, 0, &dropped[i]));
    NAPI_CALL(env, napi_close_handle_scope(env, scope));
  }
  NAPI_CALL(env, unofficial_napi_collect_garbage(env));
  int collected = 0;
  for (int i = 0; i < kDropped; ++i) {
    napi_value global = NULL;
    NAPI_CALL(env, napi_get_reference_value(env, dropped[i], &global));
    if (global == NULL) collected++;
    NAPI_CALL(env, napi_delete_reference(env, dropped[i]));
  }
  CHECK_OR_FAIL(collected == kDropped, "a dropped realm was not collected");
  printf("collected %d of %d dropped realms\n", collected, kDropped);

  // Releasing an env with live realms releases them with it.
  napi_env env2 = NULL;
  unofficial_napi_env_owner env2_scope = NULL;
  NAPI_CALL(env, unofficial_napi_create_env(8, NULL, &env2, &env2_scope));
  unofficial_napi_realm env2_realm = NULL;
  NAPI_CALL(env2, unofficial_napi_create_realm(env2, NULL, &env2_realm, NULL));
  int32_t realm_value = 0;
  NAPI_CALL(env2, RunInInt32(env2, env2_realm, "globalThis.v = 12", &realm_value));
  CHECK_OR_FAIL(realm_value == 12, "the second env's realm did not run");
  NAPI_CALL(env, unofficial_napi_release_env(env2_scope, NULL));
  printf("released an env with live realms\n");

  return PrintSuccess("TEST_REALM");
}
