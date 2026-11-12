# Standalone WASIX runner

`cargo-standalone.sh` builds against the immutable Wasmer revision in
`Cargo.standalone.toml`. Use the repository's Rust toolchain and lockfile:

```sh
./cargo-standalone.sh build --locked --release --features cli --bin napi_wasmer
```

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
