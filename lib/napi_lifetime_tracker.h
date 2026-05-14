#ifndef NAPI_LIFETIME_TRACKER_H_
#define NAPI_LIFETIME_TRACKER_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace napi::lifetime {

struct type_stats {
  std::size_t created = 0;
  std::size_t released = 0;
  std::size_t active = 0;
  std::size_t peak = 0;
};

bool env_flag_enabled(const char* name);
bool env_flag_enabled_or(const char* name, bool fallback);
int64_t monotonic_milliseconds();

inline void record_create(type_stats& stats) {
  ++stats.created;
  ++stats.active;
  stats.peak = std::max(stats.peak, stats.active);
}

inline void record_release(type_stats& stats) {
  ++stats.released;
  if (stats.active > 0) {
    --stats.active;
  }
}

}  // namespace napi::lifetime

#endif  // NAPI_LIFETIME_TRACKER_H_
