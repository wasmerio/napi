// unofficial_napi_contextify_enter / unofficial_napi_contextify_leave through
// the guest imports: realm selection for ordinary N-API calls and for native
// callbacks, 64-bit tokens, LIFO checks, and output pointer validation.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

static napi_value host_defined_option;

static napi_value Text(napi_env env, const char* text) {
  napi_value value = NULL;
  napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &value);
  return value;
}

static napi_status Run(napi_env env, napi_value sandbox, const char* code,
                       napi_value* result) {
  unofficial_napi_js_source source =
      unofficial_napi_js_source_from_text(Text(env, code));
  return unofficial_napi_contextify_run_script(
      env, sandbox, &source, Text(env, "contextify-enter.js"), 0, 0, -1, true,
      false, false, host_defined_option, result);
}

static bool ScriptIsTrue(napi_env env, napi_value sandbox, const char* code) {
  napi_value result = NULL;
  bool value = false;
  if (Run(env, sandbox, code, &result) != napi_ok ||
      napi_get_value_bool(env, result, &value) != napi_ok) {
    bool pending = false;
    napi_value exception = NULL;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_get_and_clear_last_exception(env, &exception);
    }
    return false;
  }
  return value;
}

static bool Same(napi_env env, napi_value left, napi_value right) {
  bool same = false;
  return napi_strict_equals(env, left, right, &same) == napi_ok && same;
}

static napi_value CurrentGlobal(napi_env env) {
  napi_value global = NULL;
  napi_get_global(env, &global);
  return global;
}

static napi_value NativeGlobal(napi_env env, napi_callback_info info) {
  (void)info;
  return CurrentGlobal(env);
}

static napi_value NativeObject(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value value = NULL;
  napi_create_object(env, &value);
  return value;
}

