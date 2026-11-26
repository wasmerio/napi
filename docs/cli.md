# Standalone WASIX runner

`cargo-standalone.sh` builds against the engine and the WASIX runtime that
`Cargo.standalone.toml` names: the engine is `wasmerio/wasmer-runtime`, and
WASIX is the WASIX v2 runtime of `wasmerio/wasmer-next`, pinned to one
revision. `Cargo.standalone.lock` records the revision of the engine. Use the
repository's Rust toolchain and lockfile:

```sh
./cargo-standalone.sh build --locked --release --features cli --bin napi_wasmer
```

`wasmer-next` is a private repository. Cargo fetches it with the credentials
of the `git` command when told to:

```sh
CARGO_NET_GIT_FETCH_WITH_CLI=true ./cargo-standalone.sh build --locked --release --features cli --bin napi_wasmer
```

Inside a `wasmer-next` checkout (this repository at `lib/napi`), the script
builds with that workspace instead and needs neither.

The default CLI compiles WebAssembly with Cranelift. To build the LLVM variant,
install LLVM 23.1 and enable `cli-llvm` instead:

```sh
LLVM_SYS_231_PREFIX=/path/to/llvm-23 \
LLVM_SYS_231_STRICT_VERSIONING=1 \
./cargo-standalone.sh build --locked --release --features cli-llvm --bin napi_wasmer
```

The LLVM variant uses Wasmer's default LLVM configuration, including aggressive
optimization. The N-API value engine remains V8 in either native build.

An LLVM-enabled binary can select either backend at runtime. This allows
comparisons with exactly the same native executable:

```sh
WASMER_NAPI_COMPILER=cranelift ./target/release/napi_wasmer guest.wasm
WASMER_NAPI_COMPILER=llvm ./target/release/napi_wasmer guest.wasm
```

Without the variable, `cli-llvm` builds default to LLVM and ordinary `cli`
builds default to Cranelift. Unknown values fail explicitly; requesting LLVM
from a build without `cli-llvm` also fails.

CLI module caches are separated into `cranelift` and `llvm` subdirectories
under `WASMER_NAPI_CACHE_DIR` (by default, the temporary directory's
`wasmer-napi-cache`). Use a fresh cache root when comparing different Wasmer
source revisions, and warm each guest before measuring steady-state throughput.

## How a guest runs

The runner starts a WASIX v2 machine and registers N-API with it as an
instantiation hook (`wasmer_napi::NapiHook`), next to the hook for the
WebAssembly C API imports. The machine runs WASIX v1 guests through its
compatibility layer, so the guests the runner ran before still run.

The machine compiles a guest after preparing it: it adds preemption points,
routes atomic waits and `memory.grow` through itself and defers the start
function. The cache holds the prepared module, keyed by its bytes.

Each guest thread, the main one included, runs on a host thread with a 64 MiB
stack. V8 runs on that stack below the machine's own frames, which are several
megabytes deep in a debug build.

JavaScript calls into the guest from inside V8, where the host cannot suspend.
A guest function called that way runs to its end on the same host thread, which
sleeps while the function waits in a syscall. The runtime's other threads
finish the wait. `NapiHook`'s documentation has the consequences for embedders.

## WebAssembly in the guest

Guest JavaScript has V8's `WebAssembly` by default (`WasmPolicy::EnabledUnmetered`):
wasm memories and compiled code are not charged to any N-API limit. Pass
`--restricted-webassembly` to take it away (`WasmPolicy::Restricted`). The
`WebAssembly` global is then absent from the guest's realms, and V8 refuses to
compile wasm in all of them. Options follow the module path:

```sh
./target/release/napi_wasmer guest.wasm --restricted-webassembly
```
