#ifndef NAPI_V8_RESTRICTED_CONTEXT_H_
#define NAPI_V8_RESTRICTED_CONTEXT_H_

#include <stdint.h>

#include <v8.h>

// Embedder-selected exposure of V8 WebAssembly in new environments. This is
// host configuration, passed by the bridge to snapi_private_create_env; it is
// not part of the public env-create options or of the guest ABI, so guests
// cannot change it.
enum class NapiWebAssemblyPolicy : uint32_t {
  // Remove WebAssembly from, and refuse wasm code generation in, every context
  // of an environment whose array buffers live in a guest heap. Environments
  // without a guest heap keep V8's default WebAssembly support.
  kRestrictGuestHeap = 0,
  // Keep WebAssembly in guest-heap environments. V8 allocates wasm memories
  // and code through its page allocator, not the guest heap, so nothing
  // charges them to the embedder's resource limits.
  kAllowUnmetered = 1,
};

// Decodes a policy received over the bridge. Unknown values are rejected so
// a caller built against a newer policy set fails instead of silently
// getting a different policy.
inline bool NapiWebAssemblyPolicyFromRaw(uint32_t raw,
                                         NapiWebAssemblyPolicy* out) {
  switch (static_cast<NapiWebAssemblyPolicy>(raw)) {
    case NapiWebAssemblyPolicy::kRestrictGuestHeap:
    case NapiWebAssemblyPolicy::kAllowUnmetered:
      *out = static_cast<NapiWebAssemblyPolicy>(raw);
      return true;
  }
  return false;
}

// Whether an environment's contexts must not expose or generate WebAssembly.
inline bool RestrictsUnmeteredWebAssembly(NapiWebAssemblyPolicy policy,
                                          bool has_guest_heap) {
  return has_guest_heap && policy != NapiWebAssemblyPolicy::kAllowUnmetered;
}

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
// accountant. Unless the embedder explicitly accepts unmetered WebAssembly
// (NapiWebAssemblyPolicy::kAllowUnmetered), do not expose the WebAssembly
// constructor in guest-heap-backed contexts.
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
