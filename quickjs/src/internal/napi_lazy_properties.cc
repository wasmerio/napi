#include "internal/napi_lazy_properties.h"

#include "internal/napi_env.h"
#include "internal/napi_util.h"
#include "internal/napi_value.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

namespace quickjs::detail
{
    namespace
    {
        class scoped_value final
        {
        public:
            scoped_value(JSContext *ctx, JSValue value) : ctx_(ctx), value_(value) {}
            ~scoped_value() { JS_FreeValue(ctx_, value_); }

            scoped_value(const scoped_value &) = delete;
            scoped_value &operator=(const scoped_value &) = delete;

            JSValueConst get() const { return value_; }

        private:
            JSContext *ctx_;
            JSValue value_;
        };

        // Atom references, rather than handles to the names array's elements,
        // keep already-validated names alive while later getters run JavaScript.
        class validated_names final
        {
        public:
            explicit validated_names(JSContext *ctx) : ctx_(ctx) {}
            ~validated_names()
            {
                for (JSAtom key : keys_)
                    JS_FreeAtom(ctx_, key);
            }

            validated_names(const validated_names &) = delete;
            validated_names &operator=(const validated_names &) = delete;

            void reserve(uint32_t count) { keys_.reserve(std::min<uint32_t>(count, 1024)); }

            void append(JSAtom key)
            {
                try
                {
                    keys_.push_back(key);
                }
                catch (const std::bad_alloc &)
                {
                    JS_FreeAtom(ctx_, key);
                    throw;
                }
            }

            const std::vector<JSAtom> &keys() const { return keys_; }

        private:
            JSContext *ctx_;
            std::vector<JSAtom> keys_;
        };

        // A name must not be an array index. Typed arrays also reserve every
        // canonical numeric string, including non-integral and invalid indices.
        // All conversions below receive primitives and cannot invoke user code.
        int validate_name(JSContext *ctx, JSValueConst key, bool typed_array)
        {
            if (JS_IsSymbol(key))
                return 1;
            if (!JS_IsString(key))
                return 0;

            size_t length = 0;
            const char *text = JS_ToCStringLen(ctx, &length, key);
            if (text == nullptr)
                return -1;

            bool array_index = length != 0 && length <= 10 &&
                               (length == 1 || text[0] != '0');
            uint64_t index = 0;
            for (size_t i = 0; array_index && i < length; ++i)
            {
                if (text[i] < '0' || text[i] > '9')
                    array_index = false;
                else
                    index = index * 10 + static_cast<uint64_t>(text[i] - '0');
            }
            array_index = array_index &&
                          index < static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
            const bool negative_zero = length == 2 && text[0] == '-' && text[1] == '0';
            JS_FreeCString(ctx, text);

            if (array_index || (typed_array && negative_zero))
                return 0;
            if (!typed_array)
                return 1;

            scoped_value number(ctx, JS_ToNumber(ctx, key));
            if (JS_IsException(number.get()))
                return -1;
            scoped_value canonical(ctx, JS_ToString(ctx, number.get()));
            if (JS_IsException(canonical.get()))
                return -1;
            return JS_IsStrictEqual(ctx, key, canonical.get()) ? 0 : 1;
        }
    }

