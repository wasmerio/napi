#![cfg(feature = "cli")]

//! A process whose V8 started without managed mode (its first env was
//! standalone) refuses managed environments: their lanes could not keep the
//! instance's V8 work apart from the work all isolates share. Its own process
//! (test binary), since the mode is process-wide.

use std::sync::{
    Arc,
    atomic::{AtomicUsize, Ordering},
};

use wasmer_napi::{ManagedV8Lane, NapiCtx, cli::run_wasix_main_capture_stdio_with_ctx};

mod common;
use common::build_wasix_test;

unsafe extern "C" {
    fn snapi_v8_runtime_is_managed() -> bool;
    fn snapi_v8_standalone_pool_created() -> bool;
    fn snapi_v8_standalone_worker_posts() -> u64;
}

#[test]
fn managed_envs_are_refused_once_v8_started_unmanaged() {
    let wasm = build_wasix_test("test_gc_once");

    let standalone_posts_before = unsafe { snapi_v8_standalone_worker_posts() };
    let standalone = NapiCtx::default();
    let (exit_code, stdout, stderr) =
        run_wasix_main_capture_stdio_with_ctx(&standalone, &wasm, &[], &[])
            .expect("the standalone guest did not run");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(!unsafe { snapi_v8_runtime_is_managed() });
    // A standalone env builds the pool and runs its background work there.
    assert!(unsafe { snapi_v8_standalone_pool_created() });
    assert!(unsafe { snapi_v8_standalone_worker_posts() } > standalone_posts_before);

    let activations = Arc::new(AtomicUsize::new(0));
    let lanes: Arc<std::sync::Mutex<Vec<Arc<ManagedV8Lane>>>> = Arc::default();
    let hooks = NapiCtx::builder().build_managed_imports({
        let activations = Arc::clone(&activations);
        let lanes = Arc::clone(&lanes);
        Arc::new(move || {
            activations.fetch_add(1, Ordering::SeqCst);
            let lane = ManagedV8Lane::new(256, Arc::new(|| Box::new(())), Arc::new(|| {}))?;
            lanes.lock().unwrap().push(Arc::clone(&lane));
            Ok(lane)
        })
    });
    if let Ok((exit_code, stdout, stderr)) =
        run_wasix_main_capture_stdio_with_ctx(hooks.context(), &wasm, &[], &[])
    {
        assert_ne!(
            exit_code, 0,
            "a managed env was created\n{stdout}\n{stderr}"
        );
        assert!(!stdout.contains("GC_DONE"), "{stdout}");
    }
    // Refused before the embedder was asked to start a lane.
    assert_eq!(activations.load(Ordering::SeqCst), 0);
    assert!(!unsafe { snapi_v8_runtime_is_managed() });
}
