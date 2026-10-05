#include <stdio.h>

#include "unofficial_napi.h"

int main(void) {
  if (unofficial_napi_configure_runtime(NULL) != napi_ok) return 1;
  puts("CONFIGURED_WITHOUT_ENV");
  return 0;
}
