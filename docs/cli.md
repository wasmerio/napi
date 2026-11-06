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

CLI module caches are separated into `cranelift` and `llvm` subdirectories
under `WASMER_NAPI_CACHE_DIR` (by default, the temporary directory's
`wasmer-napi-cache`). Use a fresh cache root when comparing different Wasmer
source revisions, and warm each guest before measuring steady-state throughput.
