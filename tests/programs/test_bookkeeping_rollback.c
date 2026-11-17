#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "napi_test_helpers.h"

// Fills the bridge's bookkeeping budget with references, then checks that
// calls refused for bookkeeping leave nothing behind and do not pretend to
// succeed, and that a name copy the budget refuses fails the call instead of
// becoming "".

static int g_finalized = 0;
static void Finalize(napi_env env, void* data, void* hint) {
  (void)env;
  (void)data;
  (void)hint;
  g_finalized++;
}

static napi_value Noop(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  napi_value object;
  napi_value target;
  napi_value plain_target;
  NAPI_CALL(env, napi_create_object(env, &object));
  NAPI_CALL(env, napi_create_object(env, &target));
  NAPI_CALL(env, napi_create_object(env, &plain_target));

  const size_t ceiling = 4 * 1024 * 1024;
  napi_ref* refs = malloc(ceiling * sizeof(napi_ref));
  CHECK_OR_FAIL(refs != NULL, "malloc failed");
  size_t count = 0;
  while (count < ceiling && napi_create_reference(env, object, 1, &refs[count]) == napi_ok) count++;
  CHECK_OR_FAIL(count < ceiling, "bookkeeping was never refused");

  // A wrap with a reference: refused before V8 installs it.
  napi_ref wrap_ref = NULL;
  napi_status status = napi_wrap(env, target, (void*)0x1234, Finalize, NULL, &wrap_ref);
  printf("WRAP=%d\n", (int)status);
  CHECK_OR_FAIL(status != napi_ok, "the wrap was not refused");
  void* unwrapped = NULL;
  CHECK_OR_FAIL(napi_unwrap(env, target, &unwrapped) != napi_ok,
                "a refused wrap left the object wrapped");

  // The same without a finalizer: the wrap and its reference are made
  // separately, and the refused reference takes the wrap back off.
  status = napi_wrap(env, plain_target, (void*)0x5678, NULL, NULL, &wrap_ref);
  printf("PLAIN_WRAP=%d\n", (int)status);
  CHECK_OR_FAIL(status != napi_ok, "the plain wrap was not refused");
  CHECK_OR_FAIL(napi_unwrap(env, plain_target, &unwrapped) != napi_ok,
                "a refused plain wrap left the object wrapped");

  // A promise: refused, not a success without a handle.
  napi_deferred deferred = NULL;
  napi_value promise = NULL;
  status = napi_create_promise(env, &deferred, &promise);
  printf("PROMISE=%d\n", (int)status);
  CHECK_OR_FAIL(status != napi_ok, "the promise was not refused");

  // A function: refused, not a function whose callback is dead.
  napi_value function = NULL;
  status = napi_create_function(env, "f", NAPI_AUTO_LENGTH, Noop, NULL, &function);
  printf("FUNCTION=%d\n", (int)status);
  CHECK_OR_FAIL(status != napi_ok, "the function was not refused");

  // The budget gets back what the references held.
  for (size_t i = 0; i < count; i++) NAPI_CALL(env, napi_delete_reference(env, refs[i]));
  free(refs);

  // Room for bookkeeping again; the wrap and promise now work.
  NAPI_CALL(env, napi_wrap(env, target, (void*)0x1234, Finalize, NULL, &wrap_ref));
  NAPI_CALL(env, napi_unwrap(env, target, &unwrapped));
  CHECK_OR_FAIL(unwrapped == (void*)0x1234, "wrong wrapped pointer");
  NAPI_CALL(env, napi_create_promise(env, &deferred, &promise));
  CHECK_OR_FAIL(promise != NULL, "no promise");
  napi_value undefined;
  NAPI_CALL(env, napi_get_undefined(env, &undefined));
  NAPI_CALL(env, napi_resolve_deferred(env, deferred, undefined));

  NAPI_CALL(env, napi_wrap(env, plain_target, (void*)0x5678, NULL, NULL, &wrap_ref));
  NAPI_CALL(env, napi_unwrap(env, plain_target, &unwrapped));
  CHECK_OR_FAIL(unwrapped == (void*)0x5678, "wrong plain wrapped pointer");

  puts("BOOKKEEPING_ROLLBACK_OK");
  return 0;
}
