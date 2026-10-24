#ifndef NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_
#define NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_

#include <v8.h>

#include "unofficial_napi.h"

struct napi_agent__;

bool NapiV8LookupForegroundTaskTarget(v8::Isolate* isolate,
                                      napi_env* env_out,
                                      unofficial_napi_enqueue_foreground_task_callback* callback_out);
void NapiV8ApplyPromiseHooksToContext(napi_env env, v8::Local<v8::Context> context);
// Applies the agent's promise hooks to every realm's context.
void NapiV8ApplyPromiseHooksToRealms(napi_agent__* agent);
void NapiV8TrackProviderPromise(v8::Isolate* isolate,
                                v8::Local<v8::Promise> promise);
bool NapiV8HasPendingProviderWork(napi_env env);

#endif  // NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_
