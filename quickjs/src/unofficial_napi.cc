#include "unofficial_napi.h"

#include "compat/contextify.h"
#include "compat/microtasks.h"
#include "compat/quickjs_utilities.h"
#include "compat/serdes.h"
#include "internal/napi_env.h"
#include "internal/napi_external.h"
#include "internal/napi_util.h"
#include "internal/quickjs_trace.h"
#include "node_api.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

using namespace quickjs::detail;

namespace
{
    struct SerializedValue
    {
        size_t length = 0;
        uint8_t bytes[];
    };

    struct UnofficialEnvScope
    {
        JSRuntime *rt = nullptr;
        JSContext *ctx = nullptr;
        napi_env env = nullptr;
    };

    napi_status DestroyEnvInstance(napi_env env)
    {
        if (env == nullptr)
            return napi_invalid_arg;

        JSContext *ctx = env->context();
        JSRuntime *rt = JS_GetRuntime(ctx);
        JS_SetPromiseHook(rt, nullptr, nullptr);
        JS_SetHostPromiseRejectionTracker(rt, nullptr, nullptr);
        JS_SetContextOpaque(ctx, nullptr);
        delete env;
        return napi_ok;
    }

    napi_status ReleaseEnvScope(void *scope_ptr)
    {
        if (scope_ptr == nullptr)
            return napi_invalid_arg;

        auto *scope = static_cast<UnofficialEnvScope *>(scope_ptr);
        napi_status status = napi_ok;
        if (scope->env != nullptr)
        {
            status = DestroyEnvInstance(scope->env);
            scope->env = nullptr;
        }
        if (scope->ctx != nullptr)
        {
            JS_FreeContext(scope->ctx);
            scope->ctx = nullptr;
        }
        if (scope->rt != nullptr)
        {
            // JS_FreeRuntime(scope->rt);
            scope->rt = nullptr;
        }
        delete scope;
        return status;
    }
}

