//! Shared plumbing for the integration tests that run a WASIX guest against a
//! real V8 isolate.

use std::{
    path::{Path, PathBuf},
    process::Command,
};

use wasmer_napi::{NapiCtx, cli::run_wasix_main_capture_stdio_with_ctx};

pub fn crate_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

pub fn wasix_test_dir() -> PathBuf {
    std::env::var_os("NAPI_WASIX_TEST_OUT_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            std::env::var_os("CARGO_TARGET_DIR")
                .map(PathBuf::from)
                .unwrap_or_else(|| crate_root().join("target"))
                .join("wasm32-wasix/release")
        })
}

/// Builds `tests/programs/<name>` for WASIX and returns the resulting module.
pub fn build_wasix_test(name: &str) -> PathBuf {
    let status = Command::new("./tests/build-test-wasix.sh")
        .arg(name)
        .current_dir(crate_root())
        .status()
        .expect("failed to execute tests/build-test-wasix.sh");
    assert!(status.success(), "failed to build WASIX test: {name}");
    wasix_test_dir().join(format!("{name}.wasm"))
}

pub fn run_guest(ctx: &NapiCtx, wasm: &Path) -> anyhow::Result<(i32, String, String)> {
    run_wasix_main_capture_stdio_with_ctx(ctx, wasm, &[], &[])
}
