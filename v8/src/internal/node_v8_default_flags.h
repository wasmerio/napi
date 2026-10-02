#ifndef NAPI_V8_NODE_V8_DEFAULT_FLAGS_H_
#define NAPI_V8_NODE_V8_DEFAULT_FLAGS_H_

// Keep these flags aligned with the corresponding shipping JS feature entries
// in deps/v8/src/flags/flag-definitions.h
// (JAVASCRIPT_SHIPPING_FEATURES_BASE). The embedded napi_v8 runtime currently
// enables these explicitly during bootstrap instead of plumbing them through
// edge's execArgv handling.
inline constexpr char kNodeJsExplicitResourceManagementFlag[] =
    "--js-explicit-resource-management";
inline constexpr char kNodeJsFloat16ArrayFlag[] = "--js-float16array";
// `--no-validate-asm` is not a shipping feature flag. V8 translates validated
// asm.js modules to WebAssembly and compiles them into wasm code space without
// consulting the embedder's wasm code-generation callback, which would bypass
// the provider's WebAssembly policy. Without validation asm.js runs as
// ordinary JavaScript, with the same results for correct programs.
inline constexpr char kNodeDefaultShippingV8Flags[] =
    "--js-explicit-resource-management "
    "--js-float16array "
    "--no-validate-asm";

#endif  // NAPI_V8_NODE_V8_DEFAULT_FLAGS_H_
