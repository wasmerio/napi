#include "internal/napi_promises.h"

#include "internal/napi_env.h"
#include "internal/napi_value.h"

napi_promises__::napi_promises__(napi_env env, JSContext *context)
    : env_{env},
      context_{context},
      promise_reject_callback_{JS_UNDEFINED},
      promise_hooks_{JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED}
{
}

napi_promises__::~napi_promises__()
{
  teardown();
}

void napi_promises__::teardown()
{
  if (torn_down_)
    return;
  clear_stored_values();
  torn_down_ = true;
}

bool napi_promises__::is_active() const
{
  return !torn_down_ && env_ != nullptr && context_ != nullptr;
}

napi_status napi_promises__::set_optional_function(napi_value value, JSValue *target)
{
  if (target == nullptr)
    return napi_invalid_arg;

  if (value != nullptr &&
      !JS_IsUndefined(napi_quickjs_value_inner(env_, value)) &&
      !JS_IsNull(napi_quickjs_value_inner(env_, value)) &&
      !JS_IsFunction(context_, napi_quickjs_value_inner(env_, value)))
  {
    return napi_function_expected;
  }

  JSValue replacement = JS_UNDEFINED;
  if (value != nullptr &&
      !JS_IsUndefined(napi_quickjs_value_inner(env_, value)) &&
      !JS_IsNull(napi_quickjs_value_inner(env_, value)))
  {
    replacement = JS_DupValue(context_, napi_quickjs_value_inner(env_, value));
  }

  JS_FreeValue(context_, *target);
  *target = replacement;
  return napi_ok;
}

void napi_promises__::clear_stored_values()
{
  JS_FreeValue(context_, promise_reject_callback_);
  promise_reject_callback_ = JS_UNDEFINED;

  for (auto &hook : promise_hooks_)
  {
    JS_FreeValue(context_, hook);
    hook = JS_UNDEFINED;
  }
}

void napi_promises__::clear_pending_exception_if_any()
{
  if (context_ == nullptr || !JS_HasException(context_))
    return;
  JSValue exception = JS_GetException(context_);
  JS_FreeValue(context_, exception);
}

void napi_promises__::call_function(JSValueConst function, int argc, JSValueConst *argv)
{
  if (context_ == nullptr || !JS_IsFunction(context_, function))
    return;

  // A hook fires for code in any realm of the env, so it gets no receiver.
  JSValue ret = JS_Call(context_, function, JS_UNDEFINED, argc, argv);
  if (JS_IsException(ret))
  {
    clear_pending_exception_if_any();
    return;
  }
  JS_FreeValue(context_, ret);
}

napi_status napi_promises__::set_reject_callback(napi_value callback)
{
  return set_optional_function(callback, &promise_reject_callback_);
}

bool napi_promises__::has_reject_callback() const
{
  return JS_IsFunction(context_, promise_reject_callback_);
}

napi_status napi_promises__::set_hooks(napi_value init,
                                       napi_value before,
                                       napi_value after,
                                       napi_value resolve)
{
  napi_value callbacks[] = {init, before, after, resolve};
  JSValue replacements[4] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};

  for (size_t i = 0; i < 4; ++i)
  {
    napi_value callback = callbacks[i];
    if (callback == nullptr ||
        JS_IsUndefined(napi_quickjs_value_inner(env_, callback)) ||
        JS_IsNull(napi_quickjs_value_inner(env_, callback)))
    {
      continue;
    }
    if (!JS_IsFunction(context_, napi_quickjs_value_inner(env_, callback)))
    {
      for (JSValue replacement : replacements)
        JS_FreeValue(context_, replacement);
      return napi_function_expected;
    }
    replacements[i] = JS_DupValue(context_, napi_quickjs_value_inner(env_, callback));
  }

  for (size_t i = 0; i < 4; ++i)
  {
    JS_FreeValue(context_, promise_hooks_[i]);
    promise_hooks_[i] = replacements[i];
  }
  return napi_ok;
}

void napi_promises__::call_hook(size_t index, int argc, JSValueConst *argv)
{
  if (!is_active() || index >= promise_hooks_.size() ||
      !JS_IsFunction(context_, promise_hooks_[index]))
    return;

  // The hook may replace itself while it runs.
  JSValue hook = JS_DupValue(context_, promise_hooks_[index]);
  call_function(hook, argc, argv);
  JS_FreeValue(context_, hook);
}

void napi_promises__::call_reject_callback(JSValueConst promise,
                                           JSValueConst reason,
                                           bool is_handled)
{
  if (!is_active() || !has_reject_callback())
    return;

  JSValue callback = JS_DupValue(context_, promise_reject_callback_);
  JSValue event_type = JS_NewInt32(context_, is_handled ? 1 : 0);
  JSValueConst rejection_value = is_handled ? JS_UNDEFINED : reason;
  JSValueConst argv[] = {event_type, promise, rejection_value};
  call_function(callback, 3, argv);
  JS_FreeValue(context_, event_type);
  JS_FreeValue(context_, callback);
}

JSValue napi_promises__::microtask_job(JSContext *ctx, int argc, JSValueConst *argv)
{
  if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
    return JS_UNDEFINED;
  return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, nullptr);
}
