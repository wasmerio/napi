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
must pass through exec/facade loading or `prepare_for_compile`. The CLI caches
the prepared bytes, which retain start deferral and its admission metadata
even when the preemption rewrite falls back.
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

Edge `napi_metering` passed all 18 tests on Edge `d633a38452` with
wasmer-next `6984236180`.
