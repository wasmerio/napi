#include "internal/napi_env.h"

#include "internal/napi_external.h"

napi_env__::napi_env__(napi_agent__ *agent, JSContext *context)
    : agent_{agent},
      context_{context}
{
}

JSValue napi_env__::wrap_external_data(void *data,
                                       node_api_basic_finalize finalize_cb,
                                       void *finalize_hint)
{
  JSValue obj = JS_NewObjectClass(context_, napi_external__::class_id());
  if (JS_IsException(obj))
    return obj;

  auto *hint = napi_external_backing_store_hint__::create(this, data, finalize_cb, finalize_hint);
  if (hint == nullptr)
  {
    JS_FreeValue(context_, obj);
    JS_ThrowOutOfMemory(context_);
    return JS_EXCEPTION;
  }

  JS_SetOpaque(obj, hint);
  return obj;
}

napi_status napi_quickjs_set_last_error(napi_env env,
                                        napi_status status,
                                        const char *message)
{
  if (env == nullptr)
    return status;
  return env->agent()->set_last_error(status, message);
}

napi_status napi_quickjs_clear_last_error(napi_env env)
{
  if (env == nullptr)
    return napi_ok;
  return env->agent()->clear_last_error();
}
