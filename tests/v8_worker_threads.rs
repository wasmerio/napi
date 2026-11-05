#![cfg(all(feature = "cli", target_os = "linux"))]

//! Sizing V8's background worker pool.
//!
//! The pool belongs to the process-wide V8 platform and is built with it, so
//! this test needs a process to itself — which is what a separate integration
//! test binary gives it. Thread names come from `/proc`, hence Linux only.

use std::fs;

use wasmer_napi::NapiCtx;

mod common;
use common::{build_wasix_test, run_guest};

/// Linux truncates thread names to 15 characters, so V8's "V8 DefaultWorker"
/// arrives as this.
const V8_WORKER_COMM: &str = "V8 DefaultWorke";

const REQUESTED_WORKERS: u32 = 2;

fn v8_worker_thread_count() -> usize {
    let tasks = fs::read_dir("/proc/self/task").expect("failed to read /proc/self/task");
    tasks
        .filter_map(Result::ok)
        .filter(|task| {
            fs::read_to_string(task.path().join("comm"))
                .map(|comm| comm.trim() == V8_WORKER_COMM)
                .unwrap_or(false)
        })
        .count()
}

#[test]
fn worker_pool_size_is_configurable_before_the_first_isolate() {
    assert_eq!(
        v8_worker_thread_count(),
        0,
        "V8 should not have started any workers before the platform exists"
    );

    wasmer_napi::set_v8_worker_threads(Some(REQUESTED_WORKERS))
        .expect("the pool should be sizeable before any isolate exists");

    // Running a guest creates the isolate, and with it the platform and its
    // worker pool.
    let wasm = build_wasix_test("hello_napi_test");
    let ctx = NapiCtx::default();
    let (exit_code, stdout, stderr) = run_guest(&ctx, &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "guest exited with {exit_code}\n{stderr}");
    assert!(
        stdout.contains("HELLO_NAPI_TEST_OK=1"),
        "guest did not report success\n{stdout}"
    );

    // Without this, V8 sizes the pool from the host's processor count.
    assert_eq!(
        v8_worker_thread_count(),
        REQUESTED_WORKERS as usize,
        "V8 did not honour the configured worker count"
    );

    // The pool is fixed once the platform exists, and saying so beats
    // pretending a later change took effect.
    assert!(
        wasmer_napi::set_v8_worker_threads(Some(REQUESTED_WORKERS + 2)).is_err(),
        "resizing the pool after the platform was built should be refused"
    );
    assert_eq!(v8_worker_thread_count(), REQUESTED_WORKERS as usize);
}
