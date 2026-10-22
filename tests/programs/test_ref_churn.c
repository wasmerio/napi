#include <stdio.h>
#include <stdint.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

// One host handle per short-lived JS object, the way a workload keeps a
// native handle for every hash or stream it creates. Nothing retains the
// objects, but they are too small for the JS heap to ever collect them on
// its own: only the provider's bookkeeping charge asks V8 for the collection
// that lets the finalizers release the refs. Well past the fixed cap a
// finite budget used to impose.
#define ITERATIONS 16384u

static unsigned finalized = 0;

static void ReleaseRef(napi_env env, void* data, void* hint) {
  (void)hint;
  if (napi_delete_reference(env, (napi_ref)data) == napi_ok) finalized++;
}

// Guest finalizers only run at a provider checkpoint.
static int Checkpoint(napi_env env) {
  uint32_t state = 0;
  NAPI_CALL(env, unofficial_napi_event_loop_checkpoint(
                     env, unofficial_napi_event_loop_checkpoint_microtasks,
                     false, &state));
  return 0;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  for (unsigned i = 0; i < ITERATIONS; ++i) {
    napi_handle_scope scope;
    NAPI_CALL(env, napi_open_handle_scope(env, &scope));
    napi_value obj;
    NAPI_CALL(env, napi_create_object(env, &obj));
    napi_ref ref;
    NAPI_CALL(env, napi_create_reference(env, obj, 0, &ref));
    NAPI_CALL(env, napi_add_finalizer(env, obj, ref, ReleaseRef, NULL, NULL));
    NAPI_CALL(env, napi_close_handle_scope(env, scope));
    if (i % 512 == 0 && Checkpoint(env) != 0) return 1;
  }
  if (Checkpoint(env) != 0) return 1;

  CHECK_OR_FAIL(finalized > 0,
                "no reference was released: V8 was never asked to collect");
  printf("REF_CHURN_OK finalized=%u\n", finalized);
  return 0;
}