extern "C"
{

    napi_status NAPI_CDECL unofficial_napi_create_env_from_context(
        JSContext *context, int32_t module_api_version, napi_env *result)
    {
        if (result == nullptr || context == nullptr)
            return napi_invalid_arg;

        auto rt = JS_GetRuntime(context);
        if (0 != napi_external__::register_class(rt))
            return napi_generic_failure;

        auto env = new (std::nothrow) napi_env__(context, module_api_version);
        if (env == nullptr)
            return napi_generic_failure;
        if (env->root_scope() == nullptr)
        {
            delete env;
            return napi_generic_failure;
        }

        JS_SetContextOpaque(context, env);

        *result = env;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_create_env(int32_t module_api_version,
                                                      napi_env *env_out,
                                                      void **scope_out)
    {
        return unofficial_napi_create_env_with_options(module_api_version, nullptr, env_out, scope_out);
    }

    napi_status NAPI_CDECL unofficial_napi_create_env_with_options(
        int32_t module_api_version,
        const unofficial_napi_env_create_options *options,
        napi_env *env_out,
        void **scope_out)
    {
        if (env_out == nullptr || scope_out == nullptr)
            return napi_invalid_arg;

        auto rt = JS_NewRuntime();
        if (rt == nullptr)
            return napi_generic_failure;
        if (options != nullptr)
        {
            if (options->max_old_generation_size_in_bytes > 0)
                JS_SetMemoryLimit(rt, options->max_old_generation_size_in_bytes);
        }

        auto ctx = JS_NewContext(rt);
        if (ctx == nullptr)
        {
            // JS_FreeRuntime(rt);
            return napi_generic_failure;
        }

        auto scope = new (std::nothrow) UnofficialEnvScope{.rt = rt, .ctx = ctx};
        if (scope == nullptr)
        {
            JS_FreeContext(ctx);
            // JS_FreeRuntime(rt);
            return napi_generic_failure;
        }

        auto status = unofficial_napi_create_env_from_context(ctx, module_api_version, &scope->env);
        if (status != napi_ok || scope->env == nullptr)
        {
            delete scope;
            JS_FreeContext(ctx);
            // JS_FreeRuntime(rt);
            return (status == napi_ok) ? napi_generic_failure : status;
        }

        *scope_out = reinterpret_cast<void *>(scope);
        *env_out = scope->env;

        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_embedder_hooks(
        const unofficial_napi_embedder_hooks *hooks)
    {
        (void)hooks;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_edge_environment(napi_env env, void *environment)
    {
        (void)environment;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_env_cleanup_callback(
        napi_env env,
        unofficial_napi_env_cleanup_callback callback,
        void *data)
    {
        (void)callback;
        (void)data;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_env_destroy_callback(
        napi_env env,
        unofficial_napi_env_destroy_callback callback,
        void *data)
    {
        (void)callback;
        (void)data;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_context_token_callbacks(
        napi_env env,
        unofficial_napi_context_token_callback assign_callback,
        unofficial_napi_context_token_callback unassign_callback,
        void *data)
    {
        (void)assign_callback;
        (void)unassign_callback;
        (void)data;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_release_env(void *scope)
    {
        return ReleaseEnvScope(scope);
    }

    napi_status NAPI_CDECL unofficial_napi_release_env_with_loop(
        void *scope,
        struct uv_loop_s *loop)
    {
        (void)loop;
        return ReleaseEnvScope(scope);
    }

    napi_status NAPI_CDECL unofficial_napi_low_memory_notification(napi_env env)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        JS_RunGC(Rt(env));
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_flags_from_string(
        const char *flags,
        size_t length)
    {
        (void)flags;
        (void)length;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_prepare_stack_trace_callback(
        napi_env env,
        napi_value callback)
    {
        (void)callback;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_request_gc_for_testing(napi_env env)
    {
        return unofficial_napi_low_memory_notification(env);
    }

    napi_status NAPI_CDECL unofficial_napi_process_microtasks(napi_env env)
    {
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return RunPendingJobs(env);
    }

    napi_status NAPI_CDECL unofficial_napi_terminate_execution(napi_env env)
    {
        return CheckEnv(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_cancel_terminate_execution(napi_env env)
    {
        return CheckEnv(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_request_interrupt(
        napi_env env,
        unofficial_napi_interrupt_callback callback,
        void *data)
    {
        if (!CheckEnv(env) || callback == nullptr)
            return napi_invalid_arg;
        callback(env, data);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_enqueue_foreground_task_callback(
        napi_env env,
        unofficial_napi_enqueue_foreground_task_callback callback,
        void *target)
    {
        (void)callback;
        (void)target;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_enqueue_microtask(napi_env env, napi_value callback)
    {
        if (!CheckEnv(env) || !IsCallable(env, callback))
            return napi_invalid_arg;
        JSContext *ctx = Ctx(env);
        JSValueConst argv[] = {callback->get_inner()};
        if (JS_EnqueueJob(ctx, QuickjsMicrotaskJob, 1, argv) < 0)
            return JS_HasException(ctx) ? napi_pending_exception : napi_generic_failure;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_promise_reject_callback(napi_env env,
                                                                       napi_value callback)
    {
        (void)callback;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_promise_hooks(napi_env env,
                                                             napi_value init,
                                                             napi_value before,
                                                             napi_value after,
                                                             napi_value resolve)
    {
        (void)init;
        (void)before;
        (void)after;
        (void)resolve;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_fatal_error_callbacks(
        napi_env env,
        unofficial_napi_fatal_error_callback fatal_callback,
        unofficial_napi_oom_error_callback oom_callback)
    {
        (void)fatal_callback;
        (void)oom_callback;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_near_heap_limit_callback(
        napi_env env,
        unofficial_napi_near_heap_limit_callback callback,
        void *data)
    {
        (void)callback;
        (void)data;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_remove_near_heap_limit_callback(
        napi_env env,
        size_t heap_limit)
    {
        (void)heap_limit;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_stack_limit(napi_env env, void *stack_limit)
    {
        (void)stack_limit;
        if (!CheckEnv(env) || stack_limit == nullptr)
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_promise_details(napi_env env,
                                                               napi_value promise,
                                                               int32_t *state_out,
                                                               napi_value *result_out,
                                                               bool *has_result_out)
    {
        if (!CheckEnv(env) || promise == nullptr || state_out == nullptr || has_result_out == nullptr)
            return napi_invalid_arg;
        JSContext *ctx = Ctx(env);
        JSPromiseStateEnum state = JS_PromiseState(ctx, promise->get_inner());
        if (state == JS_PROMISE_NOT_A_PROMISE)
            return napi_invalid_arg;
        *state_out = static_cast<int32_t>(state);
        *has_result_out = state != JS_PROMISE_PENDING;
        JSValue result = JS_UNDEFINED;
        if (*has_result_out)
            result = JS_PromiseResult(ctx, promise->get_inner());
        if (result_out != nullptr)
            return WrapOwned(env, result, result_out);
        JS_FreeValue(ctx, result);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_error_source_positions(
        napi_env env,
        napi_value error,
        unofficial_napi_error_source_positions *out)
    {
        if (!CheckEnv(env) || error == nullptr || out == nullptr)
            return napi_invalid_arg;
        std::memset(out, 0, sizeof(*out));
        out->line_number = -1;
        out->start_column = -1;
        out->end_column = -1;
        napi_value empty = nullptr;
        napi_status status = napi_create_string_utf8(env, "", 0, &empty);
        if (status != napi_ok)
            return status;
        out->source_line = empty;
        out->script_resource_name = empty;
        return napi_ok;
    }

    // These source-map formatting APIs are V8-shaped: V8 can build a v8::Message
    // for an arbitrary caught Error and read script name, line, column, and source.
    // QuickJS has useful throw/compile metadata while handling the exception, but
    // no equivalent message object after JS catches and returns an Error. Keep the
    // symbols linkable and argument-validating, but do not emulate V8 formatting.
    napi_status NAPI_CDECL unofficial_napi_preserve_error_source_message(
        napi_env env,
        napi_value error)
    {
        // No-op: QuickJS does not expose a V8-style message for this caught Error.
        if (!CheckEnv(env) || error == nullptr)
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_source_maps_enabled(
        napi_env env,
        bool enabled)
    {
        // No-op: QuickJS cannot later reconstruct V8-style caught-Error source messages.
        (void)enabled;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_get_source_map_error_source_callback(
        napi_env env,
        napi_value callback)
    {
        // No-op: there is no reliable caught-Error location to pass to this callback.
        (void)callback;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_error_source_line_for_stderr(
        napi_env env,
        napi_value error,
        napi_value *result_out)
    {
        if (!CheckEnv(env) || error == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        JSValue value = JS_GetPropertyStr(Ctx(env), error->get_inner(), "node:arrowMessage");
        if (JS_IsException(value))
            return napi_pending_exception;
        if (JS_IsUndefined(value))
        {
            JS_FreeValue(Ctx(env), value);
            return CreateUndefined(env, result_out);
        }
        return WrapOwned(env, value, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_get_error_thrown_at(
        napi_env env,
        napi_value error,
        napi_value *result_out)
    {
        (void)error;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return CreateUndefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_take_preserved_error_formatting(
        napi_env env,
        napi_value error,
        napi_value *source_line_out,
        napi_value *thrown_at_out)
    {
        if (!CheckEnv(env) || error == nullptr || source_line_out == nullptr || thrown_at_out == nullptr)
            return napi_invalid_arg;
        napi_status status = unofficial_napi_get_error_source_line_for_stderr(env, error, source_line_out);
        if (status != napi_ok)
            return status;
        return CreateUndefined(env, thrown_at_out);
    }

    napi_status NAPI_CDECL unofficial_napi_mark_promise_as_handled(
        napi_env env,
        napi_value promise)
    {
        return (!CheckEnv(env) || promise == nullptr) ? napi_invalid_arg : napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_proxy_details(napi_env env,
                                                             napi_value proxy,
                                                             napi_value *target_out,
                                                             napi_value *handler_out)
    {
        (void)proxy;
        if (!CheckEnv(env) || target_out == nullptr || handler_out == nullptr)
            return napi_invalid_arg;
        *target_out = nullptr;
        *handler_out = nullptr;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_preview_entries(napi_env env,
                                                           napi_value value,
                                                           napi_value *entries_out,
                                                           bool *is_key_value_out)
    {
        if (!CheckEnv(env) || value == nullptr || entries_out == nullptr || is_key_value_out == nullptr)
            return napi_invalid_arg;
        *is_key_value_out = true;
        return CreateEmptyArray(env, entries_out);
    }

    napi_status NAPI_CDECL unofficial_napi_get_call_sites(napi_env env,
                                                          uint32_t frames,
                                                          napi_value *callsites_out)
    {
        (void)frames;
        if (!CheckEnv(env) || callsites_out == nullptr)
            return napi_invalid_arg;
        return CreateEmptyArray(env, callsites_out);
    }

    napi_status NAPI_CDECL unofficial_napi_arraybuffer_view_has_buffer(napi_env env,
                                                                       napi_value value,
                                                                       bool *result_out)
    {
        if (!CheckEnv(env) || value == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        JSValue buffer = JS_GetTypedArrayBuffer(Ctx(env), value->get_inner(), nullptr, nullptr, nullptr);
        if (JS_IsException(buffer))
        {
            JSValue exc = JS_GetException(Ctx(env));
            JS_FreeValue(Ctx(env), exc);
            *result_out = false;
            return napi_ok;
        }
        *result_out = !JS_IsUndefined(buffer) && !JS_IsNull(buffer);
        JS_FreeValue(Ctx(env), buffer);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_constructor_name(napi_env env,
                                                                napi_value value,
                                                                napi_value *name_out)
    {
        if (!CheckEnv(env) || value == nullptr || name_out == nullptr)
            return napi_invalid_arg;
        JSValue name = GetConstructorNameValue(env, value->get_inner());
        if (JS_IsException(name))
            return napi_pending_exception;
        return WrapOwned(env, name, name_out);
    }

    napi_status NAPI_CDECL unofficial_napi_get_own_non_index_properties(
        napi_env env,
        napi_value value,
        uint32_t filter_bits,
        napi_value *result_out)
    {
        if (!CheckEnv(env) || value == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        JSContext *ctx = Ctx(env);
        JSValue obj = value->get_inner();
        if (!JS_IsObject(obj))
            return napi_object_expected;

        auto is_array_index_atom = [&](JSAtom atom) -> bool
        {
            size_t len = 0;
            const char *name = JS_AtomToCStringLen(ctx, &len, atom);
            if (name == nullptr)
                return false;
            bool is_index = false;
            if (len > 0 && len <= 10)
            {
                uint64_t index = 0;
                is_index = true;
                for (size_t i = 0; i < len; ++i)
                {
                    if (name[i] < '0' || name[i] > '9')
                    {
                        is_index = false;
                        break;
                    }
                    if (i == 0 && len > 1 && name[i] == '0')
                    {
                        is_index = false;
                        break;
                    }
                    index = index * 10 + static_cast<uint64_t>(name[i] - '0');
                }
                if (is_index && index > 4294967294ULL)
                    is_index = false;
            }
            JS_FreeCString(ctx, name);
            return is_index;
        };

        int gpn_flags = napi_util__::key_filter_to_gpn(static_cast<napi_key_filter>(filter_bits));
        JSPropertyEnum *props = nullptr;
        uint32_t prop_count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, obj, gpn_flags) < 0)
            return napi_util__::return_pending_if_caught(env, "Exception while getting property names");

        JSValue out = JS_NewArray(ctx);
        uint32_t out_idx = 0;

        for (uint32_t i = 0; i < prop_count; ++i)
        {
            if (is_array_index_atom(props[i].atom))
                continue;

            if ((filter_bits & (napi_key_writable | napi_key_configurable)) != 0)
            {
                JSPropertyDescriptor desc;
                int has = JS_GetOwnProperty(ctx, &desc, obj, props[i].atom);
                if (has < 0)
                {
                    JS_FreePropertyEnum(ctx, props, prop_count);
                    JS_FreeValue(ctx, out);
                    return napi_util__::return_pending_if_caught(env, "Exception while filtering property names");
                }
                if (has == 0)
                    continue;

                bool include = true;
                if ((filter_bits & napi_key_writable) != 0 && (desc.flags & JS_PROP_WRITABLE) == 0)
                    include = false;
                if ((filter_bits & napi_key_configurable) != 0 && (desc.flags & JS_PROP_CONFIGURABLE) == 0)
                    include = false;

                JS_FreeValue(ctx, desc.value);
                JS_FreeValue(ctx, desc.getter);
                JS_FreeValue(ctx, desc.setter);

                if (!include)
                    continue;
            }

            JSValue key = JS_AtomToValue(ctx, props[i].atom);
            if (JS_IsException(key))
            {
                JS_FreePropertyEnum(ctx, props, prop_count);
                JS_FreeValue(ctx, out);
                return napi_util__::return_pending_if_caught(env, "Failed to convert property name");
            }
            JS_SetPropertyUint32(ctx, out, out_idx++, key);
        }

        JS_FreePropertyEnum(ctx, props, prop_count);
        return WrapOwned(env, out, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_create_private_symbol(napi_env env,
                                                                 const char *utf8description,
                                                                 size_t length,
                                                                 napi_value *result_out)
    {
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        const size_t description_length =
            utf8description == nullptr ? 0
            : length == NAPI_AUTO_LENGTH ? std::strlen(utf8description)
                                         : length;
        std::string description(utf8description == nullptr ? "" : utf8description,
                                description_length);
        JSValue symbol = JS_NewSymbol(Ctx(env), description.c_str(), false);
        if (JS_IsException(symbol))
            return napi_pending_exception;
        return WrapOwned(env, symbol, result_out);
    }

    static napi_status QuickJSStructuredClone(napi_env env,
                                              napi_value value,
                                              napi_value transfer_list_or_null,
                                              napi_value *result_out)
    {
        if (!CheckEnv(env) || value == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        size_t size = 0;
        uint8_t *bytes = JS_WriteObject(Ctx(env),
                                        &size,
                                        value->get_inner(),
                                        JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE);
        if (bytes == nullptr)
            return napi_generic_failure;
        JSValue cloned = JS_ReadObject(Ctx(env),
                                       bytes,
                                       size,
                                       JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE);
        js_free(Ctx(env), bytes);
        if (JS_IsException(cloned))
            return napi_pending_exception;

        if (transfer_list_or_null != nullptr)
        {
            JSValueConst transfer_list = transfer_list_or_null->get_inner();
            if (!JS_IsUndefined(transfer_list) && !JS_IsNull(transfer_list) &&
                JS_IsArray(transfer_list))
            {
                JSValue length_value = JS_GetPropertyStr(Ctx(env), transfer_list, "length");
                uint32_t length = 0;
                if (!JS_IsException(length_value) &&
                    JS_ToUint32(Ctx(env), &length, length_value) == 0)
                {
                    for (uint32_t i = 0; i < length; ++i)
                    {
                        JSValue item = JS_GetPropertyUint32(Ctx(env), transfer_list, i);
                        if (!JS_IsException(item) && JS_IsArrayBuffer(item))
                            JS_DetachArrayBuffer(Ctx(env), item);
                        JS_FreeValue(Ctx(env), item);
                    }
                }
                JS_FreeValue(Ctx(env), length_value);
            }
        }
        return WrapOwned(env, cloned, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_structured_clone(
        napi_env env,
        napi_value value,
        napi_value *result_out)
    {
        return QuickJSStructuredClone(env, value, nullptr, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_structured_clone_with_transfer(
        napi_env env,
        napi_value value,
        napi_value transfer_list_or_null,
        napi_value *result_out)
    {
        return QuickJSStructuredClone(env, value, transfer_list_or_null, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_serialize_value(
        napi_env env,
        napi_value value,
        void **payload_out)
    {
        if (!CheckEnv(env) || value == nullptr || payload_out == nullptr)
            return napi_invalid_arg;
        size_t size = 0;
        uint8_t *bytes = JS_WriteObject(Ctx(env),
                                        &size,
                                        value->get_inner(),
                                        JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE);
        if (bytes == nullptr)
            return napi_generic_failure;
        auto *payload = static_cast<SerializedValue *>(std::malloc(sizeof(SerializedValue) + size));
        if (payload == nullptr)
        {
            js_free(Ctx(env), bytes);
            return napi_generic_failure;
        }
        payload->length = size;
        if (size > 0)
            std::memcpy(payload->bytes, bytes, size);
        js_free(Ctx(env), bytes);
        *payload_out = payload;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_deserialize_value(
        napi_env env,
        void *payload,
        napi_value *result_out)
    {
        if (!CheckEnv(env) || payload == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        auto *serialized = static_cast<SerializedValue *>(payload);
        JSValue value = JS_ReadObject(Ctx(env),
                                      serialized->bytes,
                                      serialized->length,
                                      JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE);
        if (JS_IsException(value))
            return napi_pending_exception;
        return WrapOwned(env, value, result_out);
    }

    void NAPI_CDECL unofficial_napi_release_serialized_value(void *payload)
    {
        std::free(payload);
    }

    napi_status NAPI_CDECL unofficial_napi_get_process_memory_info(
        napi_env env,
        double *heap_total_out,
        double *heap_used_out,
        double *external_out,
        double *array_buffers_out)
    {
        if (!CheckEnv(env) || heap_total_out == nullptr || heap_used_out == nullptr ||
            external_out == nullptr || array_buffers_out == nullptr)
            return napi_invalid_arg;

        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(Rt(env), &usage);
        *heap_total_out = static_cast<double>(usage.malloc_size);
        *heap_used_out = static_cast<double>(usage.memory_used_size);
        *external_out = static_cast<double>(usage.binary_object_size);
        *array_buffers_out = static_cast<double>(usage.binary_object_size);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_hash_seed(napi_env env,
                                                         uint64_t *hash_seed_out)
    {
        if (!CheckEnv(env) || hash_seed_out == nullptr)
            return napi_invalid_arg;
        *hash_seed_out = 1;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_heap_statistics(
        napi_env env,
        unofficial_napi_heap_statistics *stats_out)
    {
        if (!CheckEnv(env) || stats_out == nullptr)
            return napi_invalid_arg;
        std::memset(stats_out, 0, sizeof(*stats_out));
        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(Rt(env), &usage);
        stats_out->total_heap_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.malloc_size));
        stats_out->used_heap_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.memory_used_size));
        stats_out->malloced_memory = static_cast<uint64_t>(std::max<int64_t>(0, usage.malloc_size));
        stats_out->peak_malloced_memory = stats_out->malloced_memory;
        stats_out->external_memory = static_cast<uint64_t>(std::max<int64_t>(0, usage.binary_object_size));
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_heap_space_count(
        napi_env env,
        uint32_t *count_out)
    {
        if (!CheckEnv(env) || count_out == nullptr)
            return napi_invalid_arg;
        *count_out = 1;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_heap_space_statistics(
        napi_env env,
        uint32_t space_index,
        unofficial_napi_heap_space_statistics *stats_out)
    {
        if (!CheckEnv(env) || stats_out == nullptr)
            return napi_invalid_arg;
        if (space_index != 0)
            return napi_invalid_arg;
        std::memset(stats_out, 0, sizeof(*stats_out));
        std::strncpy(stats_out->space_name, "quickjs", sizeof(stats_out->space_name) - 1);
        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(Rt(env), &usage);
        stats_out->space_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.malloc_size));
        stats_out->space_used_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.memory_used_size));
        stats_out->physical_space_size = stats_out->space_size;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_heap_code_statistics(
        napi_env env,
        unofficial_napi_heap_code_statistics *stats_out)
    {
        if (!CheckEnv(env) || stats_out == nullptr)
            return napi_invalid_arg;
        std::memset(stats_out, 0, sizeof(*stats_out));
        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(Rt(env), &usage);
        stats_out->code_and_metadata_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.js_func_code_size));
        stats_out->bytecode_and_metadata_size = stats_out->code_and_metadata_size;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_start_cpu_profile(
        napi_env env,
        unofficial_napi_cpu_profile_start_result *result_out,
        uint32_t *profile_id_out)
    {
        if (!CheckEnv(env) || result_out == nullptr || profile_id_out == nullptr)
            return napi_invalid_arg;
        *result_out = unofficial_napi_cpu_profile_start_ok;
        *profile_id_out = 1;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_stop_cpu_profile(
        napi_env env,
        uint32_t profile_id,
        bool *found_out,
        char **json_out,
        size_t *json_len_out)
    {
        (void)profile_id;
        if (!CheckEnv(env) || found_out == nullptr || json_out == nullptr || json_len_out == nullptr)
            return napi_invalid_arg;
        *found_out = false;
        *json_out = nullptr;
        *json_len_out = 0;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_start_heap_profile(
        napi_env env,
        bool *started_out)
    {
        if (!CheckEnv(env) || started_out == nullptr)
            return napi_invalid_arg;
        *started_out = false;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_stop_heap_profile(
        napi_env env,
        bool *found_out,
        char **json_out,
        size_t *json_len_out)
    {
        if (!CheckEnv(env) || found_out == nullptr || json_out == nullptr || json_len_out == nullptr)
            return napi_invalid_arg;
        *found_out = false;
        *json_out = nullptr;
        *json_len_out = 0;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_take_heap_snapshot(
        napi_env env,
        const unofficial_napi_heap_snapshot_options *options,
        char **json_out,
        size_t *json_len_out)
    {
        (void)options;
        if (!CheckEnv(env) || json_out == nullptr || json_len_out == nullptr)
            return napi_invalid_arg;
        *json_out = nullptr;
        *json_len_out = 0;
        return napi_generic_failure;
    }

    void NAPI_CDECL unofficial_napi_free_buffer(void *data)
    {
        std::free(data);
    }

    napi_status NAPI_CDECL unofficial_napi_get_continuation_preserved_embedder_data(
        napi_env env,
        napi_value *result_out)
    {
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return CreateUndefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_set_continuation_preserved_embedder_data(
        napi_env env,
        napi_value value)
    {
        (void)value;
        if (!CheckEnv(env) || value == nullptr)
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_notify_datetime_configuration_change(napi_env env)
    {
        return CheckEnv(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_create_serdes_binding(napi_env env,
                                                                 napi_value *result_out)
    {
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;

        napi_value target = nullptr;
        if (napi_create_object(env, &target) != napi_ok || target == nullptr)
            return napi_generic_failure;

        napi_property_descriptor serializer_props[] = {
            {"writeHeader", nullptr, SerdesSerializerWriteHeader, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"writeValue", nullptr, SerdesSerializerWriteValue, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"releaseBuffer", nullptr, SerdesSerializerReleaseBuffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"transferArrayBuffer", nullptr, SerdesSerializerTransferArrayBuffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"writeUint32", nullptr, SerdesSerializerWriteUint32, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"writeUint64", nullptr, SerdesSerializerWriteUint64, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"writeDouble", nullptr, SerdesSerializerWriteDouble, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"writeRawBytes", nullptr, SerdesSerializerWriteRawBytes, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"_setTreatArrayBufferViewsAsHostObjects", nullptr, SerdesSerializerSetTreatArrayBufferViewsAsHostObjects, nullptr, nullptr, nullptr, napi_default_method, nullptr},
        };

        napi_value serializer_ctor = nullptr;
        if (napi_define_class(env,
                              "Serializer",
                              NAPI_AUTO_LENGTH,
                              SerdesSerializerNew,
                              nullptr,
                              sizeof(serializer_props) / sizeof(serializer_props[0]),
                              serializer_props,
                              &serializer_ctor) != napi_ok ||
            serializer_ctor == nullptr ||
            napi_set_named_property(env, target, "Serializer", serializer_ctor) != napi_ok)
        {
            return napi_generic_failure;
        }

        napi_property_descriptor deserializer_props[] = {
            {"readHeader", nullptr, SerdesDeserializerReadHeader, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"readValue", nullptr, SerdesDeserializerReadValue, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"getWireFormatVersion", nullptr, SerdesDeserializerGetWireFormatVersion, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"transferArrayBuffer", nullptr, SerdesDeserializerTransferArrayBuffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"readUint32", nullptr, SerdesDeserializerReadUint32, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"readUint64", nullptr, SerdesDeserializerReadUint64, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"readDouble", nullptr, SerdesDeserializerReadDouble, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"_readRawBytes", nullptr, SerdesDeserializerReadRawBytes, nullptr, nullptr, nullptr, napi_default_method, nullptr},
        };

        napi_value deserializer_ctor = nullptr;
        if (napi_define_class(env,
                              "Deserializer",
                              NAPI_AUTO_LENGTH,
                              SerdesDeserializerNew,
                              nullptr,
                              sizeof(deserializer_props) / sizeof(deserializer_props[0]),
                              deserializer_props,
                              &deserializer_ctor) != napi_ok ||
            deserializer_ctor == nullptr ||
            napi_set_named_property(env, target, "Deserializer", deserializer_ctor) != napi_ok)
        {
            return napi_generic_failure;
        }

        *result_out = target;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_make_context(
        napi_env env,
        napi_value sandbox_or_symbol,
        napi_value name,
        napi_value origin_or_undefined,
        bool allow_code_gen_strings,
        bool allow_code_gen_wasm,
        bool own_microtask_queue,
        napi_value host_defined_option_id,
        napi_value *result_out)
    {
        (void)name;
        (void)origin_or_undefined;
        (void)allow_code_gen_strings;
        (void)allow_code_gen_wasm;
        (void)own_microtask_queue;
        (void)host_defined_option_id;
        if (!CheckEnv(env) || sandbox_or_symbol == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        JSValue sandbox = sandbox_or_symbol->get_inner();
        if (!JS_IsObject(sandbox))
            return napi_invalid_arg;
        JS_SetPropertyStr(Ctx(env), sandbox, "__quickjs_contextified", JS_NewBool(Ctx(env), true));
        JS_SetPropertyStr(Ctx(env), sandbox, "globalThis", JS_DupValue(Ctx(env), sandbox));
        return WrapDup(env, sandbox, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_run_script(
        napi_env env,
        napi_value sandbox_or_null,
        napi_value source,
        napi_value filename,
        int32_t line_offset,
        int32_t column_offset,
        int64_t timeout,
        bool display_errors,
        bool break_on_sigint,
        bool break_on_first_line,
        napi_value host_defined_option_id,
        napi_value *result_out)
    {
        (void)line_offset;
        (void)column_offset;
        (void)timeout;
        (void)display_errors;
        (void)break_on_sigint;
        (void)break_on_first_line;
        (void)host_defined_option_id;
        if (!CheckEnv(env) || source == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        if (sandbox_or_null != nullptr && !JS_IsNull(sandbox_or_null->get_inner()) &&
            !IsTruthyProperty(env, sandbox_or_null, "__quickjs_contextified"))
            return napi_invalid_arg;

        std::string src = ToUtf8(env, source);
        std::string label = filename == nullptr ? "<contextify>" : ToUtf8(env, filename);
        JSValue result = JS_UNDEFINED;
        if (sandbox_or_null != nullptr && !JS_IsNull(sandbox_or_null->get_inner()))
        {
            const char *wrapper_source = "(function(__sandbox, __source) { with (__sandbox) { return eval(__source); } })";
            JSValue wrapper = JS_Eval(Ctx(env),
                                      wrapper_source,
                                      std::strlen(wrapper_source),
                                      "<contextify-wrapper>",
                                      JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(wrapper))
                return napi_pending_exception;
            JSValue argv[] = {sandbox_or_null->get_inner(), source->get_inner()};
            result = JS_Call(Ctx(env), wrapper, JS_UNDEFINED, 2, argv);
            JS_FreeValue(Ctx(env), wrapper);
        }
        else
        {
            result = JS_Eval(Ctx(env), src.c_str(), src.size(), label.c_str(), JS_EVAL_TYPE_GLOBAL);
        }
        if (JS_IsException(result))
            return napi_pending_exception;
        return WrapOwned(env, result, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_dispose_context(
        napi_env env,
        napi_value sandbox_or_context_global)
    {
        if (!CheckEnv(env) || sandbox_or_context_global == nullptr)
            return napi_invalid_arg;
        JSValue sandbox = sandbox_or_context_global->get_inner();
        if (!JS_IsObject(sandbox))
            return napi_invalid_arg;
        JS_SetPropertyStr(Ctx(env), sandbox, "__quickjs_contextified", JS_NewBool(Ctx(env), false));
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_compile_function(
        napi_env env,
        napi_value code,
        napi_value filename,
        int32_t line_offset,
        int32_t column_offset,
        napi_value cached_data_or_undefined,
        bool produce_cached_data,
        napi_value parsing_context_or_undefined,
        napi_value context_extensions_or_undefined,
        napi_value params_or_undefined,
        napi_value host_defined_option_id,
        napi_value *result_out)
    {
        (void)cached_data_or_undefined;
        (void)produce_cached_data;
        (void)parsing_context_or_undefined;
        (void)context_extensions_or_undefined;
        (void)host_defined_option_id;
        if (!CheckEnv(env) || code == nullptr || result_out == nullptr)
            return napi_invalid_arg;

        std::vector<JSValue> argv;
        if (params_or_undefined != nullptr && JS_IsArray(params_or_undefined->get_inner()))
        {
            uint32_t length = 0;
            JSValue len_val = JS_GetPropertyStr(Ctx(env), params_or_undefined->get_inner(), "length");
            JS_ToUint32(Ctx(env), &length, len_val);
            JS_FreeValue(Ctx(env), len_val);
            for (uint32_t i = 0; i < length; ++i)
            {
                JSValue param = JS_GetPropertyUint32(Ctx(env), params_or_undefined->get_inner(), i);
                argv.push_back(param);
            }
        }

        std::string source = ToUtf8(env, code);
        std::string source_url;
        JSValue code_arg = JS_DupValue(Ctx(env), code->get_inner());
        if (filename != nullptr && !JS_IsUndefined(filename->get_inner()) && !JS_IsNull(filename->get_inner()))
        {
            source_url = ToUtf8(env, filename);
            if (!source.empty() && !source_url.empty())
            {
                source += "\n//# sourceURL=";
                source += source_url;
                JSValue with_source_url =
                    JS_NewStringLen(Ctx(env), source.c_str(), source.size());
                if (!JS_IsException(with_source_url))
                {
                    JS_FreeValue(Ctx(env), code_arg);
                    code_arg = with_source_url;
                }
            }
        }
        argv.push_back(code_arg);

        JSValue global = JS_GetGlobalObject(Ctx(env));
        JSValue function_ctor = JS_GetPropertyStr(Ctx(env), global, "Function");
        JS_FreeValue(Ctx(env), global);
        JSValue fn = JS_CallConstructor(Ctx(env), function_ctor, static_cast<int>(argv.size()), argv.data());
        JS_FreeValue(Ctx(env), function_ctor);
        for (JSValue arg : argv)
            JS_FreeValue(Ctx(env), arg);
        if (JS_IsException(fn))
        {
            JSValue exc = JS_GetException(Ctx(env));
            AnnotateContextifyCompileException(env, exc, source, source_url, line_offset, column_offset);
            napi_util__::set_last_exception(env, exc);
            return napi_pending_exception;
        }

        JSValue out = JS_NewObject(Ctx(env));
        JS_SetPropertyStr(Ctx(env), out, "function", fn);
        if (!source_url.empty())
            SetStringProperty(Ctx(env), out, "sourceURL", source_url);
        JS_SetPropertyStr(Ctx(env), out, "sourceMapURL", JS_UNDEFINED);
        return WrapOwned(env, out, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_contains_module_syntax(
        napi_env env,
        napi_value code,
        napi_value filename,
        napi_value resource_name_or_undefined,
        bool cjs_var_in_scope,
        bool *result_out)
    {
        (void)filename;
        (void)resource_name_or_undefined;
        (void)cjs_var_in_scope;
        if (!CheckEnv(env) || code == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        std::string src = ToUtf8(env, code);
        *result_out = src.find("export ") != std::string::npos ||
                      src.find("import ") != std::string::npos ||
                      src.find("import(") != std::string::npos;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_create_cached_data(
        napi_env env,
        napi_value code,
        napi_value filename,
        int32_t line_offset,
        int32_t column_offset,
        napi_value host_defined_option_id,
        napi_value *cached_data_buffer_out)
    {
        (void)code;
        (void)filename;
        (void)line_offset;
        (void)column_offset;
        (void)host_defined_option_id;
        if (!CheckEnv(env) || cached_data_buffer_out == nullptr)
            return napi_invalid_arg;
        napi_value arraybuffer = nullptr;
        void *data = nullptr;
        napi_status status = napi_create_arraybuffer(env, 0, &data, &arraybuffer);
        if (status != napi_ok)
            return status;
        return napi_create_typedarray(env, napi_uint8_array, 0, arraybuffer, 0, cached_data_buffer_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_create_source_text(
        napi_env env,
        napi_value wrapper,
        napi_value url,
        napi_value context_or_undefined,
        napi_value source,
        int32_t line_offset,
        int32_t column_offset,
        napi_value cached_data_or_id,
        void **handle_out)
    {
        (void)wrapper;
        (void)url;
        (void)context_or_undefined;
        (void)source;
        (void)line_offset;
        (void)column_offset;
        (void)cached_data_or_id;
        if (!CheckEnv(env) || handle_out == nullptr)
            return napi_invalid_arg;
        *handle_out = nullptr;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_create_synthetic(
        napi_env env,
        napi_value wrapper,
        napi_value url,
        napi_value context_or_undefined,
        napi_value export_names,
        napi_value synthetic_eval_steps,
        void **handle_out)
    {
        (void)wrapper;
        (void)url;
        (void)context_or_undefined;
        (void)export_names;
        (void)synthetic_eval_steps;
        if (!CheckEnv(env) || handle_out == nullptr)
            return napi_invalid_arg;
        *handle_out = nullptr;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_destroy(napi_env env, void *handle)
    {
        (void)handle;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_get_module_requests(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return CreateEmptyArray(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_link(
        napi_env env,
        void *handle,
        size_t count,
        void *const *linked_handles)
    {
        (void)count;
        (void)linked_handles;
        if (!CheckEnv(env) || handle == nullptr)
            return napi_invalid_arg;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_instantiate(napi_env env, void *handle)
    {
        if (!CheckEnv(env) || handle == nullptr)
            return napi_invalid_arg;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_evaluate(
        napi_env env,
        void *handle,
        int64_t timeout,
        bool break_on_sigint,
        napi_value *result_out)
    {
        (void)timeout;
        (void)break_on_sigint;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        if (handle == nullptr)
            return napi_invalid_arg;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_evaluate_sync(
        napi_env env,
        void *handle,
        napi_value filename,
        napi_value parent_filename,
        napi_value *result_out)
    {
        (void)filename;
        (void)parent_filename;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        if (handle == nullptr)
            return napi_invalid_arg;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_get_namespace(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        if (handle == nullptr)
            return napi_invalid_arg;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_get_status(
        napi_env env,
        void *handle,
        int32_t *status_out)
    {
        if (!CheckEnv(env) || status_out == nullptr)
            return napi_invalid_arg;
        if (handle == nullptr)
            return napi_invalid_arg;
        *status_out = 0;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_get_error(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        if (handle == nullptr)
            return napi_invalid_arg;
        return CreateUndefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_has_top_level_await(
        napi_env env,
        void *handle,
        bool *result_out)
    {
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        if (handle == nullptr)
            return napi_invalid_arg;
        *result_out = false;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_has_async_graph(
        napi_env env,
        void *handle,
        bool *result_out)
    {
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        if (handle == nullptr)
            return napi_invalid_arg;
        *result_out = false;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_check_unsettled_top_level_await(
        napi_env env,
        napi_value module_wrap,
        bool warnings,
        bool *settled_out)
    {
        (void)module_wrap;
        (void)warnings;
        if (!CheckEnv(env) || settled_out == nullptr)
            return napi_invalid_arg;
        *settled_out = true;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_set_export(
        napi_env env,
        void *handle,
        napi_value export_name,
        napi_value export_value)
    {
        (void)handle;
        (void)export_name;
        (void)export_value;
        return UnsupportedIfValidEnv(env);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_set_module_source_object(
        napi_env env,
        void *handle,
        napi_value source_object)
    {
        (void)handle;
        (void)source_object;
        return CheckEnv(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_get_module_source_object(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return CreateUndefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_create_cached_data(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        napi_value arraybuffer = nullptr;
        void *data = nullptr;
        napi_status status = napi_create_arraybuffer(env, 0, &data, &arraybuffer);
        if (status != napi_ok)
            return status;
        return napi_create_typedarray(env, napi_uint8_array, 0, arraybuffer, 0, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_set_import_module_dynamically_callback(
        napi_env env,
        napi_value callback)
    {
        (void)callback;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_set_initialize_import_meta_object_callback(
        napi_env env,
        napi_value callback)
    {
        (void)callback;
        if (!CheckEnv(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_create_required_module_facade(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!CheckEnv(env) || result_out == nullptr)
            return napi_invalid_arg;
        return CreateUndefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_destroy_env_instance_for_testing(napi_env env)
    {
        if (env == nullptr)
            return napi_invalid_arg;
        JSContext *ctx = env->context();
        napi_status status = DestroyEnvInstance(env);
        JS_FreeContext(ctx);
        return status;
    }

} // extern "C"
