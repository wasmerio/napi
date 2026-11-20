#ifndef NAPI_ERROR_STATE_H_
#define NAPI_ERROR_STATE_H_

#include "js_native_api.h"

#include <string>

namespace napi {

class error_state__ {
 public:
  const napi_extended_error_info* info() const;
  napi_status set(napi_status status, const char* message);
  // Every successful call clears the last error, and nearly always there is
  // none to clear: that case writes nothing and stays in the caller. What
  // set() leaves for (napi_ok, nullptr) is exactly the state tested here.
  napi_status clear() {
    if (info_.error_code == napi_ok && info_.error_message == nullptr) [[likely]] {
      return napi_ok;
    }
    return set(napi_ok, nullptr);
  }

 private:
  napi_extended_error_info info_{};
  std::string message_;
};

}  // namespace napi

#endif  // NAPI_ERROR_STATE_H_
