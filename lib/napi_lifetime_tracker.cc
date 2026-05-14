#include "napi_lifetime_tracker.h"

#include <chrono>
#include <cstdlib>

namespace napi::lifetime {

bool env_flag_enabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool env_flag_enabled_or(const char* name, bool fallback) {
  const char* value = std::getenv(name);
  if (value != nullptr && value[0] != '\0') {
    return value[0] != '0';
  }
  return fallback;
}

int64_t monotonic_milliseconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock::now().time_since_epoch())
      .count();
}

}  // namespace napi::lifetime
