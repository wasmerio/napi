#ifndef NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_
#define NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_

#include <v8.h>

#include "napi_snapshot.h"
#include "unofficial_napi.h"

bool NapiV8LookupForegroundTaskTarget(v8::Isolate* isolate,
                                      napi_env* env_out,
                                      unofficial_napi_enqueue_foreground_task_callback* callback_out);
bool NapiV8IsContextifyContext(napi_env env, v8::Local<v8::Context> context);
bool NapiV8IsSnapshotTemplateContext(napi_env env, v8::Local<v8::Context> context);
void NapiV8ApplyPromiseHooksToContext(napi_env env, v8::Local<v8::Context> context);
void NapiV8ApplyPromiseHooksToContextifyContexts(napi_env env);
// Frees the records of collected realms, unassigning their context tokens.
void NapiV8ReleaseCollectedRealms(napi_env env);
// Unassigns the context token of every realm that still has one, also of
// realms the unassign hooks make.
void NapiV8UnassignRealmTokens(napi_env env);
// True while a realm of the env has a context token.
bool NapiV8HasRealmTokens(napi_env env);
void NapiV8TrackProviderPromise(v8::Isolate* isolate,
                                v8::Local<v8::Promise> promise);
bool NapiV8HasPendingProviderWork(napi_env env);
// The isolate's code generation callbacks: TrustedScript sources, and the
// contexts unofficial_napi_set_realm_code_generation restricts.
void NapiV8InstallCodeGenerationCallbacks(v8::Isolate* isolate);
// Whether a restricted context's check lets this compile (false without one):
// `kind` is "eval", "trusted-eval" (a TrustedScript's source) or "wasm".
bool NapiV8CheckCodeGeneration(v8::Local<v8::Context> context, const char* kind,
                               v8::Local<v8::String> source);
// The isolate's AllowWasmCodeGenerationCallback.
bool NapiV8AllowWasmCodeGeneration(v8::Local<v8::Context> context,
                                   v8::Local<v8::String> source);

#endif  // NAPI_V8_UNOFFICIAL_NAPI_BRIDGE_H_
