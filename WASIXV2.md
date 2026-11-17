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

`TODO(wasixv2): NAPI-START` marks the temporary preconfiguration activation
rejection for direct unconfigured instantiation. The coordinated exec change
defers start until after this same configure hook; it needs no new NAPI trait
method. Do not fall back to raw memory.grow or fabricate ABI layout to enable
start. Validate the composed product revision once that change is published.

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

## Validation

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