    napi_status define_lazy_data_properties(napi_env env,
                                            napi_value object,
                                            napi_value names,
                                            napi_value initializer,
                                            napi_property_attributes attributes)
    {
        if (!napi_util__::check_env(env) || object == nullptr || names == nullptr ||
            initializer == nullptr)
            return napi_invalid_arg;
        if ((attributes & ~(napi_writable | napi_enumerable | napi_configurable)) != 0)
            return napi_invalid_arg;

        JSContext *ctx = env->context();
        napi_status status = napi_util__::fail_if_terminating(env, ctx);
        if (status != napi_ok)
            return status;

        JSValueConst raw_object = napi_quickjs_value_inner(env, object);
        JSValueConst raw_names = napi_quickjs_value_inner(env, names);
        JSValueConst raw_initializer = napi_quickjs_value_inner(env, initializer);
        if (!JS_IsObject(raw_object))
            return napi_object_expected;
        if (!JS_IsArray(raw_names))
            return napi_array_expected;
        if (!JS_IsFunction(ctx, raw_initializer))
            return napi_function_expected;
        // Check even an empty names array without running any proxy trap.
        if (JS_IsProxy(raw_object))
            return napi_generic_failure;

        const bool typed_array = JS_GetTypedArrayType(raw_object) >= 0;
        scoped_value length(ctx, JS_GetPropertyStr(ctx, raw_names, "length"));
        if (JS_IsException(length.get()))
            return napi_util__::return_pending_if_caught(env, ctx,
                                                        "Exception getting lazy property names");
        uint32_t count = 0;
        if (JS_ToUint32(ctx, &count, length.get()) < 0)
            return napi_util__::return_pending_if_caught(env, ctx,
                                                        "Exception getting lazy property names");

        try
        {
            validated_names keys(ctx);
            keys.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                scoped_value key(ctx, JS_GetPropertyUint32(ctx, raw_names, i));
                if (JS_IsException(key.get()))
                    return napi_util__::return_pending_if_caught(
                        env, ctx, "Exception getting a lazy property name");
                status = napi_util__::fail_if_terminating(env, ctx);
                if (status != napi_ok)
                    return status;

                const int valid = validate_name(ctx, key.get(), typed_array);
                if (valid < 0)
                    return napi_util__::return_pending_if_caught(
                        env, ctx, "Exception validating a lazy property name");
                if (valid == 0)
                    return napi_name_expected;

                JSAtom atom = JS_ValueToAtom(ctx, key.get());
                if (atom == JS_ATOM_NULL)
                    return napi_util__::return_pending_if_caught(
                        env, ctx, "Failed to retain a lazy property name");
                keys.append(atom);
            }

            int flags = 0;
            if ((attributes & napi_writable) != 0)
                flags |= JS_PROP_WRITABLE;
            if ((attributes & napi_enumerable) != 0)
                flags |= JS_PROP_ENUMERABLE;
            if ((attributes & napi_configurable) != 0)
                flags |= JS_PROP_CONFIGURABLE;
            for (JSAtom key : keys.keys())
            {
                const int defined = JS_DefineLazyDataProperty(ctx, raw_object, key,
                                                             raw_initializer, flags);
                if (defined < 0)
                    return napi_util__::return_pending_if_caught(
                        env, ctx, "Exception defining a lazy data property");
                if (defined == 0)
                    return napi_generic_failure;
            }
        }
        catch (const std::bad_alloc &)
        {
            JS_ThrowOutOfMemory(ctx);
            return napi_util__::return_pending_if_caught(env, ctx,
                                                        "Failed to allocate lazy property names");
        }
        return napi_quickjs_clear_last_error(env);
    }

    napi_status is_lazy_data_property(napi_env env,
                                      napi_value object,
                                      napi_value key,
                                      bool *result_out)
    {
        if (!napi_util__::check_env(env) || object == nullptr || key == nullptr ||
            result_out == nullptr)
            return napi_invalid_arg;
        *result_out = false;

        JSContext *ctx = env->context();
        napi_status status = napi_util__::fail_if_terminating(env, ctx);
        if (status != napi_ok)
            return status;
        JSValueConst raw_object = napi_quickjs_value_inner(env, object);
        JSValueConst raw_key = napi_quickjs_value_inner(env, key);
        if (!JS_IsObject(raw_object))
            return napi_object_expected;
        if (!JS_IsString(raw_key) && !JS_IsSymbol(raw_key))
            return napi_name_expected;
        if (JS_IsProxy(raw_object))
            return napi_quickjs_clear_last_error(env);

        JSAtom atom = JS_ValueToAtom(ctx, raw_key);
        if (atom == JS_ATOM_NULL)
            return napi_util__::return_pending_if_caught(env, ctx,
                                                        "Failed to convert a lazy property name");
        *result_out = JS_IsLazyDataProperty(ctx, raw_object, atom);
        JS_FreeAtom(ctx, atom);
        return napi_quickjs_clear_last_error(env);
    }
}
