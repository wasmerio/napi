#![cfg(feature = "cli")]

use std::{
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
        mpsc,
    },
    thread,
    time::Duration,
};

use wasmer_napi::{NapiCtx, cli::run_wasix_main_capture_stdio_with_hooks};

mod common;
use common::build_wasix_test;

unsafe extern "C" {
    fn snapi_v8_fallback_worker_posts() -> u64;
}

struct TaskGuard(Arc<AtomicUsize>);

impl Drop for TaskGuard {
    fn drop(&mut self) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
}

#[test]
fn one_instance_lane_runs_and_releases_its_budget() {
    let wasm = build_wasix_test("test_gc_once");
    let spawns = Arc::new(AtomicUsize::new(0));
    let scopes = Arc::new(AtomicUsize::new(0));
    let scope_exits = Arc::new(AtomicUsize::new(0));
    let (finished_tx, finished_rx) = mpsc::channel();
    let hooks = NapiCtx::builder()
        .total_memory_bytes(512 * 1024 * 1024)
        .background_thread_spawner({
            let spawns = Arc::clone(&spawns);
            Arc::new(move |work| {
                spawns.fetch_add(1, Ordering::SeqCst);
                let finished_tx = finished_tx.clone();
                thread::Builder::new()
                    .name("napi-test-lane".into())
                    .spawn(move || {
                        work();
                        let _ = finished_tx.send(());
                    })?;
                Ok(())
            })
        })
        .background_task_scope({
            let scopes = Arc::clone(&scopes);
            let scope_exits = Arc::clone(&scope_exits);
            Arc::new(move || {
                scopes.fetch_add(1, Ordering::SeqCst);
                Box::new(TaskGuard(Arc::clone(&scope_exits)))
            })
        })
        .build_lazy_hooks()
        .unwrap();
    let budget = hooks.budget();
    let fallback_before = unsafe { snapi_v8_fallback_worker_posts() };
    assert_eq!(spawns.load(Ordering::SeqCst), 0);
    assert_eq!(budget.snapshot().v8_background_lane, 0);

    let (exit_code, stdout, stderr) =
        run_wasix_main_capture_stdio_with_hooks(&hooks, &wasm, &[], &[]).unwrap();
    assert_eq!(exit_code, 0, "{stderr}");
    assert!(stdout.contains("GC_DONE"), "{stdout}\n{stderr}");
    assert_eq!(spawns.load(Ordering::SeqCst), 1);
    assert!(hooks.is_initialized());
    assert_eq!(budget.snapshot().v8_background_lane, 8 * 1024 * 1024);

    hooks.runtime_control().shutdown_background_lane();
    finished_rx
        .recv_timeout(Duration::from_secs(5))
        .expect("lane worker exits");
    drop(hooks);
    assert_eq!(budget.snapshot().v8_background_lane, 0);
    assert_eq!(
        unsafe { snapi_v8_fallback_worker_posts() },
        fallback_before,
        "a managed V8 task escaped to the process-wide fallback pool"
    );
    let started = scopes.load(Ordering::SeqCst);
    assert!(started > 0, "GC did not exercise the metered V8 lane");
    assert_eq!(scope_exits.load(Ordering::SeqCst), started);
}
