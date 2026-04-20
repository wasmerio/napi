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

struct napi_env__
{
    explicit napi_env__(JSContext *context, int32_t module_api_version);
    ~napi_env__();

    JSContext *context() const;

    JSContext *ctx;
    napi_extended_error_info last_error{};
    std::string last_error_message;
    JSValue last_exception;

    // TODO: Do we need these?
    // JSValue last_exception_message;
    // std::string last_exception_source_line;
    // std::string last_exception_thrown_at;
    
    int32_t module_api_version = 8;
};

napi_status napi_quickjs_set_last_error(napi_env env,
                                        napi_status status,
                                        const char *message);

napi_status napi_quickjs_clear_last_error(napi_env env);

napi_value napi_quickjs_wrap_value(napi_env env, JSValue value);
JSValue napi_quickjs_unwrap_value(napi_value value);

#endif // NAPI_QUICKJS_ENV_H_