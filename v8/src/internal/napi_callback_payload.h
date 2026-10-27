#ifndef NAPI_V8_CALLBACK_PAYLOAD_H_
#define NAPI_V8_CALLBACK_PAYLOAD_H_

#include "js_native_api.h"
#include "unofficial_napi.h"

struct napi_callback_payload__ {
  napi_env env;
  napi_callback cb;
  void* data;
  // The realm the function belongs to (null: the env's own). The function
  // keeps its creation context, and so the realm, alive.
  unofficial_napi_realm realm = nullptr;
};

struct napi_accessor_payload__ {
  napi_env env;
  napi_callback getter_cb;
  napi_callback setter_cb;
  void* data;
};

#endif  // NAPI_V8_CALLBACK_PAYLOAD_H_
