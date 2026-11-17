#![cfg(feature = "cli")]

//! Managed embedders run each instance's V8 background work on a lane they
//! own (`NapiCtxBuilder::build_managed_imports`). Every test here is managed:
//! the process never makes an env with no lane, so it never builds the
//! process-wide worker pool, and no V8 work may go unattributed.

use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
        mpsc,
    },
    thread,
    time::{Duration, Instant},
};

use anyhow::bail;
use wasmer_napi::{
    ManagedV8Lane, ManagedV8LaneActivator, NapiCtx, NapiRuntimeHooks,
    cli::run_wasix_main_capture_stdio_with_ctx,
};

mod common;
use common::build_wasix_test;

unsafe extern "C" {
    fn snapi_v8_runtime_is_managed() -> bool;
    fn snapi_v8_standalone_pool_created() -> bool;
    fn snapi_v8_unattributed_worker_posts() -> u64;
}

/// One instance's lane, run on a thread of the test, with counts of what the
/// provider asked of it.
#[derive(Default)]
struct Lane {
    activations: AtomicUsize,
    tasks_started: Arc<AtomicUsize>,
    tasks_finished: Arc<AtomicUsize>,
    overloads: Arc<AtomicUsize>,
    lane: Mutex<Option<Arc<ManagedV8Lane>>>,
    worker: Mutex<Option<(thread::JoinHandle<()>, mpsc::Receiver<()>)>>,
}

struct TaskGuard(Arc<AtomicUsize>);

impl Drop for TaskGuard {
    fn drop(&mut self) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
}

impl Lane {
    /// The activator an embedder passes: the first call starts the lane,
    /// later calls (other stores of the instance) return it.
    fn activator(self: &Arc<Self>) -> ManagedV8LaneActivator {
        let this = Arc::clone(self);
        Arc::new(move || {
            this.activations.fetch_add(1, Ordering::SeqCst);
            let mut slot = this.lane.lock().unwrap();
            if let Some(lane) = &*slot {
                return Ok(Arc::clone(lane));
            }
            let started = Arc::clone(&this.tasks_started);
            let finished = Arc::clone(&this.tasks_finished);
            let overloads = Arc::clone(&this.overloads);
            let lane = ManagedV8Lane::new(
                256,
                Arc::new(move || {
                    started.fetch_add(1, Ordering::SeqCst);
                    Box::new(TaskGuard(Arc::clone(&finished))) as Box<dyn Send>
                }),
                Arc::new(move || {
                    overloads.fetch_add(1, Ordering::SeqCst);
                }),
            )?;
            let (exited_tx, exited_rx) = mpsc::channel();
            let worker_lane = Arc::clone(&lane);
            let worker = thread::Builder::new()
                .name("napi-test-lane".into())
                .spawn(move || {
                    worker_lane.run();
                    let _ = exited_tx.send(());
                })?;
            *this.worker.lock().unwrap() = Some((worker, exited_rx));
            *slot = Some(Arc::clone(&lane));
            Ok(lane)
        })
    }

    fn hooks(self: &Arc<Self>) -> NapiRuntimeHooks {
        NapiCtx::builder()
            .total_memory_bytes(512 * 1024 * 1024)
            .build_managed_imports(self.activator())
    }

    fn started(&self) -> bool {
        self.lane.lock().unwrap().is_some()
    }

    /// Waits until the lane has run a task, then stops it and waits for its
    /// thread: every task it started has finished by then.
    fn stop_after_work(&self) {
        let deadline = Instant::now() + Duration::from_secs(10);
        while self.tasks_started.load(Ordering::SeqCst) == 0 && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(10));
        }
        self.stop();
    }

    /// Stops the lane, as an embedder does on completion or a kill, and waits
    /// for its thread. The lane must not have overflowed before.
    fn stop(&self) {
        assert_eq!(
            self.overloads.load(Ordering::SeqCst),
            0,
            "the lane overflowed"
        );
        if let Some(lane) = &*self.lane.lock().unwrap() {
            lane.stop();
        }
        if let Some((worker, exited)) = self.worker.lock().unwrap().take() {
            exited
                .recv_timeout(Duration::from_secs(10))
                .expect("the lane's thread did not return after stop");
            worker.join().unwrap();
        }
    }
}

fn try_run(hooks: &NapiRuntimeHooks, program: &str) -> anyhow::Result<(i32, String, String)> {
    let wasm = build_wasix_test(program);
    run_wasix_main_capture_stdio_with_ctx(hooks.context(), &wasm, &[], &[])
}

fn run(hooks: &NapiRuntimeHooks, program: &str) -> (i32, String, String) {
    try_run(hooks, program).expect("the guest did not run")
}

/// Asserts that the GC guest could not create its env: the runner reports
/// its non-zero exit as an error.
fn assert_env_refused(hooks: &NapiRuntimeHooks) {
    if let Ok((exit_code, stdout, stderr)) = try_run(hooks, "test_gc_once") {
        assert_ne!(exit_code, 0, "the guest created an env\n{stdout}\n{stderr}");
        assert!(!stdout.contains("GC_DONE"), "{stdout}");
    }
}

/// Asserts that the process built no worker pool and dropped no V8 work for
/// want of a lane.
fn assert_all_v8_work_had_a_lane(unattributed_before: u64) {
    assert!(
        !unsafe { snapi_v8_standalone_pool_created() },
        "a managed process built the process-wide V8 worker pool"
    );
    assert_eq!(
        unsafe { snapi_v8_unattributed_worker_posts() },
        unattributed_before,
        "V8 posted worker tasks with no instance lane bound"
    );
}

