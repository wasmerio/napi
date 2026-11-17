//! Shared plumbing for the integration tests that run a WASIX guest against a
//! real V8 isolate.

use std::{
    collections::HashMap,
    path::{Path, PathBuf},
    process::Command,
    sync::{Mutex, OnceLock},
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
///
/// Each module is built once per test process. Tests run in parallel, and two
/// of them building the same module at once would each rewrite it.
pub fn build_wasix_test(name: &str) -> PathBuf {
    static BUILT: OnceLock<Mutex<HashMap<String, PathBuf>>> = OnceLock::new();
    // Held across the build: a test that needs a module another test is
    // building waits for it.
    let mut built = BUILT
        .get_or_init(Default::default)
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    if let Some(path) = built.get(name) {
        return path.clone();
    }
    let status = Command::new("./tests/build-test-wasix.sh")
        .arg(name)
        .current_dir(crate_root())
        .status()
        .expect("failed to execute tests/build-test-wasix.sh");
    assert!(status.success(), "failed to build WASIX test: {name}");
    let path = wasix_test_dir().join(format!("{name}.wasm"));
    built.insert(name.to_string(), path.clone());
    path
}

pub fn run_guest(ctx: &NapiCtx, wasm: &Path) -> anyhow::Result<(i32, String, String)> {
    run_wasix_main_capture_stdio_with_ctx(ctx, wasm, &[], &[])
}
