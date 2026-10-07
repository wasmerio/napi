# Host JavaScript context regressions

Run the context tests directly, without compiling Wasmer or installing npm dependencies:

```sh
node --expose-gc --test tests/host-js/global-context.test.mjs
bun test tests/host-js/global-context.test.mjs
```

The tests extract the context implementation from the JavaScript embedded in
`src/snapi_js.rs`. They cover virtual global identity, lazy host accessor reads,
context-owned writes, Error constructor/prototype ownership, native error
compatibility, causes/subclasses, stack formatting and collection of released
contexts and their shared WebAssembly buffers. The shared-object registry and
module loader are outside this isolated test.

Custom `Error.prepareStackTrace` tests detect whether the engine honors the
hook. Bun 1.3.14 does not honor it in the same way as V8, so those two tests skip
while the ownership and collection tests still run.

The Wasm Rust test `snapi_js::tests` creates 64 real N-API callback trampolines,
releases their environment, checks that no unowned externref roots remain, and
invokes a saved callback after release. It must return `undefined` through the
liveness guard instead of entering the freed environment or throwing a
dropped-closure error. With a matching `wasm-bindgen-test-runner`, `rust-src`
and Acorn available to the runner, execute it with:

```sh
RUSTFLAGS='-C target-feature=+atomics,+bulk-memory,+mutable-globals' \
CARGO_TARGET_WASM32_UNKNOWN_UNKNOWN_RUNNER=wasm-bindgen-test-runner \
  ./cargo-standalone.sh +nightly test --locked \
  --target wasm32-unknown-unknown --features js --lib \
  -Z build-std=std,panic_abort snapi_js::tests
```

CI installs the runner version recorded in `Cargo.standalone.lock`. Its
temporary directory contains the Acorn dependency so wasm-bindgen's generated
host module can resolve the parser without adding npm dependencies to this
crate.
