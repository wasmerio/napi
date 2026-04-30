set(QUICKJS_NG_QUICKJS_C "${quickjs_ng_sources_SOURCE_DIR}/quickjs.c")
file(READ "${QUICKJS_NG_QUICKJS_C}" QUICKJS_NG_QUICKJS_SOURCE)

set(EDGE_PROMISE_REACTION_HELPER [=[
typedef struct JSPromiseReactionData {
    struct list_head link; /* not used in promise_reaction_job */
    JSValue resolving_funcs[2];
    JSValue handler;
} JSPromiseReactionData;

static JSValueConst js_promise_function_promise(JSValueConst func)
{
    JSObject *p;
    JSPromiseFunctionData *s;

    if (!JS_IsObject(func))
        return JS_UNDEFINED;
    p = JS_VALUE_GET_OBJ(func);
    if (p->class_id != JS_CLASS_PROMISE_RESOLVE_FUNCTION &&
        p->class_id != JS_CLASS_PROMISE_REJECT_FUNCTION)
        return JS_UNDEFINED;
    s = p->u.promise_function_data;
    return s ? s->promise : JS_UNDEFINED;
}
]=])

if(NOT QUICKJS_NG_QUICKJS_SOURCE MATCHES "js_promise_function_promise")
  string(REPLACE
[=[typedef struct JSPromiseReactionData {
    struct list_head link; /* not used in promise_reaction_job */
    JSValue resolving_funcs[2];
    JSValue handler;
} JSPromiseReactionData;
]=]
"${EDGE_PROMISE_REACTION_HELPER}"
QUICKJS_NG_QUICKJS_SOURCE
"${QUICKJS_NG_QUICKJS_SOURCE}")

  string(REPLACE
[=[static JSValue promise_reaction_job(JSContext *ctx, int argc,
                                    JSValueConst *argv)
{
    JSValueConst handler, func;
    JSValue res, res2;
    JSValueConst arg;
    bool is_reject;

    assert(argc == 5);
    handler = argv[2];
    is_reject = JS_ToBool(ctx, argv[3]);
    arg = argv[4];

    promise_trace(ctx, "promise_reaction_job: is_reject=%d\n", is_reject);

    if (JS_IsUndefined(handler)) {
]=]
[=[static JSValue promise_reaction_job(JSContext *ctx, int argc,
                                    JSValueConst *argv)
{
    JSValueConst handler, func;
    JSValue res, res2;
    JSValueConst arg;
    JSValueConst hook_promise;
    JSRuntime *rt;
    bool is_reject;

    assert(argc == 5);
    handler = argv[2];
    is_reject = JS_ToBool(ctx, argv[3]);
    arg = argv[4];
    rt = ctx->rt;
    hook_promise = JS_UNDEFINED;

    promise_trace(ctx, "promise_reaction_job: is_reject=%d\n", is_reject);

    if (rt->promise_hook) {
        hook_promise = js_promise_function_promise(argv[0]);
        rt->promise_hook(ctx, JS_PROMISE_HOOK_BEFORE, hook_promise, JS_UNDEFINED,
                         rt->promise_hook_opaque);
    }
    if (JS_IsUndefined(handler)) {
]=]
QUICKJS_NG_QUICKJS_SOURCE
"${QUICKJS_NG_QUICKJS_SOURCE}")

  string(REPLACE
[=[    } else {
        res = JS_Call(ctx, handler, JS_UNDEFINED, 1, &arg);
    }
    is_reject = JS_IsException(res);
]=]
[=[    } else {
        res = JS_Call(ctx, handler, JS_UNDEFINED, 1, &arg);
    }
    if (rt->promise_hook) {
        rt->promise_hook(ctx, JS_PROMISE_HOOK_AFTER, hook_promise, JS_UNDEFINED,
                         rt->promise_hook_opaque);
    }
    is_reject = JS_IsException(res);
]=]
QUICKJS_NG_QUICKJS_SOURCE
"${QUICKJS_NG_QUICKJS_SOURCE}")

  file(WRITE "${QUICKJS_NG_QUICKJS_C}" "${QUICKJS_NG_QUICKJS_SOURCE}")
endif()