#[test]
fn a_lane_starts_with_the_first_env_and_runs_the_instances_v8_work() {
    let unattributed_before = unsafe { snapi_v8_unattributed_worker_posts() };
    let lane = Arc::new(Lane::default());
    let hooks = lane.hooks();

    // Linking and configuring the runtime start nothing.
    let (exit_code, stdout, stderr) = run(&hooks, "test_configure_without_env");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("CONFIGURED_WITHOUT_ENV"),
        "{stdout}\n{stderr}"
    );
    assert_eq!(lane.activations.load(Ordering::SeqCst), 0);

    // The first env starts the lane; a later store of the instance gets the
    // same one.
    for _ in 0..2 {
        let (exit_code, stdout, stderr) = run(&hooks, "test_gc_once");
        assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
        assert!(stdout.contains("GC_DONE"), "{stdout}\n{stderr}");
    }
    assert!(lane.started());
    assert_eq!(lane.activations.load(Ordering::SeqCst), 2);
    // V8 runs without the work its isolates would share across lanes.
    assert!(unsafe { snapi_v8_runtime_is_managed() });

    lane.stop_after_work();
    let started = lane.tasks_started.load(Ordering::SeqCst);
    assert!(started > 0, "the instance's V8 work never ran on its lane");
    assert_eq!(lane.tasks_finished.load(Ordering::SeqCst), started);
    assert_all_v8_work_had_a_lane(unattributed_before);
}

#[test]
fn instances_run_their_v8_work_on_their_own_lanes() {
    let unattributed_before = unsafe { snapi_v8_unattributed_worker_posts() };
    let lanes: Vec<Arc<Lane>> = (0..2).map(|_| Arc::new(Lane::default())).collect();
    let guests: Vec<_> = lanes
        .iter()
        .map(|lane| {
            let hooks = lane.hooks();
            thread::spawn(move || run(&hooks, "test_gc_once"))
        })
        .collect();
    for guest in guests {
        let (exit_code, stdout, stderr) = guest.join().unwrap();
        assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    }
    for lane in &lanes {
        lane.stop_after_work();
        assert_eq!(lane.activations.load(Ordering::SeqCst), 1);
        assert!(
            lane.tasks_started.load(Ordering::SeqCst) > 0,
            "an instance's V8 work never ran on its own lane"
        );
    }
    assert_all_v8_work_had_a_lane(unattributed_before);
}

#[test]
fn a_stopped_instance_starts_no_lane() {
    let lane = Arc::new(Lane::default());
    let hooks = lane.hooks();
    let unattributed_before = unsafe { snapi_v8_unattributed_worker_posts() };
    hooks.runtime_control().terminate_all();
    assert_env_refused(&hooks);
    assert_eq!(lane.activations.load(Ordering::SeqCst), 0);
    assert_all_v8_work_had_a_lane(unattributed_before);
}

#[test]
fn a_refused_lane_fails_env_creation() {
    let unattributed_before = unsafe { snapi_v8_unattributed_worker_posts() };
    let refusals = Arc::new(AtomicUsize::new(0));
    let hooks = NapiCtx::builder().build_managed_imports({
        let refusals = Arc::clone(&refusals);
        Arc::new(move || {
            refusals.fetch_add(1, Ordering::SeqCst);
            bail!("no lane for this instance")
        })
    });
    assert_env_refused(&hooks);
    assert_eq!(refusals.load(Ordering::SeqCst), 1);
    // The env was not made on the process-wide pool instead.
    assert_all_v8_work_had_a_lane(unattributed_before);
}

/// How an embedder kills a running instance (Edge's kill_callback): stop its
/// lane, then terminate its JS. The spinning guest must stop, and its store
/// must tear down with the lane already stopped.
#[test]
fn killing_an_instance_stops_its_js_with_the_lane_already_stopped() {
    let unattributed_before = unsafe { snapi_v8_unattributed_worker_posts() };
    let lane = Arc::new(Lane::default());
    let hooks = lane.hooks();
    let control = hooks.runtime_control();
    let (finished_tx, finished_rx) = mpsc::channel();
    let guest = thread::spawn(move || {
        let outcome = try_run(&hooks, "test_js_infinite_loop");
        let _ = finished_tx.send(());
        outcome
    });
    let deadline = Instant::now() + Duration::from_secs(30);
    while !lane.started() && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(10));
    }
    assert!(lane.started(), "the guest never created its env");
    // Give the guest time to enter its loop.
    thread::sleep(Duration::from_secs(1));
    lane.stop();
    control.terminate_all();
    finished_rx
        .recv_timeout(Duration::from_secs(60))
        .expect("the killed instance did not stop");
    if let Ok((_exit_code, stdout, stderr)) = guest.join().expect("the guest thread panicked") {
        assert!(
            stdout.contains("JS_LOOP_ENTERED") && !stdout.contains("RESUME_STATUS=0"),
            "{stdout}\n{stderr}"
        );
    }
    assert_all_v8_work_had_a_lane(unattributed_before);
}

/// A managed instance's guests cannot set process-wide V8 flags.
#[test]
fn managed_guests_cannot_set_v8_flags() {
    let lane = Arc::new(Lane::default());
    let hooks = lane.hooks();
    let (exit_code, stdout, stderr) = run(&hooks, "test_managed_runtime_flags");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("MANAGED_RUNTIME_FLAGS_OK"),
        "{stdout}\n{stderr}"
    );
    lane.stop_after_work();
}