// Creates a context and installs callbacks while that context is entered.
static napi_value MakeRealm(napi_env env, const char* name) {
  napi_value sandbox = NULL;
  napi_value ignored = NULL;
  napi_value function = NULL;
  unofficial_napi_context_scope scope = 0;
  if (napi_create_object(env, &sandbox) != napi_ok ||
      unofficial_napi_contextify_make_context(
          env, sandbox, Text(env, name), Text(env, "test://contextify-enter"),
          true, true, false, host_defined_option, &ignored) != napi_ok ||
      unofficial_napi_contextify_enter(env, sandbox, &scope) != napi_ok) {
    return NULL;
  }
  bool installed =
      napi_create_function(env, "nativeGlobal", NAPI_AUTO_LENGTH, NativeGlobal,
                           NULL, &function) == napi_ok &&
      napi_set_named_property(env, sandbox, "nativeGlobal", function) ==
          napi_ok &&
      napi_create_function(env, "nativeObject", NAPI_AUTO_LENGTH, NativeObject,
                           NULL, &function) == napi_ok &&
      napi_set_named_property(env, sandbox, "nativeObject", function) ==
          napi_ok;
  if (unofficial_napi_contextify_leave(env, scope) != napi_ok || !installed) {
    return NULL;
  }
  return sandbox;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  NAPI_CALL(env, napi_create_symbol(env, Text(env, "contextify-enter"),
                                    &host_defined_option));

  napi_value root = CurrentGlobal(env);
  napi_value parent = MakeRealm(env, "parent");
  napi_value child = MakeRealm(env, "child");
  CHECK_OR_FAIL(parent != NULL && child != NULL, "create contexts");
  napi_value parent_global = NULL;
  napi_value child_global = NULL;
  NAPI_CALL(env, Run(env, parent, "globalThis", &parent_global));
  NAPI_CALL(env, Run(env, child, "globalThis", &child_global));
  CHECK_OR_FAIL(!Same(env, parent_global, child_global), "distinct globals");

  // Ordinary N-API calls allocate in the entered context.
  unofficial_napi_context_scope outer = 0;
  unofficial_napi_context_scope inner = 0;
  unofficial_napi_context_scope newer = 0;
  NAPI_CALL(env, unofficial_napi_contextify_enter(env, parent, &outer));
  CHECK_OR_FAIL(outer != 0, "enter returns a nonzero token");
  CHECK_OR_FAIL(Same(env, CurrentGlobal(env), parent_global),
                "napi_get_global selects the entered context");
  napi_value object = NULL;
  napi_value array = NULL;
  NAPI_CALL(env, napi_create_object(env, &object));
  NAPI_CALL(env, napi_create_array(env, &array));
  NAPI_CALL(env, napi_set_named_property(env, parent_global, "allocatedObject",
                                         object));
  NAPI_CALL(env, napi_set_named_property(env, parent_global, "allocatedArray",
                                         array));

  // Strict LIFO: an out-of-order leave fails and keeps the selection.
  NAPI_CALL(env, unofficial_napi_contextify_enter(env, child, &inner));
  CHECK_OR_FAIL(inner != outer, "distinct tokens");
  CHECK_OR_FAIL(unofficial_napi_contextify_leave(env, outer) == napi_invalid_arg,
                "non-LIFO leave is rejected");
  CHECK_OR_FAIL(Same(env, CurrentGlobal(env), child_global),
                "rejected leave keeps the selection");
  NAPI_CALL(env, unofficial_napi_contextify_leave(env, inner));
  CHECK_OR_FAIL(Same(env, CurrentGlobal(env), parent_global),
                "leave restores the outer context");
  NAPI_CALL(env, unofficial_napi_contextify_enter(env, child, &newer));
  CHECK_OR_FAIL(newer != inner, "tokens are not reused");
  CHECK_OR_FAIL(unofficial_napi_contextify_leave(env, inner) == napi_invalid_arg,
                "closed token is rejected");
  NAPI_CALL(env, unofficial_napi_contextify_leave(env, newer));

  // A null sandbox selects the root context.
  NAPI_CALL(env, unofficial_napi_contextify_enter(env, NULL, &inner));
  CHECK_OR_FAIL(Same(env, CurrentGlobal(env), root), "null selects the root");
  NAPI_CALL(env, unofficial_napi_contextify_leave(env, inner));
  NAPI_CALL(env, unofficial_napi_contextify_leave(env, outer));
  CHECK_OR_FAIL(Same(env, CurrentGlobal(env), root), "root after leave");

  CHECK_OR_FAIL(
      ScriptIsTrue(env, parent,
                   "Object.getPrototypeOf(allocatedObject) === Object.prototype "
                   "&& Object.getPrototypeOf(allocatedArray) === Array.prototype"),
      "values allocated while entered belong to that context");
  CHECK_OR_FAIL(ScriptIsTrue(env, parent, "nativeGlobal() === globalThis"),
                "callback runs in its creation context");
  CHECK_OR_FAIL(
      ScriptIsTrue(env, child,
                   "Object.getPrototypeOf(nativeObject()) === Object.prototype"),
      "callback allocates in its creation context");

  // Leaving keeps an exception raised while the scope was open.
  napi_value ignored = NULL;
  NAPI_CALL(env, unofficial_napi_contextify_enter(env, parent, &outer));
  CHECK_OR_FAIL(Run(env, child, "throw 91", &ignored) == napi_pending_exception,
                "script throws");
  NAPI_CALL(env, unofficial_napi_contextify_leave(env, outer));
  bool pending = false;
  napi_value exception = NULL;
  int32_t thrown = 0;
  NAPI_CALL(env, napi_is_exception_pending(env, &pending));
  CHECK_OR_FAIL(pending, "leave preserves the pending exception");
  NAPI_CALL(env, napi_get_and_clear_last_exception(env, &exception));
  NAPI_CALL(env, napi_get_value_int32(env, exception, &thrown));
  CHECK_OR_FAIL(thrown == 91, "original exception value");

  // Invalid arguments fail without leaving a scope open.
  napi_value plain = NULL;
  unofficial_napi_context_scope token = 123;
  NAPI_CALL(env, napi_create_object(env, &plain));
  CHECK_OR_FAIL(unofficial_napi_contextify_enter(env, plain, &token) ==
                        napi_invalid_arg &&
                    token == 0,
                "unregistered object is rejected");
  CHECK_OR_FAIL(unofficial_napi_contextify_enter(env, parent, NULL) ==
                    napi_invalid_arg,
                "null output pointer is rejected");
#ifdef __wasm__
  // The token is 8 bytes; this address leaves only 4 below the 4 GiB limit.
  CHECK_OR_FAIL(unofficial_napi_contextify_enter(
                    env, parent,
                    (unofficial_napi_context_scope*)(uintptr_t)0xfffffffcu) ==
                    napi_invalid_arg,
                "out-of-bounds output pointer is rejected");
#endif
  CHECK_OR_FAIL(unofficial_napi_contextify_leave(env, 0) == napi_invalid_arg,
                "zero token is rejected");
  CHECK_OR_FAIL(Same(env, CurrentGlobal(env), root),
                "rejected calls leave the root selected");

  return PrintSuccess("TEST_CONTEXTIFY_ENTER");
}
