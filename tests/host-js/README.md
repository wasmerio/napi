# Host JavaScript context regressions

Run the context tests directly, without compiling Wasmer or installing npm dependencies:

```sh
node --expose-gc --test tests/host-js/global-context.test.mjs tests/host-js/finalizer-queue.test.mjs tests/host-js/native-realm.test.mjs
bun test tests/host-js/global-context.test.mjs tests/host-js/finalizer-queue.test.mjs tests/host-js/native-realm.test.mjs
```

The tests extract the context implementation from the JavaScript embedded in
`src/snapi_js.rs`. They cover virtual global identity, lazy host accessor reads,
context-owned writes, Error constructor/prototype ownership, native error
compatibility, causes/subclasses, stack formatting and collection of released
contexts and their shared WebAssembly buffers. Retained callback wrappers must
become inert on release and allow collection of their contexts and dispatch
captures. The shared-object registry and
module loader are outside this isolated test.

`finalizer-queue.test.mjs` extracts the finalizer queue the same way. It
checks that a drain takes only objects the host collected, oldest first, with
20,000 live ones registered; that queues are independent; that a retried id is
queued once; that removing an id cancels a queued or later collection; that
author code replacing `Map` or `FinalizationRegistry` methods afterwards
changes nothing; and, with the host's own GC, that a collected object is
reported and a live one is not.

`native-realm.test.mjs` loads the whole embedded JavaScript into a `vm`
realm, with and without a fake `__wasmerNapiNativeRealm` bridge. With it,
scripts run in the realm's global with their filename and offsets, global
lexical declarations persist, the realm serves one environment and refuses
secondary contexts, context extensions and modules, callbacks and drained guest
code are bracketed, checkpoints, release and the bracketing hooks go to the
bridge without letting its errors escape, and the bridge global is removed once
captured; a bridge that can't be removed is refused. Without it, contexts stay
isolated.

Custom `Error.prepareStackTrace` tests detect whether the engine honors the
hook. Bun 1.3.14 does not honor it in the same way as V8, so those three tests skip
while the ownership and collection tests still run.

Stack support is cached by the host capture function, allowing an embedding
SDK to install a stack shim later. Modern V8 reuses its native lazy stack
accessor; Node 20 and target-bound shim accessors exercise the fallback.

The Wasm Rust test `snapi_js::tests` creates 64 real N-API callback trampolines,
releases their environment, checks that no unowned externref roots remain, and
verifies the Rust closure captures are freed immediately without GC. It invokes
a saved callback after release, which must return `undefined` through its empty
dispatch slot instead of entering the freed environment or throwing a
dropped-closure error. The finalizer tests check that finalizers and
buffer finalizers are taken only once their object is reported collected, that
`napi_remove_wrap` cancels its finalizer even after that report, and that the
bookkeeping stays consistent. With a matching `wasm-bindgen-test-runner`, `rust-src`
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
