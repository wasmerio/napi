#ifndef NAPI_QUICKJS_CALLBACK_INFO_H_
#define NAPI_QUICKJS_CALLBACK_INFO_H_

#include "../../../include/js_native_api.h"

#include <cstddef>
#include <quickjs.h>

struct napi_callback_info__
{
  napi_callback_info__(napi_env env,
                       JSValueConst this_val,
                       JSValue new_target,
                       int argc,
                       JSValueConst *argv,
                       void *data);

  napi_env env() const;
  JSValueConst this_value() const;
  JSValue new_target() const;
  size_t argc() const;
  JSValueConst arg(size_t index) const;
  void *data() const;

private:
  napi_env env_;
  JSValueConst this_val_;
  JSValue new_target_;
  int argc_;
  JSValueConst *argv_;
  void *data_;
};

#endif // NAPI_QUICKJS_CALLBACK_INFO_H_
