#ifndef NAPI_QUICKJS_ENV_H_
#define NAPI_QUICKJS_ENV_H_

// TBD: relative path makes VCode less confused - probably need to fix CMakeFiles or VSCode settings
// I think VSCode is confused with includes in /node directory.
#include "../../../include/js_native_api.h"

#include <string>
#include <quickjs.h>

struct napi_value__
{
    explicit napi_value__(napi_env env, JSValue local);
    ~napi_value__();

    JSValue local() const;

    napi_env env;
    JSValue value;
};

struct napi_callback_info__
{
    napi_env env;
    JSValueConst this_val;
    JSValue new_target;
    int argc;
    JSValueConst *argv;
    void *data;
};

struct napi_deferred__ {
  napi_env env = nullptr;
  JSValue resolve = JS_UNDEFINED; // resolving_funcs[0]
  JSValue reject  = JS_UNDEFINED; // resolving_funcs[1]
};

struct napi_ref__
{
    explicit napi_ref__(napi_env env, JSValue local, uint32_t initial_ref_count);
    ~napi_ref__();

    napi_env env;
    JSValue value;

    bool can_be_weak;
    uint32_t ref_count;
};

struct napi_env__
{
    explicit napi_env__(JSContext *context, int32_t module_api_version);
    ~napi_env__();

    JSContext *context() const;

    JSContext *ctx;
    napi_extended_error_info last_error{};
    std::string last_error_message;
    JSValue last_exception;
    bool has_last_exception = false;

    // TODO: Do we need these?
    // JSValue last_exception_message;
    // std::string last_exception_source_line;
    // std::string last_exception_thrown_at;

    int32_t module_api_version = 8;

    void *instance_data = nullptr;
    napi_finalize instance_data_finalize_cb = nullptr;
    void *instance_data_finalize_hint = nullptr;
};

napi_status napi_quickjs_set_last_error(napi_env env,
                                        napi_status status,
                                        const char *message);

napi_status napi_quickjs_clear_last_error(napi_env env);

napi_value napi_quickjs_wrap_value(napi_env env, JSValue value);
JSValue napi_quickjs_unwrap_value(napi_value value);

int RegisterExternalClass(JSRuntime *rt);

#endif // NAPI_QUICKJS_ENV_H_
