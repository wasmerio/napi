#ifndef NAPI_V8_RESTRICTED_CONTEXT_H_
#define NAPI_V8_RESTRICTED_CONTEXT_H_

#include <v8.h>

// V8's WebAssembly.Memory allocations do not use the guest linear-memory
// accountant. Do not expose the WebAssembly constructor in guest contexts
// until those allocations can be charged to the owning workload. Apply this
// to every context, including vm/contextify contexts, before guest code runs.
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
