#ifndef NAPI_QUICKJS_ENV_H_
#define NAPI_QUICKJS_ENV_H_

// TBD: relative path makes VCode less confused - probably need to fix CMakeFiles or VSCode settings
// I think VSCode is confused with includes in /node directory.
#include "../../../include/js_native_api.h"

#include <string>
#include <quickjs.h>

struct napi_value__ {
  explicit napi_value__(napi_env env, JSValue local);
  ~napi_value__();

  JSValue local() const;

  napi_env env;
  JSValue value;
};

struct napi_env__
{
    JSContext *context() const;

    JSContext *ctx;
    napi_extended_error_info last_error{};
    std::string last_error_message;
};

#endif // NAPI_QUICKJS_ENV_H_