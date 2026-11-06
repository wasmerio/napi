#ifndef NAPI_TESTING_H_
#define NAPI_TESTING_H_

#include <cstddef>

#include "js_native_api.h"

// For tests only: what the public API cannot observe. Each provider
// implements it.
namespace napi::testing {

// The number of napi_values the open handle scopes of `env` hold.
size_t open_value_count(napi_env env);

}  // namespace napi::testing

#endif  // NAPI_TESTING_H_
