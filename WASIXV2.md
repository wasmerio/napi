# Edge WASIX v2 integration

This branch starts from `edge-napi-webassembly-metered` at
`af20edeae76192e3cdd49c5966c34cc81d0192d6`. It composes the Wasmer-next
integration from `next` at `c17595ac738853fa3935096bf3ec28718c62deda` with
Edge's native provider. The merge-base for that integration is
`82b328f50ddaac78de388b67b4707f21864b9245`.

Native embedders register `NapiHook::new(context)` or
`NapiHook::from_hooks(builder.build_managed_imports(activator))` with the
v2 `MachineBuilder`. Hook configuration binds each actual Store/instance to
its own session, protected guest arena and kernel-backed refill. The original
runner owns deactivation and lease-release cleanup. The hook does not own a
Store. Ordinary low-level NapiSession entry points remain available.

Managed lane activation stays lazy. Its callback runs from the parked native
creation owner, outside borrowed Store access. The lane, policy, shared budget
and unpublished provider move together through creation and physical retirement.
Soft charges and provider-limit reporting retain Edge's budget pools; protected
lease accounting identifies already-funded engine memory to avoid double billing.
The native bridge retains Edge's frozen legacy imports and native policy/code
budget controls alongside the v2 frame/GC and callback error custody.

The v2 facade configures the attachment before invoking the deferred Wasm start
section. NAPI activation during start therefore uses the same protected heap and
refill as ordinary entry. No extra NAPI hook phase or opt-in is needed. Source
must pass through exec/facade loading or instrumentation. The CLI's permitted
instrumentation fallback uses `InstrumentError::fallback_source`, preserving
start deferral and its admission metadata in the cached compiler input.
Direct unconfigured instantiation still rejects NAPI activation; it bypasses this
facade lifecycle. Old compiled caches must be invalidated when adopting deferral.

The native engine no longer has the private v1 `do_wait_interruptible` method.
WASIX v2 execution rewrites guest atomic waits through the kernel's cancellable
futex path. Low-level raw engine waits retain the engine's ordinary semantics.

Build this submodule in the Wasmer-next workspace (one Git engine source and the
sealed local WLK crates). The NAPI CI profile is `cargo check -p wasmer-napi
--features cli`; native SDK uses `wasmer-sdk/napi-v8`. Browser builds use
`--target wasm32-unknown-unknown --no-default-features --features js,wasix`.
A check proves compilation, not native linking, runtime metering, or browser
execution. Platform/frame-provider behavior needs the retained native/JSPI and
browser suites in CI.

## Initial port validation

Checked in the isolated `wasmer-next-napi-check` clone of product
`95918f0bd0b5db60451a9adff1dfb76bf879656e`, whose lock pins runtime
`9cc2b035f6715cbf47b3efa9f481673f2949705d`:

- `cargo check -p wasmer-napi --features cli`
- `cargo check --offline --locked -p wasmer-napi --features cli --all-targets`
- `cargo check --locked -p wasmer-cli -p wasmer-c-api -p wasmer-sdk -p wasmer-napi --features wasmer-cli/cranelift,wasmer-sdk/napi-v8,wasmer-napi/cli`
- `cargo check --offline --locked -p wasmer-napi --lib --no-default-features --features js,wasix --target wasm32-unknown-unknown`

All passed (with warnings). Rust formatting and `git diff --check` passed.
The build downloaded V8 and compiled the native bridge. No executable linking,
runtime tests, browser execution or cross-platform execution was performed.
The standalone lock was generated and pinned to this same product/engine pair.
The later exec start-deferral work is not part of this validation snapshot.

## Deferred-start validation

Updated against wasmer-next main `a49d8ddce79278d944c3214e31241e8a9dbcadf7`
and its locked runtime `93ebbc98e695c0727f9107c444edfb166bf22ac6` in the
same isolated scratch checkout. The scratch product lock required only the
NAPI `parking_lot` dependency entry; no product source was changed. The standalone
lock now selects this product/engine pair.

The combined CLI/C API/SDK/NAPI check, native NAPI all-targets check, and wasm32
`js,wasix` library check above all passed again with `--locked`.

`cargo test --locked --offline -p wasmer-napi --features cli --test wasix_facade
facade_native_start_evaluates_js_with_protected_leases -- --exact --nocapture`
passed without an external stack override. It executes the Edge-style start
section reading `41+1` from imported guest memory, then a variant allocating a
32 MiB Uint8Array during start to require protected lease refill. Both verify
`done` output and retired session/isolate/memory accounting.

The first execution exhausted libtest's default host thread stack. The native
subprocess harness now explicitly uses a 16 MiB host test-thread stack; guest
stack limits are unchanged. The focused test passed after that harness fix.
Broader runtime suites and browser execution were not run.
