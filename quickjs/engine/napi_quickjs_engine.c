/* Compile alongside the pinned engine internals without changing its source. */
#include <quickjs.c>

JS_EXTERN bool napi_quickjs_is_construct_call(JSContext *ctx)
{
    JSStackFrame *frame = ctx->rt->current_stack_frame;
    return frame != NULL && frame->is_constructor;
}

JS_EXTERN JSValue napi_quickjs_new_object_from_constructor(JSContext *ctx, JSValueConst constructor)
{
    return js_create_from_ctor(ctx, constructor, JS_CLASS_OBJECT);
}

JS_EXTERN JSValue napi_quickjs_function_data(JSValueConst function, int magic)
{
    JSCFunctionDataRecord *record = JS_GetOpaque(function, JS_CLASS_C_FUNCTION_DATA);
    return record != NULL && record->magic == magic && record->data_len != 0 ? record->data[0] : JS_UNDEFINED;
}
