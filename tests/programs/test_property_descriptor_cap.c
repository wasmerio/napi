#include <stdio.h>

#include "napi_test_helpers.h"

static napi_value constructor(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value value = NULL;
  napi_get_undefined(env, &value);
  return value;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  napi_value object;
  NAPI_CALL(env, napi_create_object(env, &object));

  // The bridge must reject the count before reading descriptors or allocating
  // host arrays: only one descriptor is actually present at this pointer.
  napi_property_descriptor one = {0};
  if (napi_define_properties(env, object, 4097, &one) != napi_invalid_arg) {
    return 1;
  }
  napi_value klass;
  if (napi_define_class(env, "Cap", NAPI_AUTO_LENGTH, constructor, NULL,
                        4097, &one, &klass) != napi_invalid_arg) {
    return 2;
  }
  puts("PROPERTY_CAP_REJECTED");
  return 0;
}
