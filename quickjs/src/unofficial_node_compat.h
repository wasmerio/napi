#ifndef NAPI_QUICKJS_UNOFFICIAL_NODE_COMPAT_H_
#define NAPI_QUICKJS_UNOFFICIAL_NODE_COMPAT_H_

#include "unofficial_napi.h"

#include <quickjs.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
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

    enum QuickjsModuleWrapStatus : int32_t
    {
        kQuickjsModuleUninstantiated = 0,
        kQuickjsModuleInstantiating = 1,
        kQuickjsModuleInstantiated = 2,
        kQuickjsModuleEvaluating = 3,
        kQuickjsModuleEvaluated = 4,
        kQuickjsModuleErrored = 5,
    };

    struct QuickjsModuleWrap
    {
        JSValue module = JS_UNDEFINED;
        JSValue namespace_value = JS_UNDEFINED;
        JSValue error = JS_UNDEFINED;
        QuickjsModuleWrapStatus status = kQuickjsModuleUninstantiated;
        bool has_top_level_await = false;
    };

    extern std::mutex g_mu;
    extern EmbedderHooksState g_embedder_hooks;
    extern std::unordered_map<napi_env, EnvState> g_env_states;

    bool CheckEnv(napi_env env);
    JSContext *Ctx(napi_env env);
    JSRuntime *Rt(napi_env env);
    EnvState &EnsureEnvState(napi_env env);
    void EnsureQuickjsGlobalCompat(JSContext *ctx);

    void QuickjsPromiseHook(JSContext *ctx,
                            JSPromiseHookType type,
                            JSValueConst promise,
                            JSValueConst parent_promise,
                            void *opaque);
    void QuickjsPromiseRejectionTracker(JSContext *ctx,
                                        JSValueConst promise,
                                        JSValueConst reason,
                                        bool is_handled,
                                        void *opaque);
    JSValue QuickjsMicrotaskJob(JSContext *ctx, int argc, JSValueConst *argv);
    char *QuickjsModuleNormalize(JSContext *ctx, const char *module_base_name, const char *module_name, void *opaque);
    JSModuleDef *QuickjsModuleLoader(JSContext *ctx, const char *module_name, void *opaque);

    napi_status DestroyEnvInstance(napi_env env);
    napi_status ReleaseEnvScope(void *scope_ptr);
    napi_status StoreOptionalFunction(napi_env env, napi_value callback, JSValue *target);
    napi_status RunPendingJobs(napi_env env);
    napi_status WrapOwned(napi_env env, JSValue value, napi_value *result);
    napi_status WrapDup(napi_env env, JSValueConst value, napi_value *result);
    napi_status CreateEmptyArray(napi_env env, napi_value *result);
    napi_status CreateUndefined(napi_env env, napi_value *result);
    napi_status UnsupportedIfValidEnv(napi_env env);

    bool IsCallable(napi_env env, napi_value value);
    JSValue GetConstructorNameValue(napi_env env, JSValueConst value);
    JSModuleDef *ModuleDefFromValue(JSValueConst value);
    int SetModuleImportMetaUrl(JSContext *ctx, JSValueConst module_value, const std::string &url);
    void StoreModuleError(napi_env env, QuickjsModuleWrap *module);
    std::string ToUtf8(napi_env env, napi_value value);
    std::string ToUtf8(JSContext *ctx, JSValueConst value);
    bool IsTruthyProperty(napi_env env, napi_value object, const char *name);
    void SetStringProperty(JSContext *ctx, JSValueConst object, const char *name, const std::string &value);
    void AnnotateContextifyCompileException(napi_env env,
                                            JSValueConst exception,
                                            const std::string &source,
                                            const std::string &resource_name,
                                            int32_t line_offset,
                                            int32_t column_offset);

    napi_value SerdesSerializerNew(napi_env env, napi_callback_info info);
    napi_value SerdesSerializerWriteHeader(napi_env env, napi_callback_info info);
    napi_value SerdesSerializerWriteValue(napi_env env, napi_callback_info info);
    napi_value SerdesSerializerReleaseBuffer(napi_env env, napi_callback_info info);
    napi_value SerdesSerializerTransferArrayBuffer(napi_env env, napi_callback_info info);
    napi_value SerdesSerializerWriteUint32(napi_env env, napi_callback_info info);
    napi_value SerdesSerializerWriteUint64(napi_env env, napi_callback_info info);
    napi_value SerdesSerializerWriteDouble(napi_env env, napi_callback_info info);
    napi_value SerdesSerializerWriteRawBytes(napi_env env, napi_callback_info info);
    napi_value SerdesSerializerSetTreatArrayBufferViewsAsHostObjects(napi_env env, napi_callback_info info);
    napi_value SerdesDeserializerNew(napi_env env, napi_callback_info info);
    napi_value SerdesDeserializerReadHeader(napi_env env, napi_callback_info info);
    napi_value SerdesDeserializerReadValue(napi_env env, napi_callback_info info);
    napi_value SerdesDeserializerGetWireFormatVersion(napi_env env, napi_callback_info info);
    napi_value SerdesDeserializerTransferArrayBuffer(napi_env env, napi_callback_info info);
    napi_value SerdesDeserializerReadUint32(napi_env env, napi_callback_info info);
    napi_value SerdesDeserializerReadUint64(napi_env env, napi_callback_info info);
    napi_value SerdesDeserializerReadDouble(napi_env env, napi_callback_info info);
    napi_value SerdesDeserializerReadRawBytes(napi_env env, napi_callback_info info);
}

#endif // NAPI_QUICKJS_UNOFFICIAL_NODE_COMPAT_H_
