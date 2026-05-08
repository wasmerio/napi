#ifndef NAPI_QUICKJS_COMPAT_ENVIRONMENT_H_
#define NAPI_QUICKJS_COMPAT_ENVIRONMENT_H_

#include "unofficial_napi.h"

#include <quickjs.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace quickjs::detail
{
    constexpr size_t kDefaultEdgeQuickjsStackSize = 4 * 1024 * 1024;

    struct ErrorFormattingState
    {
        bool source_maps_enabled = false;
        JSValue get_source_map_error_source = JS_UNDEFINED;
        JSValue preserved_source_line = JS_UNDEFINED;
        JSValue preserved_thrown_at = JS_UNDEFINED;
    };

    struct EnvState
    {
        void *edge_environment = nullptr;
        unofficial_napi_env_cleanup_callback cleanup_callback = nullptr;
        void *cleanup_callback_data = nullptr;
        unofficial_napi_env_destroy_callback destroy_callback = nullptr;
        void *destroy_callback_data = nullptr;
        unofficial_napi_context_token_callback context_token_assign_callback = nullptr;
        unofficial_napi_context_token_callback context_token_unassign_callback = nullptr;
        void *context_token_callback_data = nullptr;
        unofficial_napi_enqueue_foreground_task_callback enqueue_foreground_task_callback = nullptr;
        void *enqueue_foreground_task_target = nullptr;
        unofficial_napi_fatal_error_callback fatal_error_callback = nullptr;
        unofficial_napi_oom_error_callback oom_error_callback = nullptr;
        unofficial_napi_near_heap_limit_callback near_heap_limit_callback = nullptr;
        void *near_heap_limit_callback_data = nullptr;
        void *stack_limit = nullptr;
        JSValue prepare_stack_trace_callback = JS_UNDEFINED;
        JSValue promise_reject_callback = JS_UNDEFINED;
        JSValue promise_hooks[4] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
        JSValue continuation_preserved_embedder_data = JS_UNDEFINED;
        std::unordered_map<void *, JSValue> promise_context_frames;
        std::vector<JSValue> promise_context_frame_stack;
        JSValue import_module_dynamically_callback = JS_UNDEFINED;
        JSValue initialize_import_meta_object_callback = JS_UNDEFINED;
        ErrorFormattingState error_formatting;
        uint64_t hash_seed = 1;
    };

    struct EmbedderHooksState
    {
        unofficial_napi_embedder_hooks hooks{};
    };

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

    extern std::mutex g_mu;
    extern EmbedderHooksState g_embedder_hooks;
    extern std::unordered_map<napi_env, EnvState> g_env_states;

    void FreeStoredValue(JSContext *ctx, JSValue *value);
    EnvState &EnsureEnvState(napi_env env);
    napi_status DestroyEnvInstance(napi_env env);
    napi_status ReleaseEnvScope(void *scope_ptr);
}

#endif // NAPI_QUICKJS_COMPAT_ENVIRONMENT_H_
