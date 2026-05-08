#include "compat/microtasks.h"

#include "compat/environment.h"
#include "compat/quickjs_utilities.h"

namespace quickjs::detail
{
    // Brief: ReplaceStoredValue belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void ReplaceStoredValue(napi_env env, JSValue *target, JSValueConst value)
    {
        JSContext *ctx = Ctx(env);
        FreeStoredValue(ctx, target);
        *target = JS_DupValue(ctx, value);
    }

    // Brief: JsIdentity belongs to the microtask and promise hook compatibility layer.
    // It extracts the stable QuickJS object pointer used to associate promise frames.
    // Inputs stay as QuickJS handles owned by the caller.
    // Non-object values have no identity and return null.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void *JsIdentity(JSValueConst value)
    {
        return JS_IsObject(value) ? JS_VALUE_GET_PTR(value) : nullptr;
    }

    // Brief: ClearPendingExceptionIfAny belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void ClearPendingExceptionIfAny(JSContext *ctx)
    {
        if (ctx == nullptr || !JS_HasException(ctx))
            return;
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
    }

    // Brief: CallPromiseHook belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void CallPromiseHook(napi_env env, JSValueConst hook, int argc, JSValueConst *argv)
    {
        if (!CheckEnv(env) || !JS_IsFunction(Ctx(env), hook))
            return;

        JSContext *ctx = Ctx(env);
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ret = JS_Call(ctx, hook, global, argc, argv);
        JS_FreeValue(ctx, global);
        if (JS_IsException(ret))
        {
            ClearPendingExceptionIfAny(ctx);
            return;
        }
        JS_FreeValue(ctx, ret);
    }

    // Brief: CapturePromiseContextFrame belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void CapturePromiseContextFrame(napi_env env, JSValueConst promise)
    {
        if (!CheckEnv(env))
            return;
        void *identity = JsIdentity(promise);
        if (identity == nullptr)
            return;

        auto &state = EnsureEnvState(env);
        if (JS_IsUndefined(state.continuation_preserved_embedder_data))
            return;
        JSContext *ctx = Ctx(env);
        JSValue frame = JS_DupValue(ctx, state.continuation_preserved_embedder_data);
        auto it = state.promise_context_frames.find(identity);
        if (it != state.promise_context_frames.end())
        {
            FreeStoredValue(ctx, &it->second);
            it->second = frame;
        }
        else
        {
            state.promise_context_frames.emplace(identity, frame);
        }
    }

    // Brief: EnterPromiseContextFrame belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void EnterPromiseContextFrame(napi_env env, JSValueConst promise)
    {
        if (!CheckEnv(env))
            return;

        auto &state = EnsureEnvState(env);
        JSContext *ctx = Ctx(env);
        state.promise_context_frame_stack.push_back(
            JS_DupValue(ctx, state.continuation_preserved_embedder_data));

        JSValueConst frame = JS_UNDEFINED;
        void *identity = JsIdentity(promise);
        if (identity != nullptr)
        {
            auto it = state.promise_context_frames.find(identity);
            if (it != state.promise_context_frames.end())
                frame = it->second;
        }
        ReplaceStoredValue(env, &state.continuation_preserved_embedder_data, frame);
    }

    // Brief: LeavePromiseContextFrame belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void LeavePromiseContextFrame(napi_env env, JSValueConst promise)
    {
        if (!CheckEnv(env))
            return;

        auto &state = EnsureEnvState(env);
        JSContext *ctx = Ctx(env);
        if (!state.promise_context_frame_stack.empty())
        {
            JSValue previous = state.promise_context_frame_stack.back();
            state.promise_context_frame_stack.pop_back();
            FreeStoredValue(ctx, &state.continuation_preserved_embedder_data);
            state.continuation_preserved_embedder_data = previous;
        }

        void *identity = JsIdentity(promise);
        if (identity != nullptr)
        {
            auto it = state.promise_context_frames.find(identity);
            if (it != state.promise_context_frames.end())
            {
                FreeStoredValue(ctx, &it->second);
                state.promise_context_frames.erase(it);
            }
        }
    }

    // Brief: GetStoredFunction belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    JSValue GetStoredFunction(napi_env env, JSValueConst value)
    {
        if (!CheckEnv(env) || !JS_IsFunction(Ctx(env), value))
            return JS_UNDEFINED;
        return JS_DupValue(Ctx(env), value);
    }

    // Brief: GetPromiseHook belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    JSValue GetPromiseHook(napi_env env, size_t index)
    {
        if (!CheckEnv(env) || index >= 4)
            return JS_UNDEFINED;
        auto &state = EnsureEnvState(env);
        return GetStoredFunction(env, state.promise_hooks[index]);
    }

    // Brief: GetPromiseRejectCallback belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    JSValue GetPromiseRejectCallback(napi_env env)
    {
        if (!CheckEnv(env))
            return JS_UNDEFINED;
        auto &state = EnsureEnvState(env);
        return GetStoredFunction(env, state.promise_reject_callback);
    }

    // Brief: QuickjsMicrotaskJob belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    JSValue QuickjsMicrotaskJob(JSContext *ctx, int argc, JSValueConst *argv)
    {
        if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
            return JS_UNDEFINED;
        return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, nullptr);
    }

    // Brief: QuickjsPromiseHook belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void QuickjsPromiseHook(JSContext *ctx,
                            JSPromiseHookType type,
                            JSValueConst promise,
                            JSValueConst parent_promise,
                            void *opaque)
    {
        napi_env env = static_cast<napi_env>(opaque);
        if (!CheckEnv(env) || Ctx(env) != ctx)
            return;

        if (type == JS_PROMISE_HOOK_INIT)
            CapturePromiseContextFrame(env, promise);
        else if (type == JS_PROMISE_HOOK_BEFORE)
            EnterPromiseContextFrame(env, promise);

        size_t hook_index = static_cast<size_t>(type);
        JSValue hook = GetPromiseHook(env, hook_index);
        if (JS_IsFunction(ctx, hook))
        {
            if (type == JS_PROMISE_HOOK_INIT)
            {
                JSValueConst argv[] = {promise, parent_promise};
                CallPromiseHook(env, hook, 2, argv);
            }
            else
            {
                JSValueConst argv[] = {promise};
                CallPromiseHook(env, hook, 1, argv);
            }
        }
        JS_FreeValue(ctx, hook);

        if (type == JS_PROMISE_HOOK_AFTER)
            LeavePromiseContextFrame(env, promise);
    }

    // Brief: QuickjsPromiseRejectionTracker belongs to the microtask and promise hook compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void QuickjsPromiseRejectionTracker(JSContext *ctx,
                                        JSValueConst promise,
                                        JSValueConst reason,
                                        bool is_handled,
                                        void *opaque)
    {
        napi_env env = static_cast<napi_env>(opaque);
        if (!CheckEnv(env) || Ctx(env) != ctx)
            return;

        JSValue callback = GetPromiseRejectCallback(env);
        if (!JS_IsFunction(ctx, callback))
        {
            JS_FreeValue(ctx, callback);
            return;
        }

        JSValue event_type = JS_NewInt32(ctx, is_handled ? 1 : 0);
        JSValueConst argv[] = {event_type, promise, reason};
        CallPromiseHook(env, callback, 3, argv);
        JS_FreeValue(ctx, event_type);
        JS_FreeValue(ctx, callback);
    }
}
