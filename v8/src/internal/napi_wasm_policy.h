#ifndef NAPI_V8_WASM_POLICY_H_
#define NAPI_V8_WASM_POLICY_H_

#include <stdint.h>

#include <v8-version.h>
#include <v8.h>

// V8 before 15.2 validated "use asm" modules and compiled them with its wasm
// pipeline. That path never asked the isolate's AllowWasmCodeGenerationCallback,
// so a restricted isolate could still make V8 reserve wasm code space by
// running asm.js. V8 15.2 removed asm.js translation: asm.js runs as ordinary
// JavaScript under every policy, and the callback below sees every way to make
// wasm code. Refuse an older V8 rather than reopen that path.
static_assert(V8_MAJOR_VERSION > 15 ||
                  (V8_MAJOR_VERSION == 15 && V8_MINOR_VERSION >= 2),
              "the WebAssembly policy needs V8 15.2 or later, which no longer "
              "translates asm.js to wasm");

// The embedder's choice of V8 WebAssembly for an env, passed by the bridge to
// snapi_private_create_env. It is not an env-create option: that descriptor is
// also the guest's wasm32 wire ABI, and guests can neither set nor observe the
// policy. The values are WasmPolicy::bridge_code in src/ctx.rs.
enum class NapiWebAssemblyPolicy : uint32_t {
  // No context of the env's isolate has the WebAssembly global, and V8 refuses
  // to compile wasm in all of them.
  kRestricted = 0,
  // V8's WebAssembly as the engine ships it. Wasm memories and code come from
  // V8's page allocator, outside the guest heap, and no embedder limit charges
  // them. Realms follow their allow_code_gen_wasm option.
  kAllowUnmetered = 1,
};

// Decodes a policy received over the bridge. An unknown value is refused, so a
// caller built against a newer policy set fails instead of getting another
// policy.
inline bool NapiWebAssemblyPolicyFromRaw(uint32_t raw,
                                         NapiWebAssemblyPolicy* out) {
  switch (static_cast<NapiWebAssemblyPolicy>(raw)) {
    case NapiWebAssemblyPolicy::kRestricted:
    case NapiWebAssemblyPolicy::kAllowUnmetered:
      *out = static_cast<NapiWebAssemblyPolicy>(raw);
      return true;
  }
  return false;
}

// Deletes the WebAssembly namespace from `context`'s global. Runs no
// JavaScript: the property is a plain data property of a fresh global. False
// if the global still has one afterwards.
inline bool RemoveWebAssemblyGlobal(v8::Local<v8::Context> context) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::String> key =
      v8::String::NewFromUtf8Literal(isolate, "WebAssembly");
  v8::Local<v8::Object> global = context->Global();
  if (!global->Delete(context, key).FromMaybe(false)) return false;
  v8::Local<v8::Value> remaining;
  return global->Get(context, key).ToLocal(&remaining) &&
         remaining->IsUndefined();
}

#endif  // NAPI_V8_WASM_POLICY_H_
