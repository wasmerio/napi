#include "compat/serdes.h"

#include "compat/quickjs_utilities.h"
#include "internal/napi_value.h"
#include "node_api.h"

#include <cstring>
#include <new>
#include <vector>

namespace quickjs::detail
{
    struct SerdesSerializer
    {
        std::vector<uint8_t> bytes;
    };

    struct SerdesDeserializer
    {
        std::vector<uint8_t> bytes;
        size_t offset = 0;
    };

    // Brief: ReadBytesFromArrayBufferLike belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool ReadBytesFromArrayBufferLike(napi_env env,
                                      napi_value value,
                                      std::vector<uint8_t> *bytes_out)
    {
        if (value == nullptr || bytes_out == nullptr)
            return false;

        JSContext *ctx = Ctx(env);
        JSValueConst input = value->get_inner();
        uint8_t *data = nullptr;
        size_t length = 0;

        if (JS_IsArrayBuffer(input))
        {
            data = JS_GetArrayBuffer(ctx, &length, input);
            if (data == nullptr && JS_HasException(ctx))
                return false;
            if (length == 0)
                bytes_out->clear();
            else
                bytes_out->assign(data, data + length);
            return true;
        }

        if (JS_GetTypedArrayType(input) >= 0)
        {
            size_t offset = 0;
            JSValue array_buffer = JS_GetTypedArrayBuffer(ctx, input, &offset, &length, nullptr);
            if (JS_IsException(array_buffer))
                return false;
            size_t array_buffer_length = 0;
            data = JS_GetArrayBuffer(ctx, &array_buffer_length, array_buffer);
            JS_FreeValue(ctx, array_buffer);
            if (data == nullptr && JS_HasException(ctx))
                return false;
            if (offset > array_buffer_length || length > array_buffer_length - offset)
                return false;
            if (length == 0)
                bytes_out->clear();
            else
                bytes_out->assign(data + offset, data + offset + length);
            return true;
        }

        if (JS_IsDataView(input))
        {
            JSValue buffer = JS_GetPropertyStr(ctx, input, "buffer");
            JSValue byte_offset = JS_GetPropertyStr(ctx, input, "byteOffset");
            JSValue byte_length = JS_GetPropertyStr(ctx, input, "byteLength");
            uint32_t offset = 0;
            uint32_t view_length = 0;
            bool ok = !JS_IsException(buffer) &&
                      JS_ToUint32(ctx, &offset, byte_offset) == 0 &&
                      JS_ToUint32(ctx, &view_length, byte_length) == 0;
            JS_FreeValue(ctx, byte_offset);
            JS_FreeValue(ctx, byte_length);
            if (!ok)
            {
                JS_FreeValue(ctx, buffer);
                return false;
            }
            size_t array_buffer_length = 0;
            data = JS_GetArrayBuffer(ctx, &array_buffer_length, buffer);
            JS_FreeValue(ctx, buffer);
            if (data == nullptr && JS_HasException(ctx))
                return false;
            if (offset > array_buffer_length || view_length > array_buffer_length - offset)
                return false;
            if (view_length == 0)
                bytes_out->clear();
            else
                bytes_out->assign(data + offset, data + offset + view_length);
            return true;
        }

        return false;
    }

    // Brief: AppendLittleEndian belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    template <typename T>
    void AppendLittleEndian(std::vector<uint8_t> *bytes, T value)
    {
        for (size_t i = 0; i < sizeof(T); ++i)
            bytes->push_back(static_cast<uint8_t>((static_cast<uint64_t>(value) >> (i * 8)) & 0xff));
    }

    // Brief: ReadLittleEndian belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    bool ReadLittleEndian(const std::vector<uint8_t> &bytes,
                          size_t *offset,
                          size_t width,
                          uint64_t *value_out)
    {
        if (offset == nullptr || value_out == nullptr || *offset > bytes.size() ||
            width > bytes.size() - *offset || width > sizeof(uint64_t))
            return false;
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i)
            value |= static_cast<uint64_t>(bytes[*offset + i]) << (i * 8);
        *offset += width;
        *value_out = value;
        return true;
    }

    // Brief: SerdesSerializerFinalize belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void SerdesSerializerFinalize(napi_env /*env*/, void *data, void * /*hint*/)
    {
        delete static_cast<SerdesSerializer *>(data);
    }

    // Brief: SerdesDeserializerFinalize belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void SerdesDeserializerFinalize(napi_env /*env*/, void *data, void * /*hint*/)
    {
        delete static_cast<SerdesDeserializer *>(data);
    }

    // Brief: GetSerdesSerializer belongs to the serdes compatibility layer.
    // It unwraps the native serializer state stored on a JavaScript instance.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Invalid instances return null so callers can throw the public serdes error.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    SerdesSerializer *GetSerdesSerializer(napi_env env, napi_value this_arg)
    {
        void *data = nullptr;
        if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr)
            return nullptr;
        return static_cast<SerdesSerializer *>(data);
    }

    // Brief: GetSerdesDeserializer belongs to the serdes compatibility layer.
    // It unwraps the native deserializer state stored on a JavaScript instance.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Invalid instances return null so callers can throw the public serdes error.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    SerdesDeserializer *GetSerdesDeserializer(napi_env env, napi_value this_arg)
    {
        void *data = nullptr;
        if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr)
            return nullptr;
        return static_cast<SerdesDeserializer *>(data);
    }

    // Brief: SerdesSerializerNew belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesSerializerNew(napi_env env, napi_callback_info info)
    {
        napi_value new_target = nullptr;
        if (napi_get_new_target(env, info, &new_target) != napi_ok || new_target == nullptr)
        {
            napi_throw_type_error(env,
                                  "ERR_CONSTRUCT_CALL_REQUIRED",
                                  "Class constructor Serializer cannot be invoked without 'new'");
            return nullptr;
        }

        napi_value this_arg = nullptr;
        size_t argc = 0;
        if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr)
            return nullptr;

        auto *serializer = new (std::nothrow) SerdesSerializer();
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Failed to allocate Serializer");
            return nullptr;
        }
        if (napi_wrap(env, this_arg, serializer, SerdesSerializerFinalize, nullptr, nullptr) != napi_ok)
        {
            delete serializer;
            napi_throw_error(env, nullptr, "Failed to initialize Serializer");
            return nullptr;
        }
        return nullptr;
    }

    // Brief: SerdesSerializerWriteHeader belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesSerializerWriteHeader(napi_env env, napi_callback_info /*info*/)
    {
        return UndefinedValue(env);
    }

    // Brief: SerdesSerializerWriteValue belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesSerializerWriteValue(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        napi_value argv[1] = {nullptr};
        size_t argc = 1;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok)
            return nullptr;

        SerdesSerializer *serializer = GetSerdesSerializer(env, this_arg);
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Serializer state");
            return nullptr;
        }

        napi_value value = argc >= 1 && argv[0] != nullptr ? argv[0] : UndefinedValue(env);
        size_t size = 0;
        uint8_t *bytes = JS_WriteObject(Ctx(env),
                                        &size,
                                        value->get_inner(),
                                        JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE);
        if (bytes == nullptr)
        {
            if (!JS_HasException(Ctx(env)))
                napi_throw_error(env, nullptr, "Value could not be serialized");
            return nullptr;
        }

        serializer->bytes.insert(serializer->bytes.end(), bytes, bytes + size);
        js_free(Ctx(env), bytes);

        napi_value result = nullptr;
        napi_get_boolean(env, true, &result);
        return result;
    }

    // Brief: SerdesSerializerReleaseBuffer belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesSerializerReleaseBuffer(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        size_t argc = 0;
        if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesSerializer *serializer = GetSerdesSerializer(env, this_arg);
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Serializer state");
            return nullptr;
        }

        napi_value buffer = nullptr;
        const void *data = serializer->bytes.empty() ? nullptr : serializer->bytes.data();
        if (napi_create_buffer_copy(env, serializer->bytes.size(), data, nullptr, &buffer) != napi_ok)
            return nullptr;
        serializer->bytes.clear();
        return buffer;
    }

    // Brief: SerdesSerializerTransferArrayBuffer belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesSerializerTransferArrayBuffer(napi_env env, napi_callback_info /*info*/)
    {
        return UndefinedValue(env);
    }

    // Brief: SerdesSerializerWriteUint32 belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesSerializerWriteUint32(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        napi_value argv[1] = {nullptr};
        size_t argc = 1;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesSerializer *serializer = GetSerdesSerializer(env, this_arg);
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Serializer state");
            return nullptr;
        }
        uint32_t value = 0;
        if (argc < 1 || napi_get_value_uint32(env, argv[0], &value) != napi_ok)
            return UndefinedValue(env);
        AppendLittleEndian<uint32_t>(&serializer->bytes, value);
        return UndefinedValue(env);
    }

    // Brief: SerdesSerializerWriteUint64 belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesSerializerWriteUint64(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        napi_value argv[2] = {nullptr, nullptr};
        size_t argc = 2;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesSerializer *serializer = GetSerdesSerializer(env, this_arg);
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Serializer state");
            return nullptr;
        }
        uint32_t hi = 0;
        uint32_t lo = 0;
        if (argc < 2 || napi_get_value_uint32(env, argv[0], &hi) != napi_ok ||
            napi_get_value_uint32(env, argv[1], &lo) != napi_ok)
            return UndefinedValue(env);
        AppendLittleEndian<uint64_t>(&serializer->bytes,
                                     (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo));
        return UndefinedValue(env);
    }

    // Brief: SerdesSerializerWriteDouble belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesSerializerWriteDouble(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        napi_value argv[1] = {nullptr};
        size_t argc = 1;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesSerializer *serializer = GetSerdesSerializer(env, this_arg);
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Serializer state");
            return nullptr;
        }
        double value = 0;
        if (argc < 1 || napi_get_value_double(env, argv[0], &value) != napi_ok)
            return UndefinedValue(env);
        const auto *raw = reinterpret_cast<const uint8_t *>(&value);
        serializer->bytes.insert(serializer->bytes.end(), raw, raw + sizeof(value));
        return UndefinedValue(env);
    }

    // Brief: SerdesSerializerWriteRawBytes belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesSerializerWriteRawBytes(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        napi_value argv[1] = {nullptr};
        size_t argc = 1;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesSerializer *serializer = GetSerdesSerializer(env, this_arg);
        if (serializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Serializer state");
            return nullptr;
        }
        std::vector<uint8_t> bytes;
        if (argc < 1 || !ReadBytesFromArrayBufferLike(env, argv[0], &bytes))
        {
            napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "source must be a TypedArray or a DataView");
            return nullptr;
        }
        serializer->bytes.insert(serializer->bytes.end(), bytes.begin(), bytes.end());
        return UndefinedValue(env);
    }

    // Brief: SerdesSerializerSetTreatArrayBufferViewsAsHostObjects belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesSerializerSetTreatArrayBufferViewsAsHostObjects(napi_env env,
                                                                     napi_callback_info /*info*/)
    {
        return UndefinedValue(env);
    }

    // Brief: SerdesDeserializerNew belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesDeserializerNew(napi_env env, napi_callback_info info)
    {
        napi_value new_target = nullptr;
        if (napi_get_new_target(env, info, &new_target) != napi_ok || new_target == nullptr)
        {
            napi_throw_type_error(env,
                                  "ERR_CONSTRUCT_CALL_REQUIRED",
                                  "Class constructor Deserializer cannot be invoked without 'new'");
            return nullptr;
        }

        napi_value this_arg = nullptr;
        napi_value argv[1] = {nullptr};
        size_t argc = 1;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr)
            return nullptr;
        if (argc < 1 || argv[0] == nullptr)
        {
            napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "buffer must be a TypedArray or a DataView");
            return nullptr;
        }

        auto *deserializer = new (std::nothrow) SerdesDeserializer();
        if (deserializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Failed to allocate Deserializer");
            return nullptr;
        }
        if (!ReadBytesFromArrayBufferLike(env, argv[0], &deserializer->bytes))
        {
            delete deserializer;
            napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "buffer must be a TypedArray or a DataView");
            return nullptr;
        }
        if (napi_wrap(env, this_arg, deserializer, SerdesDeserializerFinalize, nullptr, nullptr) != napi_ok)
        {
            delete deserializer;
            napi_throw_error(env, nullptr, "Failed to initialize Deserializer");
            return nullptr;
        }
        napi_set_named_property(env, this_arg, "buffer", argv[0]);
        return nullptr;
    }

    // Brief: SerdesDeserializerReadHeader belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesDeserializerReadHeader(napi_env env, napi_callback_info /*info*/)
    {
        napi_value result = nullptr;
        napi_get_boolean(env, true, &result);
        return result;
    }

    // Brief: SerdesDeserializerReadValue belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesDeserializerReadValue(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        size_t argc = 0;
        if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesDeserializer *deserializer = GetSerdesDeserializer(env, this_arg);
        if (deserializer == nullptr)
        {
            napi_throw_error(env, nullptr, "Invalid Deserializer state");
            return nullptr;
        }
        if (deserializer->offset > deserializer->bytes.size())
        {
            napi_throw_error(env, nullptr, "Deserializer offset is out of range");
            return nullptr;
        }

        JSValue value = JS_ReadObject(Ctx(env),
                                      deserializer->bytes.data() + deserializer->offset,
                                      deserializer->bytes.size() - deserializer->offset,
                                      JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE);
        if (JS_IsException(value))
            return nullptr;
        deserializer->offset = deserializer->bytes.size();

        napi_value result = nullptr;
        if (WrapOwned(env, value, &result) != napi_ok)
            return nullptr;
        return result;
    }

    // Brief: SerdesDeserializerGetWireFormatVersion belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesDeserializerGetWireFormatVersion(napi_env env, napi_callback_info /*info*/)
    {
        napi_value result = nullptr;
        napi_create_uint32(env, 0, &result);
        return result;
    }

    // Brief: SerdesDeserializerTransferArrayBuffer belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesDeserializerTransferArrayBuffer(napi_env env, napi_callback_info /*info*/)
    {
        return UndefinedValue(env);
    }

    // Brief: SerdesDeserializerReadUint32 belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesDeserializerReadUint32(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        size_t argc = 0;
        if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesDeserializer *deserializer = GetSerdesDeserializer(env, this_arg);
        uint64_t value = 0;
        if (deserializer == nullptr || !ReadLittleEndian(deserializer->bytes, &deserializer->offset, 4, &value))
        {
            napi_throw_error(env, nullptr, "ReadUint32() failed");
            return nullptr;
        }
        napi_value result = nullptr;
        napi_create_uint32(env, static_cast<uint32_t>(value), &result);
        return result;
    }

    // Brief: SerdesDeserializerReadUint64 belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesDeserializerReadUint64(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        size_t argc = 0;
        if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesDeserializer *deserializer = GetSerdesDeserializer(env, this_arg);
        uint64_t value = 0;
        if (deserializer == nullptr || !ReadLittleEndian(deserializer->bytes, &deserializer->offset, 8, &value))
        {
            napi_throw_error(env, nullptr, "ReadUint64() failed");
            return nullptr;
        }

        napi_value result = nullptr;
        napi_value hi = nullptr;
        napi_value lo = nullptr;
        napi_create_array_with_length(env, 2, &result);
        napi_create_uint32(env, static_cast<uint32_t>(value >> 32), &hi);
        napi_create_uint32(env, static_cast<uint32_t>(value), &lo);
        napi_set_element(env, result, 0, hi);
        napi_set_element(env, result, 1, lo);
        return result;
    }

    // Brief: SerdesDeserializerReadDouble belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesDeserializerReadDouble(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        size_t argc = 0;
        if (napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesDeserializer *deserializer = GetSerdesDeserializer(env, this_arg);
        if (deserializer == nullptr ||
            deserializer->offset > deserializer->bytes.size() ||
            sizeof(double) > deserializer->bytes.size() - deserializer->offset)
        {
            napi_throw_error(env, nullptr, "ReadDouble() failed");
            return nullptr;
        }
        double value = 0;
        std::memcpy(&value, deserializer->bytes.data() + deserializer->offset, sizeof(value));
        deserializer->offset += sizeof(value);
        napi_value result = nullptr;
        napi_create_double(env, value, &result);
        return result;
    }

    // Brief: SerdesDeserializerReadRawBytes belongs to the serdes compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    napi_value SerdesDeserializerReadRawBytes(napi_env env, napi_callback_info info)
    {
        napi_value this_arg = nullptr;
        napi_value argv[1] = {nullptr};
        size_t argc = 1;
        if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok)
            return nullptr;
        SerdesDeserializer *deserializer = GetSerdesDeserializer(env, this_arg);
        int64_t length = 0;
        if (deserializer == nullptr || argc < 1 || napi_get_value_int64(env, argv[0], &length) != napi_ok ||
            length < 0 || deserializer->offset > deserializer->bytes.size() ||
            static_cast<size_t>(length) > deserializer->bytes.size() - deserializer->offset)
        {
            napi_throw_error(env, nullptr, "ReadRawBytes() failed");
            return nullptr;
        }

        size_t offset = deserializer->offset;
        deserializer->offset += static_cast<size_t>(length);
        napi_value result = nullptr;
        napi_create_uint32(env, static_cast<uint32_t>(offset), &result);
        return result;
    }
}
