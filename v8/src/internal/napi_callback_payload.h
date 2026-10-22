#ifndef NAPI_V8_CALLBACK_PAYLOAD_H_
#define NAPI_V8_CALLBACK_PAYLOAD_H_

#include "js_native_api.h"
#include <v8.h>

struct napi_callback_payload__ {
  napi_env env;
  napi_callback cb;
  void* data;
  // Creation context, selected while the callback runs. A strong handle: the
  // context stays alive as long as this function's payload does.
  v8::Global<v8::Context> context;
};

struct napi_accessor_payload__ {
  napi_env env;
  napi_callback getter_cb;
  napi_callback setter_cb;
  void* data;
};

#endif  // NAPI_V8_CALLBACK_PAYLOAD_H_
