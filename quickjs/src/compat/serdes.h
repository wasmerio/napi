#ifndef NAPI_QUICKJS_COMPAT_SERDES_H_
#define NAPI_QUICKJS_COMPAT_SERDES_H_

#include "unofficial_napi.h"

namespace quickjs::detail
{
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

#endif // NAPI_QUICKJS_COMPAT_SERDES_H_
