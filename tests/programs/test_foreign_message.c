#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "napi_test_helpers.h"
#include "unofficial_napi.h"

// Tries to take, then drop, the message whose id is argv[1]: a message of
// another context.
int main(int argc, char** argv) {
  CHECK_OR_FAIL(argc == 2, "usage: test_foreign_message <message id>");
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");
  unofficial_napi_message foreign =
      (unofficial_napi_message)(uintptr_t)strtoul(argv[1], NULL, 10);
  napi_value value = NULL;
  napi_status status = unofficial_napi_message_take(env, foreign, &value);
  CHECK_OR_FAIL(status != napi_ok && value == NULL,
                "took another context's message");
  unofficial_napi_message_drop(foreign);
  puts("FOREIGN_MESSAGE_REFUSED");
  return 0;
}
