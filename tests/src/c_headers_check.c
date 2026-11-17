/* The public headers must compile as plain C11, their C _Static_asserts
 * included (the wasm32 wire layouts in unofficial_napi_wasm32_abi.h). Built
 * as an object library: compiling it is the check. */
#include "js_native_api.h"
#include "node_api.h"
#include "unofficial_napi.h"

int napi_c_headers_check(void);
int napi_c_headers_check(void) {
  return (int)sizeof(unofficial_napi_env_hooks) +
         (int)sizeof(unofficial_napi_wasm32_env_hooks_v1);
}
