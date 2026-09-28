#include <stdio.h>

#include "unofficial_napi.h"

int main(void) {
  // The first byte is NUL, so an accidental strlen() would see no flags and
  // allow the guest to configure process-wide V8 state for other workloads.
  static const char flags[] = {'\0', '-', '-', 'n', 'o', '-', 'j', 'i', 't'};
  unofficial_napi_runtime_options options = {
      .size = sizeof(options),
      .version = UNOFFICIAL_NAPI_RUNTIME_OPTIONS_VERSION,
      .engine_flags = flags,
      .engine_flags_length = sizeof(flags),
  };
  if (unofficial_napi_configure_runtime(&options) != napi_invalid_arg) {
    return 1;
  }
  puts("FLAGS_REJECTED");
  return 0;
}
