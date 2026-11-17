#![cfg(feature = "cli")]

//! A budgeted application is created under resource limits before anything
//! runs in its isolate, including the automatic CPU profile that
//! `NAPI_V8_CPU_PROFILE_DIR` starts: it is refused in any process, not only a
//! managed one. Its own binary: the variable is set before any thread of the
//! process can read the environment.

use std::path::Path;

use wasmer_napi::{NapiCtx, cli::run_wasix_main_capture_stdio_with_ctx};

mod common;
use common::build_wasix_test;

fn profiles_in(directory: &Path) -> usize {
    std::fs::read_dir(directory)
        .unwrap()
        .filter(|entry| {
            entry
                .as_ref()
                .unwrap()
                .path()
                .extension()
                .is_some_and(|extension| extension == "cpuprofile")
        })
        .count()
}

#[test]
fn a_budgeted_application_is_not_profiled_by_the_host_variable() {
    let directory =
        std::env::temp_dir().join(format!("napi-budgeted-profile-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    // SAFETY: the test's only thread; nothing reads the environment yet.
    unsafe { std::env::set_var("NAPI_V8_CPU_PROFILE_DIR", &directory) };
    let wasm = build_wasix_test("test_gc_once");

    let budgeted = NapiCtx::builder()
        .total_memory_bytes(512 * 1024 * 1024)
        .build();
    let (exit_code, stdout, stderr) =
        run_wasix_main_capture_stdio_with_ctx(&budgeted, &wasm, &[], &[]).expect("budgeted run");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    let after_budgeted = profiles_in(&directory);

    // A standalone application in the same process still is.
    let (exit_code, stdout, stderr) =
        run_wasix_main_capture_stdio_with_ctx(&NapiCtx::default(), &wasm, &[], &[])
            .expect("standalone run");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    let after_standalone = profiles_in(&directory);
    let _ = std::fs::remove_dir_all(&directory);

    assert_eq!(after_budgeted, 0, "a budgeted application was profiled");
    assert_eq!(
        after_standalone, 1,
        "the standalone application was not profiled"
    );
}
