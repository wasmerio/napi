#ifndef NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_
#define NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_

#include <v8.h>

#include "unofficial_napi.h"

bool NapiV8LookupForegroundTaskTarget(v8::Isolate* isolate,
                                      napi_env* env_out,
                                      unofficial_napi_enqueue_foreground_task_callback* callback_out);
bool NapiV8IsContextifyContext(napi_env env, v8::Local<v8::Context> context);
void NapiV8ApplyPromiseHooksToContext(napi_env env, v8::Local<v8::Context> context);
void NapiV8ApplyPromiseHooksToContextifyContexts(napi_env env);
class NapiV8ProviderPromiseReservation {
 public:
  explicit NapiV8ProviderPromiseReservation(v8::Isolate* isolate);
  ~NapiV8ProviderPromiseReservation();

  NapiV8ProviderPromiseReservation(const NapiV8ProviderPromiseReservation&) = delete;
  NapiV8ProviderPromiseReservation& operator=(const NapiV8ProviderPromiseReservation&) = delete;

  bool acquired() const { return acquired_; }

 private:
  v8::Isolate* isolate_;
  bool acquired_;
  bool reserved_;
};

bool NapiV8TrackProviderPromise(v8::Isolate* isolate,
                                v8::Local<v8::Promise> promise);
bool NapiV8HasPendingProviderWork(napi_env env);

#endif  // NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_
