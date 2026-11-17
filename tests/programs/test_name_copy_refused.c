#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "napi_test_helpers.h"

// argv[1]: the length in MiB of a NAPI_AUTO_LENGTH name the budget cannot
// copy. Defining a property, a class or a function by it must fail, not use
// the name "".

static napi_value Noop(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

int main(int argc, char** argv) {
  CHECK_OR_FAIL(argc == 2, "usage: test_name_copy_refused <MiB>");
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  const size_t length = strtoul(argv[1], NULL, 10) * 1024 * 1024;
  char* name = malloc(length + 1);
  CHECK_OR_FAIL(name != NULL, "malloc failed");
  memset(name, 'n', length);
  name[length] = '\0';

  napi_value object;
  NAPI_CALL(env, napi_create_object(env, &object));
  napi_value value;
  NAPI_CALL(env, napi_create_int32(env, 1, &value));
  napi_property_descriptor descriptor = {name, NULL, NULL, NULL, NULL, value, napi_default, NULL};
  napi_status status = napi_define_properties(env, object, 1, &descriptor);
  printf("DEFINE_PROPERTIES=%d\n", (int)status);
  CHECK_OR_FAIL(status != napi_ok, "a refused name was defined");
  bool has_empty = true;
  NAPI_CALL(env, napi_has_named_property(env, object, "", &has_empty));
  CHECK_OR_FAIL(!has_empty, "the refused name became \"\"");

  napi_value function = NULL;
  status = napi_create_function(env, name, NAPI_AUTO_LENGTH, Noop, NULL, &function);
  printf("CREATE_FUNCTION=%d\n", (int)status);
  CHECK_OR_FAIL(status != napi_ok, "a function was named after a refused copy");

  napi_value klass = NULL;
  status = napi_define_class(env, name, NAPI_AUTO_LENGTH, Noop, NULL, 0, NULL, &klass);
  printf("DEFINE_CLASS=%d\n", (int)status);
  CHECK_OR_FAIL(status != napi_ok, "a class was named after a refused copy");

  // A class property named by it.
  napi_property_descriptor method = {name, NULL, Noop, NULL, NULL, NULL, napi_default, NULL};
  status = napi_define_class(env, "C", NAPI_AUTO_LENGTH, Noop, NULL, 1, &method, &klass);
  printf("CLASS_PROPERTY=%d\n", (int)status);
  CHECK_OR_FAIL(status != napi_ok, "a class property was named after a refused copy");

  // An error code: the throw fails rather than throwing without the code.
  status = napi_throw_error(env, name, "message");
  printf("THROW_CODE=%d\n", (int)status);
  CHECK_OR_FAIL(status != napi_ok, "an error was thrown without its refused code");
  bool pending = true;
  NAPI_CALL(env, napi_is_exception_pending(env, &pending));
  CHECK_OR_FAIL(!pending, "a refused throw left an exception");

  // A short name still works.
  descriptor.utf8name = "ok";
  NAPI_CALL(env, napi_define_properties(env, object, 1, &descriptor));
  puts("NAME_COPY_REFUSED_OK");
  free(name);
  return 0;
}
