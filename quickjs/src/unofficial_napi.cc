#include "unofficial_napi.h"

#include "internal/napi_serdes.h"
#include "internal/napi_env.h"
#include "internal/napi_external.h"
#include "internal/napi_promises.h"
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
        JS_SetPromiseHook(rt, napi_promises__::promise_hook, env);

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
        if (!napi_util__::check_env(env))
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
        if (!napi_util__::check_env(env))
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
        if (!napi_util__::check_env(env))
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
        if (!napi_util__::check_env(env))
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
        if (!napi_util__::check_env(env))
            return napi_invalid_arg;
        JS_RunGC(napi_util__::runtime(env));
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
        if (!napi_util__::check_env(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_request_gc_for_testing(napi_env env)
    {
        return unofficial_napi_low_memory_notification(env);
    }

    napi_status NAPI_CDECL unofficial_napi_process_microtasks(napi_env env)
    {
        if (!napi_util__::check_env(env))
            return napi_invalid_arg;
        return napi_util__::run_pending_jobs(env);
    }

    napi_status NAPI_CDECL unofficial_napi_terminate_execution(napi_env env)
    {
        return napi_util__::check_env(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_cancel_terminate_execution(napi_env env)
    {
        return napi_util__::check_env(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_request_interrupt(
        napi_env env,
        unofficial_napi_interrupt_callback callback,
        void *data)
    {
        if (!napi_util__::check_env(env) || callback == nullptr)
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
        if (!napi_util__::check_env(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_enqueue_microtask(napi_env env, napi_value callback)
    {
        if (!napi_util__::check_env(env) || !napi_util__::is_callable(env, callback))
            return napi_invalid_arg;
        JSContext *ctx = napi_util__::context(env);
        JSValueConst argv[] = {callback->get_inner()};
        if (JS_EnqueueJob(ctx, napi_promises__::microtask_job, 1, argv) < 0)
            return JS_HasException(ctx) ? napi_pending_exception : napi_generic_failure;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_promise_reject_callback(napi_env env,
                                                                       napi_value callback)
    {
        if (!napi_util__::check_env(env))
            return napi_invalid_arg;
        napi_status status = env->promises().set_reject_callback(callback);
        if (status != napi_ok)
            return status;
        JS_SetHostPromiseRejectionTracker(
            napi_util__::runtime(env),
            env->promises().has_reject_callback() ? napi_promises__::rejection_tracker : nullptr,
            env);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_promise_hooks(napi_env env,
                                                             napi_value init,
                                                             napi_value before,
                                                             napi_value after,
                                                             napi_value resolve)
    {
        if (!napi_util__::check_env(env))
            return napi_invalid_arg;
        napi_status status = env->promises().set_hooks(init, before, after, resolve);
        if (status != napi_ok)
            return status;
        JS_SetPromiseHook(napi_util__::runtime(env), napi_promises__::promise_hook, env);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_fatal_error_callbacks(
        napi_env env,
        unofficial_napi_fatal_error_callback fatal_callback,
        unofficial_napi_oom_error_callback oom_callback)
    {
        (void)fatal_callback;
        (void)oom_callback;
        if (!napi_util__::check_env(env))
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
        if (!napi_util__::check_env(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_remove_near_heap_limit_callback(
        napi_env env,
        size_t heap_limit)
    {
        (void)heap_limit;
        if (!napi_util__::check_env(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_set_stack_limit(napi_env env, void *stack_limit)
    {
        (void)stack_limit;
        if (!napi_util__::check_env(env) || stack_limit == nullptr)
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_promise_details(napi_env env,
                                                               napi_value promise,
                                                               int32_t *state_out,
                                                               napi_value *result_out,
                                                               bool *has_result_out)
    {
        if (!napi_util__::check_env(env) || promise == nullptr || state_out == nullptr || has_result_out == nullptr)
            return napi_invalid_arg;
        JSContext *ctx = napi_util__::context(env);
        JSPromiseStateEnum state = JS_PromiseState(ctx, promise->get_inner());
        if (state == JS_PROMISE_NOT_A_PROMISE)
            return napi_invalid_arg;
        *state_out = static_cast<int32_t>(state);
        *has_result_out = state != JS_PROMISE_PENDING;
        JSValue result = JS_UNDEFINED;
        if (*has_result_out)
            result = JS_PromiseResult(ctx, promise->get_inner());
        if (result_out != nullptr)
            return napi_util__::wrap_owned(env, result, result_out);
        JS_FreeValue(ctx, result);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_error_source_positions(
        napi_env env,
        napi_value error,
        unofficial_napi_error_source_positions *out)
    {
        return napi_util__::check_env(env) ? env->contextify().get_error_source_positions(error, out) : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_preserve_error_source_message(
        napi_env env,
        napi_value error)
    {
        return napi_util__::check_env(env) ? env->contextify().preserve_error_source_message(error) : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_set_source_maps_enabled(
        napi_env env,
        bool enabled)
    {
        return napi_util__::check_env(env) ? env->contextify().set_source_maps_enabled(enabled) : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_set_get_source_map_error_source_callback(
        napi_env env,
        napi_value callback)
    {
        return napi_util__::check_env(env) ? env->contextify().set_get_source_map_error_source_callback(callback) : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_get_error_source_line_for_stderr(
        napi_env env,
        napi_value error,
        napi_value *result_out)
    {
        return napi_util__::check_env(env) ? env->contextify().get_error_source_line_for_stderr(error, result_out) : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_get_error_thrown_at(
        napi_env env,
        napi_value error,
        napi_value *result_out)
    {
        return napi_util__::check_env(env) ? env->contextify().get_error_thrown_at(error, result_out) : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_take_preserved_error_formatting(
        napi_env env,
        napi_value error,
        napi_value *source_line_out,
        napi_value *thrown_at_out)
    {
        return napi_util__::check_env(env) ? env->contextify().take_preserved_error_formatting(error, source_line_out, thrown_at_out) : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_mark_promise_as_handled(
        napi_env env,
        napi_value promise)
    {
        return (!napi_util__::check_env(env) || promise == nullptr) ? napi_invalid_arg : napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_proxy_details(napi_env env,
                                                             napi_value proxy,
                                                             napi_value *target_out,
                                                             napi_value *handler_out)
    {
        (void)proxy;
        if (!napi_util__::check_env(env) || target_out == nullptr || handler_out == nullptr)
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
        if (!napi_util__::check_env(env) || value == nullptr || entries_out == nullptr || is_key_value_out == nullptr)
            return napi_invalid_arg;
        *is_key_value_out = true;
        return napi_util__::create_empty_array(env, entries_out);
    }

    napi_status NAPI_CDECL unofficial_napi_get_call_sites(napi_env env,
                                                          uint32_t frames,
                                                          napi_value *callsites_out)
    {
        (void)frames;
        if (!napi_util__::check_env(env) || callsites_out == nullptr)
            return napi_invalid_arg;
        return napi_util__::create_empty_array(env, callsites_out);
    }

    napi_status NAPI_CDECL unofficial_napi_arraybuffer_view_has_buffer(napi_env env,
                                                                       napi_value value,
                                                                       bool *result_out)
    {
        if (!napi_util__::check_env(env) || value == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        JSValue buffer = JS_GetTypedArrayBuffer(napi_util__::context(env), value->get_inner(), nullptr, nullptr, nullptr);
        if (JS_IsException(buffer))
        {
            JSValue exc = JS_GetException(napi_util__::context(env));
            JS_FreeValue(napi_util__::context(env), exc);
            *result_out = false;
            return napi_ok;
        }
        *result_out = !JS_IsUndefined(buffer) && !JS_IsNull(buffer);
        JS_FreeValue(napi_util__::context(env), buffer);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_constructor_name(napi_env env,
                                                                napi_value value,
                                                                napi_value *name_out)
    {
        if (!napi_util__::check_env(env) || value == nullptr || name_out == nullptr)
            return napi_invalid_arg;
        JSValue name = napi_util__::get_constructor_name_value(env, value->get_inner());
        if (JS_IsException(name))
            return napi_pending_exception;
        return napi_util__::wrap_owned(env, name, name_out);
    }

    napi_status NAPI_CDECL unofficial_napi_get_own_non_index_properties(
        napi_env env,
        napi_value value,
        uint32_t filter_bits,
        napi_value *result_out)
    {
        if (!napi_util__::check_env(env) || value == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        JSContext *ctx = napi_util__::context(env);
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
        return napi_util__::wrap_owned(env, out, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_create_private_symbol(napi_env env,
                                                                 const char *utf8description,
                                                                 size_t length,
                                                                 napi_value *result_out)
    {
        if (!napi_util__::check_env(env) || result_out == nullptr)
            return napi_invalid_arg;
        const size_t description_length =
            utf8description == nullptr ? 0
            : length == NAPI_AUTO_LENGTH ? std::strlen(utf8description)
                                         : length;
        std::string description(utf8description == nullptr ? "" : utf8description,
                                description_length);
        JSValue symbol = JS_NewSymbol(napi_util__::context(env), description.c_str(), false);
        if (JS_IsException(symbol))
            return napi_pending_exception;
        return napi_util__::wrap_owned(env, symbol, result_out);
    }

    static napi_status QuickJSStructuredClone(napi_env env,
                                              napi_value value,
                                              napi_value transfer_list_or_null,
                                              napi_value *result_out)
    {
        if (!napi_util__::check_env(env) || value == nullptr || result_out == nullptr)
            return napi_invalid_arg;
        size_t size = 0;
        uint8_t *bytes = JS_WriteObject(napi_util__::context(env),
                                        &size,
                                        value->get_inner(),
                                        JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE);
        if (bytes == nullptr)
            return napi_generic_failure;
        JSValue cloned = JS_ReadObject(napi_util__::context(env),
                                       bytes,
                                       size,
                                       JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE);
        js_free(napi_util__::context(env), bytes);
        if (JS_IsException(cloned))
            return napi_pending_exception;

        if (transfer_list_or_null != nullptr)
        {
            JSValueConst transfer_list = transfer_list_or_null->get_inner();
            if (!JS_IsUndefined(transfer_list) && !JS_IsNull(transfer_list) &&
                JS_IsArray(transfer_list))
            {
                JSValue length_value = JS_GetPropertyStr(napi_util__::context(env), transfer_list, "length");
                uint32_t length = 0;
                if (!JS_IsException(length_value) &&
                    JS_ToUint32(napi_util__::context(env), &length, length_value) == 0)
                {
                    for (uint32_t i = 0; i < length; ++i)
                    {
                        JSValue item = JS_GetPropertyUint32(napi_util__::context(env), transfer_list, i);
                        if (!JS_IsException(item) && JS_IsArrayBuffer(item))
                            JS_DetachArrayBuffer(napi_util__::context(env), item);
                        JS_FreeValue(napi_util__::context(env), item);
                    }
                }
                JS_FreeValue(napi_util__::context(env), length_value);
            }
        }
        return napi_util__::wrap_owned(env, cloned, result_out);
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
        return napi_serdes__::serialize_value(env, value, payload_out);
    }

    napi_status NAPI_CDECL unofficial_napi_deserialize_value(
        napi_env env,
        void *payload,
        napi_value *result_out)
    {
        return napi_serdes__::deserialize_value(env, payload, result_out);
    }

    void NAPI_CDECL unofficial_napi_release_serialized_value(void *payload)
    {
        napi_serdes__::release_serialized_value(payload);
    }

    napi_status NAPI_CDECL unofficial_napi_get_process_memory_info(
        napi_env env,
        double *heap_total_out,
        double *heap_used_out,
        double *external_out,
        double *array_buffers_out)
    {
        if (!napi_util__::check_env(env) || heap_total_out == nullptr || heap_used_out == nullptr ||
            external_out == nullptr || array_buffers_out == nullptr)
            return napi_invalid_arg;

        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(napi_util__::runtime(env), &usage);
        *heap_total_out = static_cast<double>(usage.malloc_size);
        *heap_used_out = static_cast<double>(usage.memory_used_size);
        *external_out = static_cast<double>(usage.binary_object_size);
        *array_buffers_out = static_cast<double>(usage.binary_object_size);
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_hash_seed(napi_env env,
                                                         uint64_t *hash_seed_out)
    {
        if (!napi_util__::check_env(env) || hash_seed_out == nullptr)
            return napi_invalid_arg;
        *hash_seed_out = 1;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_heap_statistics(
        napi_env env,
        unofficial_napi_heap_statistics *stats_out)
    {
        if (!napi_util__::check_env(env) || stats_out == nullptr)
            return napi_invalid_arg;
        std::memset(stats_out, 0, sizeof(*stats_out));
        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(napi_util__::runtime(env), &usage);
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
        if (!napi_util__::check_env(env) || count_out == nullptr)
            return napi_invalid_arg;
        *count_out = 1;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_heap_space_statistics(
        napi_env env,
        uint32_t space_index,
        unofficial_napi_heap_space_statistics *stats_out)
    {
        if (!napi_util__::check_env(env) || stats_out == nullptr)
            return napi_invalid_arg;
        if (space_index != 0)
            return napi_invalid_arg;
        std::memset(stats_out, 0, sizeof(*stats_out));
        std::strncpy(stats_out->space_name, "quickjs", sizeof(stats_out->space_name) - 1);
        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(napi_util__::runtime(env), &usage);
        stats_out->space_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.malloc_size));
        stats_out->space_used_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.memory_used_size));
        stats_out->physical_space_size = stats_out->space_size;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_get_heap_code_statistics(
        napi_env env,
        unofficial_napi_heap_code_statistics *stats_out)
    {
        if (!napi_util__::check_env(env) || stats_out == nullptr)
            return napi_invalid_arg;
        std::memset(stats_out, 0, sizeof(*stats_out));
        JSMemoryUsage usage{};
        JS_ComputeMemoryUsage(napi_util__::runtime(env), &usage);
        stats_out->code_and_metadata_size = static_cast<uint64_t>(std::max<int64_t>(0, usage.js_func_code_size));
        stats_out->bytecode_and_metadata_size = stats_out->code_and_metadata_size;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_start_cpu_profile(
        napi_env env,
        unofficial_napi_cpu_profile_start_result *result_out,
        uint32_t *profile_id_out)
    {
        if (!napi_util__::check_env(env) || result_out == nullptr || profile_id_out == nullptr)
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
        if (!napi_util__::check_env(env) || found_out == nullptr || json_out == nullptr || json_len_out == nullptr)
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
        if (!napi_util__::check_env(env) || started_out == nullptr)
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
        if (!napi_util__::check_env(env) || found_out == nullptr || json_out == nullptr || json_len_out == nullptr)
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
        if (!napi_util__::check_env(env) || json_out == nullptr || json_len_out == nullptr)
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
        if (!napi_util__::check_env(env) || result_out == nullptr)
            return napi_invalid_arg;
        JSValueConst value = env->promises().continuation_preserved_embedder_data();
        if (JS_IsUndefined(value))
            return napi_util__::create_undefined(env, result_out);
        return napi_util__::wrap_dup(env, value, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_set_continuation_preserved_embedder_data(
        napi_env env,
        napi_value value)
    {
        if (!napi_util__::check_env(env) || value == nullptr)
            return napi_invalid_arg;
        env->promises().set_continuation_preserved_embedder_data(value->get_inner());
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_notify_datetime_configuration_change(napi_env env)
    {
        return napi_util__::check_env(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_create_serdes_binding(napi_env env,
                                                                 napi_value *result_out)
    {
        if (!napi_util__::check_env(env) || result_out == nullptr)
            return napi_invalid_arg;

        napi_value target = nullptr;
        if (napi_create_object(env, &target) != napi_ok || target == nullptr)
            return napi_generic_failure;

        napi_property_descriptor serializer_props[] = {
            {"writeHeader", nullptr, napi_serdes__::serializer_write_header, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"writeValue", nullptr, napi_serdes__::serializer_write_value, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"releaseBuffer", nullptr, napi_serdes__::serializer_release_buffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"transferArrayBuffer", nullptr, napi_serdes__::serializer_transfer_array_buffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"writeUint32", nullptr, napi_serdes__::serializer_write_uint32, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"writeUint64", nullptr, napi_serdes__::serializer_write_uint64, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"writeDouble", nullptr, napi_serdes__::serializer_write_double, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"writeRawBytes", nullptr, napi_serdes__::serializer_write_raw_bytes, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"_setTreatArrayBufferViewsAsHostObjects", nullptr, napi_serdes__::serializer_set_treat_array_buffer_views_as_host_objects, nullptr, nullptr, nullptr, napi_default_method, nullptr},
        };

        napi_value serializer_ctor = nullptr;
        if (napi_define_class(env,
                              "Serializer",
                              NAPI_AUTO_LENGTH,
                              napi_serdes__::serializer_new,
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
            {"readHeader", nullptr, napi_serdes__::deserializer_read_header, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"readValue", nullptr, napi_serdes__::deserializer_read_value, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"getWireFormatVersion", nullptr, napi_serdes__::deserializer_get_wire_format_version, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"transferArrayBuffer", nullptr, napi_serdes__::deserializer_transfer_array_buffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"readUint32", nullptr, napi_serdes__::deserializer_read_uint32, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"readUint64", nullptr, napi_serdes__::deserializer_read_uint64, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"readDouble", nullptr, napi_serdes__::deserializer_read_double, nullptr, nullptr, nullptr, napi_default_method, nullptr},
            {"_readRawBytes", nullptr, napi_serdes__::deserializer_read_raw_bytes, nullptr, nullptr, nullptr, napi_default_method, nullptr},
        };

        napi_value deserializer_ctor = nullptr;
        if (napi_define_class(env,
                              "Deserializer",
                              NAPI_AUTO_LENGTH,
                              napi_serdes__::deserializer_new,
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
        return napi_util__::check_env(env) ? env->contextify().make_context(sandbox_or_symbol,
                                                              name,
                                                              origin_or_undefined,
                                                              allow_code_gen_strings,
                                                              allow_code_gen_wasm,
                                                              own_microtask_queue,
                                                              host_defined_option_id,
                                                              result_out)
                             : napi_invalid_arg;
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
        return napi_util__::check_env(env) ? env->contextify().run_script(sandbox_or_null,
                                                            source,
                                                            filename,
                                                            line_offset,
                                                            column_offset,
                                                            timeout,
                                                            display_errors,
                                                            break_on_sigint,
                                                            break_on_first_line,
                                                            host_defined_option_id,
                                                            result_out)
                             : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_dispose_context(
        napi_env env,
        napi_value sandbox_or_context_global)
    {
        return napi_util__::check_env(env) ? env->contextify().dispose_context(sandbox_or_context_global) : napi_invalid_arg;
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
        return napi_util__::check_env(env) ? env->contextify().compile_function(code,
                                                                  filename,
                                                                  line_offset,
                                                                  column_offset,
                                                                  cached_data_or_undefined,
                                                                  produce_cached_data,
                                                                  parsing_context_or_undefined,
                                                                  context_extensions_or_undefined,
                                                                  params_or_undefined,
                                                                  host_defined_option_id,
                                                                  result_out)
                             : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_contextify_contains_module_syntax(
        napi_env env,
        napi_value code,
        napi_value filename,
        napi_value resource_name_or_undefined,
        bool cjs_var_in_scope,
        bool *result_out)
    {
        return napi_util__::check_env(env) ? env->contextify().contains_module_syntax(code,
                                                                        filename,
                                                                        resource_name_or_undefined,
                                                                        cjs_var_in_scope,
                                                                        result_out)
                             : napi_invalid_arg;
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
        return napi_util__::check_env(env) ? env->contextify().create_cached_data(code,
                                                                    filename,
                                                                    line_offset,
                                                                    column_offset,
                                                                    host_defined_option_id,
                                                                    cached_data_buffer_out)
                             : napi_invalid_arg;
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
        if (!napi_util__::check_env(env) || handle_out == nullptr)
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
        if (!napi_util__::check_env(env) || handle_out == nullptr)
            return napi_invalid_arg;
        *handle_out = nullptr;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_destroy(napi_env env, void *handle)
    {
        (void)handle;
        if (!napi_util__::check_env(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_get_module_requests(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!napi_util__::check_env(env) || result_out == nullptr)
            return napi_invalid_arg;
        return napi_util__::create_empty_array(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_link(
        napi_env env,
        void *handle,
        size_t count,
        void *const *linked_handles)
    {
        (void)count;
        (void)linked_handles;
        if (!napi_util__::check_env(env) || handle == nullptr)
            return napi_invalid_arg;
        return napi_generic_failure;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_instantiate(napi_env env, void *handle)
    {
        if (!napi_util__::check_env(env) || handle == nullptr)
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
        if (!napi_util__::check_env(env) || result_out == nullptr)
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
        if (!napi_util__::check_env(env) || result_out == nullptr)
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
        if (!napi_util__::check_env(env) || result_out == nullptr)
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
        if (!napi_util__::check_env(env) || status_out == nullptr)
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
        if (!napi_util__::check_env(env) || result_out == nullptr)
            return napi_invalid_arg;
        if (handle == nullptr)
            return napi_invalid_arg;
        return napi_util__::create_undefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_has_top_level_await(
        napi_env env,
        void *handle,
        bool *result_out)
    {
        if (!napi_util__::check_env(env) || result_out == nullptr)
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
        if (!napi_util__::check_env(env) || result_out == nullptr)
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
        if (!napi_util__::check_env(env) || settled_out == nullptr)
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
        return napi_util__::unsupported_if_valid_env(env);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_set_module_source_object(
        napi_env env,
        void *handle,
        napi_value source_object)
    {
        (void)handle;
        (void)source_object;
        return napi_util__::check_env(env) ? napi_ok : napi_invalid_arg;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_get_module_source_object(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!napi_util__::check_env(env) || result_out == nullptr)
            return napi_invalid_arg;
        return napi_util__::create_undefined(env, result_out);
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_create_cached_data(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!napi_util__::check_env(env) || result_out == nullptr)
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
        if (!napi_util__::check_env(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_set_initialize_import_meta_object_callback(
        napi_env env,
        napi_value callback)
    {
        (void)callback;
        if (!napi_util__::check_env(env))
            return napi_invalid_arg;
        return napi_ok;
    }

    napi_status NAPI_CDECL unofficial_napi_module_wrap_create_required_module_facade(
        napi_env env,
        void *handle,
        napi_value *result_out)
    {
        (void)handle;
        if (!napi_util__::check_env(env) || result_out == nullptr)
            return napi_invalid_arg;
        return napi_util__::create_undefined(env, result_out);
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
