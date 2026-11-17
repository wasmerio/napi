#![cfg(feature = "cli")]

//! A managed process runs many tenants' applications, so the host's debug
//! variables do not apply to its isolates: `NAPI_V8_CPU_PROFILE_DIR` would
//! profile whichever env came first and write its function names and URLs to
//! a shared directory. Its own binary: the variable is set before any thread
//! of the process can read the environment.

use std::{
    sync::{Arc, Mutex},
    thread,
};

use wasmer_napi::{ManagedV8Lane, NapiCtx, cli::run_wasix_main_capture_stdio_with_ctx};

mod common;
use common::build_wasix_test;

#[test]
fn a_managed_process_ignores_the_cpu_profile_directory() {
    let directory =
        std::env::temp_dir().join(format!("napi-managed-profile-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    // SAFETY: the test's only thread; nothing reads the environment yet.
    unsafe { std::env::set_var("NAPI_V8_CPU_PROFILE_DIR", &directory) };

    let wasm = build_wasix_test("test_gc_once");
    let lane: Arc<Mutex<Option<Arc<ManagedV8Lane>>>> = Arc::default();
    let hooks = NapiCtx::builder()
        .total_memory_bytes(512 * 1024 * 1024)
        .build_managed_imports({
            let lane = Arc::clone(&lane);
            Arc::new(move || {
                let mut slot = lane.lock().unwrap();
                if let Some(lane) = &*slot {
                    return Ok(Arc::clone(lane));
                }
                let started = ManagedV8Lane::new(
                    256,
                    Arc::new(|| Box::new(()) as Box<dyn Send>),
                    Arc::new(|| {}),
                )?;
                let worker = Arc::clone(&started);
                thread::spawn(move || worker.run());
                *slot = Some(Arc::clone(&started));
                Ok(started)
            })
        });
    let (exit_code, stdout, stderr) =
        run_wasix_main_capture_stdio_with_ctx(hooks.context(), &wasm, &[], &[])
            .expect("the guest did not run");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(stdout.contains("GC_DONE"), "{stdout}\n{stderr}");
    if let Some(lane) = &*lane.lock().unwrap() {
        lane.stop();
    }

    let written: Vec<_> = std::fs::read_dir(&directory)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .collect();
    let _ = std::fs::remove_dir_all(&directory);
    assert!(
        written.is_empty(),
        "a managed process wrote CPU profiles: {written:?}"
    );
}
