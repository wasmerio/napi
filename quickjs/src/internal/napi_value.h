#ifndef NAPI_QUICKJS_VALUE_H_
#define NAPI_QUICKJS_VALUE_H_

#include "../../../include/js_native_api.h"

#include <quickjs.h>

struct napi_value__
{
  static napi_value__ *create(napi_env env, JSValue value, bool owned);
  static void destroy(napi_value__ *value);

  ~napi_value__();

  JSValueConst get_inner() const;

private:
  napi_value__(napi_env env, JSValue value, bool owned);

  napi_env env_;
  JSValue value_;
};

#endif // NAPI_QUICKJS_VALUE_H_
