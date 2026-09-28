#![cfg(feature = "cli")]

use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
        mpsc,
    },
    thread,
    time::Duration,
};

use wasmer_napi::{ManagedV8Lane, NapiCtx, cli::run_wasix_main_capture_stdio_with_hooks};

mod common;
use common::build_wasix_test;

unsafe extern "C" {
    fn snapi_v8_fallback_worker_posts() -> u64;
    fn snapi_v8_platform_created() -> bool;
    fn snapi_v8_standalone_pool_created() -> bool;
    fn snapi_v8_unattributed_worker_posts() -> u64;
}

struct TaskGuard(Arc<AtomicUsize>);

impl Drop for TaskGuard {
    fn drop(&mut self) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
}

#[test]
fn embedder_lane_starts_on_first_env_and_routes_gc_tasks() {
    let configure_only = build_wasix_test("test_configure_without_env");
    let wasm = build_wasix_test("test_gc_once");
    let spawns = Arc::new(AtomicUsize::new(0));
    let scopes = Arc::new(AtomicUsize::new(0));
    let scope_exits = Arc::new(AtomicUsize::new(0));
    let (finished_tx, finished_rx) = mpsc::channel();
    let lane_slot: Arc<Mutex<Option<Arc<ManagedV8Lane>>>> = Arc::new(Mutex::new(None));
    let hooks = NapiCtx::builder()
        .total_memory_bytes(512 * 1024 * 1024)
        .build_managed_imports({
            let spawns = Arc::clone(&spawns);
            let lane_slot = Arc::clone(&lane_slot);
            let scopes = Arc::clone(&scopes);
            let scope_exits = Arc::clone(&scope_exits);
            Arc::new(move || {
                let mut slot = lane_slot.lock().unwrap();
                if let Some(lane) = &*slot {
                    return Ok(Arc::clone(lane));
                }
                let task_scope = {
                    let scopes = Arc::clone(&scopes);
                    let scope_exits = Arc::clone(&scope_exits);
                    Arc::new(move || {
                        scopes.fetch_add(1, Ordering::SeqCst);
                        Box::new(TaskGuard(Arc::clone(&scope_exits))) as Box<dyn Send>
                    })
                };
                let lane = ManagedV8Lane::new(256, task_scope, Arc::new(|| {}))?;
                let worker_lane = Arc::clone(&lane);
                let finished_tx = finished_tx.clone();
                thread::Builder::new()
                    .name("napi-test-lane".into())
                    .spawn(move || {
                        worker_lane.run();
                        let _ = finished_tx.send(());
                    })?;
                spawns.fetch_add(1, Ordering::SeqCst);
                *slot = Some(Arc::clone(&lane));
                Ok(lane)
            })
        });
    let budget = hooks.budget();
    let fallback_before = unsafe { snapi_v8_fallback_worker_posts() };
    let unattributed_before = unsafe { snapi_v8_unattributed_worker_posts() };
    assert_eq!(spawns.load(Ordering::SeqCst), 0);
    assert_eq!(budget.snapshot().v8_background_lane, 0);
    assert!(!unsafe { snapi_v8_platform_created() });
    let (exit_code, stdout, stderr) =
        run_wasix_main_capture_stdio_with_hooks(&hooks, &configure_only, &[], &[]).unwrap();
    assert_eq!(exit_code, 0, "{stderr}");
    assert!(
        stdout.contains("CONFIGURED_WITHOUT_ENV"),
        "{stdout}\n{stderr}"
    );
    assert_eq!(spawns.load(Ordering::SeqCst), 0);
    assert!(!unsafe { snapi_v8_platform_created() });

    let (exit_code, stdout, stderr) =
        run_wasix_main_capture_stdio_with_hooks(&hooks, &wasm, &[], &[]).unwrap();
    assert_eq!(exit_code, 0, "{stderr}");
    assert!(stdout.contains("GC_DONE"), "{stdout}\n{stderr}");
    assert_eq!(spawns.load(Ordering::SeqCst), 1);
    assert!(lane_slot.lock().unwrap().is_some());
    assert!(unsafe { snapi_v8_platform_created() });
    assert!(!unsafe { snapi_v8_standalone_pool_created() });
    assert_eq!(
        budget.snapshot().v8_background_lane,
        0,
        "the embedder, not N-API, owns the lane reservation"
    );

    lane_slot.lock().unwrap().as_ref().unwrap().stop();
    finished_rx
        .recv_timeout(Duration::from_secs(5))
        .expect("lane worker exits");
    drop(hooks);
    assert_eq!(
        unsafe { snapi_v8_fallback_worker_posts() },
        fallback_before,
        "a managed V8 task escaped to the process-wide fallback pool"
    );
    assert_eq!(
        unsafe { snapi_v8_unattributed_worker_posts() },
        unattributed_before,
        "managed V8 posted work without its instance lane"
    );
    let started = scopes.load(Ordering::SeqCst);
    assert!(started > 0, "GC did not exercise the metered V8 lane");
    assert_eq!(scope_exits.load(Ordering::SeqCst), started);
}
