#ifndef NAPI_V8_RESTRICTED_CONTEXT_H_
#define NAPI_V8_RESTRICTED_CONTEXT_H_

#include <v8.h>

// Provider-created contexts use one embedder slot for their Wasm compilation
// policy. Unmarked contexts fail closed, including new realms that did not go
// through contextify. Slot 0 is reserved by V8's debugger.
constexpr int kNapiWasmCodeGenerationPolicyIndex = 1;

inline void SetWasmCodeGenerationAllowed(v8::Local<v8::Context> context,
                                         bool allowed) {
  context->SetEmbedderData(kNapiWasmCodeGenerationPolicyIndex,
                           v8::Boolean::New(context->GetIsolate(), allowed));
}

inline bool AllowWasmCodeGeneration(v8::Local<v8::Context> context,
                                    v8::Local<v8::String> /*source*/) {
  if (context->GetNumberOfEmbedderDataFields() <=
      kNapiWasmCodeGenerationPolicyIndex) {
    return false;
  }
  return context->GetEmbedderData(kNapiWasmCodeGenerationPolicyIndex)->IsTrue();
}

// V8's WebAssembly.Memory allocations do not use the guest linear-memory
// accountant. Do not expose the WebAssembly constructor in guest-heap-backed
// contexts until those allocations can be charged to the owning workload.
inline bool RemoveUnmeteredWebAssembly(v8::Local<v8::Context> context) {
  v8::Isolate* isolate = context->GetIsolate();
  v8::Local<v8::String> key =
      v8::String::NewFromUtf8Literal(isolate, "WebAssembly");
  v8::Local<v8::Object> global = context->Global();
  if (!global->Delete(context, key).FromMaybe(false)) return false;
  v8::Local<v8::Value> remaining;
  return global->Get(context, key).ToLocal(&remaining) &&
         remaining->IsUndefined();
}

#endif  // NAPI_V8_RESTRICTED_CONTEXT_H_
