#ifndef NAPI_QUICKJS_JS_NATIVE_API_NODE_COMPAT_H_
#define NAPI_QUICKJS_JS_NATIVE_API_NODE_COMPAT_H_

#include "js_native_api.h"
#include <quickjs.h>

namespace quickjs::detail
{
  void clear_quickjs_exception(JSContext *ctx);
  void install_runtime_buffer_prototype(napi_env env, JSValueConst buffer);
  int set_property_with_node_compat(JSContext *ctx,
                                    JSValueConst object,
                                    JSAtom property,
                                    JSValueConst value);
}

#endif // NAPI_QUICKJS_JS_NATIVE_API_NODE_COMPAT_H_
