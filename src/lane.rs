//! One bounded V8 background execution lane per embedding context.

use std::{
    ffi::c_void,
    ptr::NonNull,
    sync::{Arc, Condvar, Mutex, mpsc, atomic::{AtomicBool, Ordering}},
    time::Duration,
};

use anyhow::{Result, bail};
use crate::budget::{Pool, ResourceBudget};

const LANE_RESERVATION_BYTES: u64 = 8 * 1024 * 1024;

/// The embedder schedules one dedicated, instance-accounted native thread.
/// Returning an error must mean the closure was not accepted for execution.
pub type BackgroundThreadSpawner =
    Arc<dyn Fn(Box<dyn FnOnce() + Send>) -> Result<()> + Send + Sync>;

/// Called for each actual V8 background task. Dropping the returned guard
/// ends that task's metered active interval.
pub type BackgroundTaskScope = Arc<dyn Fn() -> Box<dyn Send> + Send + Sync>;

unsafe extern "C" {
    fn snapi_v8_lane_new(
        scope_context: *mut c_void,
        enter_scope: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
        leave_scope: unsafe extern "C" fn(*mut c_void, *mut c_void),
    ) -> *mut c_void;
    fn snapi_v8_lane_run(handle: *mut c_void);
    fn snapi_v8_lane_stop(handle: *mut c_void);
    fn snapi_v8_lane_delete(handle: *mut c_void);
    fn snapi_v8_lane_swap_current(handle: *mut c_void) -> *mut c_void;
    fn snapi_v8_lane_overloaded(handle: *mut c_void) -> bool;
}

struct BackgroundLane {
    handle: NonNull<c_void>,
    _scope: Box<BackgroundTaskScope>,
    budget: Arc<ResourceBudget>,
    charged: AtomicBool,
}

// The C++ lane synchronizes all queue access. Its handle remains allocated
// while either the context or its dedicated worker owns this Arc.
unsafe impl Send for BackgroundLane {}
unsafe impl Sync for BackgroundLane {}

impl BackgroundLane {
    fn new(budget: Arc<ResourceBudget>, scope: BackgroundTaskScope) -> Result<Self> {
        budget.try_charge(Pool::V8BackgroundLane, LANE_RESERVATION_BYTES)?;
        let scope = Box::new(scope);
        let handle = NonNull::new(unsafe {
            snapi_v8_lane_new(
                (&*scope as *const BackgroundTaskScope).cast_mut().cast(),
                enter_task_scope,
                leave_task_scope,
            )
        });
        let Some(handle) = handle else {
            budget.uncharge(Pool::V8BackgroundLane, LANE_RESERVATION_BYTES);
            bail!("failed to allocate V8 background lane");
        };
        Ok(Self { handle, _scope: scope, budget, charged: AtomicBool::new(true) })
    }

    fn stop(&self) {
        unsafe { snapi_v8_lane_stop(self.handle.as_ptr()) }
    }

    fn run(&self) {
        unsafe { snapi_v8_lane_run(self.handle.as_ptr()) }
    }

    fn release_reservation(&self) {
        if self.charged.swap(false, Ordering::AcqRel) {
            self.budget.uncharge(Pool::V8BackgroundLane, LANE_RESERVATION_BYTES);
        }
    }

    fn overloaded(&self) -> bool {
        unsafe { snapi_v8_lane_overloaded(self.handle.as_ptr()) }
    }

    fn enter(&self) -> LaneScope {
        let previous = unsafe { snapi_v8_lane_swap_current(self.handle.as_ptr()) };
        LaneScope { previous }
    }
}

impl Drop for BackgroundLane {
    fn drop(&mut self) {
        unsafe {
            snapi_v8_lane_stop(self.handle.as_ptr());
            snapi_v8_lane_delete(self.handle.as_ptr());
        }
        self.release_reservation();
    }
}

unsafe extern "C" fn enter_task_scope(context: *mut c_void) -> *mut c_void {
    if context.is_null() { return std::ptr::null_mut(); }
    let callback = unsafe { &*context.cast::<BackgroundTaskScope>() };
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| callback())) {
        Ok(guard) => Box::into_raw(Box::new(guard)).cast(),
        Err(_) => std::ptr::null_mut(),
    }
}

unsafe extern "C" fn leave_task_scope(_context: *mut c_void, scope: *mut c_void) {
    if scope.is_null() { return; }
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        drop(unsafe { Box::from_raw(scope.cast::<Box<dyn Send>>()) });
    }));
}

pub(crate) struct LaneScope {
    previous: *mut c_void,
}

impl Drop for LaneScope {
    fn drop(&mut self) {
        unsafe { snapi_v8_lane_swap_current(self.previous) };
    }
}

enum State {
    Uninitialized,
    Initializing(Option<Arc<BackgroundLane>>),
    Ready(Arc<BackgroundLane>),
    // Keep the native lane allocation until all N-API sessions drop. The C++
    // env state retains a raw lane pointer even after stop is requested.
    Stopped(Option<Arc<BackgroundLane>>),
}

pub(crate) struct LazyBackgroundLane {
    spawner: BackgroundThreadSpawner,
    state: Mutex<State>,
    changed: Condvar,
    on_overload: Arc<dyn Fn() + Send + Sync>,
    budget: Arc<ResourceBudget>,
    task_scope: BackgroundTaskScope,
}

impl std::fmt::Debug for LazyBackgroundLane {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("LazyBackgroundLane").finish_non_exhaustive()
    }
}

impl LazyBackgroundLane {
    pub(crate) fn new(
        spawner: BackgroundThreadSpawner,
        on_overload: Arc<dyn Fn() + Send + Sync>,
        budget: Arc<ResourceBudget>,
        task_scope: BackgroundTaskScope,
    ) -> Self {
        Self {
            spawner,
            state: Mutex::new(State::Uninitialized),
            changed: Condvar::new(),
            on_overload,
            budget,
            task_scope,
        }
    }

    pub(crate) fn enter(&self) -> Result<LaneScope> {
        let lane = self.ensure_started()?;
        Ok(lane.enter())
    }

    fn ensure_started(&self) -> Result<Arc<BackgroundLane>> {
        let mut state = self.state.lock().expect("poisoned V8 lane state");
        loop {
            match &*state {
                State::Ready(lane) => return Ok(Arc::clone(lane)),
                State::Stopped(_) => bail!("V8 background lane is stopped"),
                State::Initializing(_) => {
                    state = self.changed.wait(state).expect("poisoned V8 lane state");
                }
                State::Uninitialized => {
                    *state = State::Initializing(None);
                    break;
                }
            }
        }
        drop(state);

        let result = (|| {
            let lane = Arc::new(BackgroundLane::new(
                Arc::clone(&self.budget),
                Arc::clone(&self.task_scope),
            )?);
            {
                let mut state = self.state.lock().expect("poisoned V8 lane state");
                match &mut *state {
                    State::Initializing(slot) => *slot = Some(Arc::clone(&lane)),
                    State::Stopped(_) => {
                        lane.stop();
                        bail!("V8 background lane stopped during initialization");
                    }
                    _ => unreachable!("lane initializer lost ownership"),
                }
            }
            let worker_lane = Arc::clone(&lane);
            let on_overload = Arc::clone(&self.on_overload);
            let (ready_tx, ready_rx) = mpsc::sync_channel(1);
            (self.spawner)(Box::new(move || {
                let _ = ready_tx.send(());
                worker_lane.run();
                if worker_lane.overloaded() {
                    on_overload();
                }
                worker_lane.release_reservation();
            }))?;
            if ready_rx.recv_timeout(Duration::from_secs(5)).is_err() {
                lane.stop();
                bail!("V8 background lane did not start within five seconds");
            }
            Ok::<_, anyhow::Error>(lane)
        })();

        let mut state = self.state.lock().expect("poisoned V8 lane state");
        let outcome = match result {
            Ok(lane) if matches!(*state, State::Initializing(_)) => {
                *state = State::Ready(Arc::clone(&lane));
                Ok(lane)
            }
            Ok(lane) => {
                lane.stop();
                // `stop()` already retained the allocation if this init
                // raced it; the local `Arc` may now be released.
                drop(lane);
                bail!("V8 background lane stopped during initialization")
            }
            Err(error) => {
                if matches!(*state, State::Initializing(_)) {
                    *state = State::Uninitialized;
                }
                Err(error)
            }
        };
        self.changed.notify_all();
        outcome
    }

    pub(crate) fn stop(&self) {
        let mut state = self.state.lock().expect("poisoned V8 lane state");
        let previous = std::mem::replace(&mut *state, State::Stopped(None));
        *state = match previous {
            State::Ready(lane) => {
                lane.stop();
                State::Stopped(Some(lane))
            }
            State::Stopped(lane) => State::Stopped(lane),
            State::Initializing(lane) => {
                if let Some(lane) = &lane { lane.stop(); }
                State::Stopped(lane)
            }
            _ => State::Stopped(None),
        };
        self.changed.notify_all();
    }

    pub(crate) fn is_initialized(&self) -> bool {
        matches!(
            *self.state.lock().expect("poisoned V8 lane state"),
            State::Ready(_)
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{sync::{Barrier, atomic::{AtomicUsize, Ordering}}, thread};

    unsafe extern "C" {
        fn snapi_v8_lane_post_test_task(
            handle: *mut c_void,
            callback: unsafe extern "C" fn(*mut c_void),
            data: *mut c_void,
        ) -> bool;
    }

    struct TestTask {
        started: mpsc::Sender<()>,
        release: Option<Arc<(Mutex<bool>, Condvar)>>,
    }

    unsafe extern "C" fn run_test_task(data: *mut c_void) {
        let task = unsafe { Box::from_raw(data.cast::<TestTask>()) };
        let _ = task.started.send(());
        if let Some(release) = &task.release {
            let (lock, changed) = &**release;
            let mut ready = lock.lock().unwrap();
            while !*ready { ready = changed.wait(ready).unwrap(); }
        }
    }

    fn post(lane: &LazyBackgroundLane, task: TestTask) {
        let handle = match &*lane.state.lock().unwrap() {
            State::Ready(lane) => lane.handle.as_ptr(),
            _ => panic!("lane not ready"),
        };
        let data = Box::into_raw(Box::new(task));
        if !unsafe { snapi_v8_lane_post_test_task(handle, run_test_task, data.cast()) } {
            drop(unsafe { Box::from_raw(data) });
            panic!("task admission rejected");
        }
    }

    #[test]
    fn a_blocked_instance_lane_does_not_stall_another() {
        let entered = Arc::new(AtomicUsize::new(0));
        let exited = Arc::new(AtomicUsize::new(0));
        struct Guard(Arc<AtomicUsize>);
        impl Drop for Guard {
            fn drop(&mut self) { self.0.fetch_add(1, Ordering::SeqCst); }
        }
        let make_lane = || {
            let (finished_tx, finished_rx) = mpsc::channel();
            let spawner: BackgroundThreadSpawner = Arc::new(move |work| {
                let finished_tx = finished_tx.clone();
                thread::Builder::new().spawn(move || {
                    work();
                    let _ = finished_tx.send(());
                })?;
                Ok(())
            });
            let scope: BackgroundTaskScope = {
                let entered = Arc::clone(&entered);
                let exited = Arc::clone(&exited);
                Arc::new(move || {
                    entered.fetch_add(1, Ordering::SeqCst);
                    Box::new(Guard(Arc::clone(&exited)))
                })
            };
            let budget = ResourceBudget::with_memory_limit(16 * 1024 * 1024);
            (
                LazyBackgroundLane::new(spawner, Arc::new(|| {}), Arc::clone(&budget), scope),
                budget,
                finished_rx,
            )
        };
        let (first, first_budget, first_finished) = make_lane();
        let (second, second_budget, second_finished) = make_lane();
        drop(first.enter().unwrap());
        drop(second.enter().unwrap());
        let release = Arc::new((Mutex::new(false), Condvar::new()));
        let (first_tx, first_rx) = mpsc::channel();
        post(&first, TestTask { started: first_tx, release: Some(Arc::clone(&release)) });
        first_rx.recv_timeout(Duration::from_secs(2)).expect("first task starts");
        let (second_tx, second_rx) = mpsc::channel();
        post(&second, TestTask { started: second_tx, release: None });
        second_rx.recv_timeout(Duration::from_secs(2)).expect("second instance progresses independently");
        first.stop();
        second.stop();
        {
            let (lock, changed) = &*release;
            *lock.lock().unwrap() = true;
            changed.notify_all();
        }
        first_finished.recv_timeout(Duration::from_secs(2)).expect("first worker stops");
        second_finished.recv_timeout(Duration::from_secs(2)).expect("second worker stops");
        assert_eq!(entered.load(Ordering::SeqCst), 2);
        assert_eq!(exited.load(Ordering::SeqCst), 2);
        assert_eq!(first_budget.snapshot().v8_background_lane, 0);
        assert_eq!(second_budget.snapshot().v8_background_lane, 0);
    }

    #[test]
    fn concurrent_first_calls_admit_exactly_one_lane() {
        let spawns = Arc::new(AtomicUsize::new(0));
        let (finished_tx, finished_rx) = mpsc::channel();
        let spawner: BackgroundThreadSpawner = {
            let spawns = Arc::clone(&spawns);
            Arc::new(move |work| {
                spawns.fetch_add(1, Ordering::SeqCst);
                let finished_tx = finished_tx.clone();
                thread::Builder::new().spawn(move || {
                    work();
                    let _ = finished_tx.send(());
                })?;
                Ok(())
            })
        };
        let budget = ResourceBudget::with_memory_limit(8 * 1024 * 1024);
        let lane = Arc::new(LazyBackgroundLane::new(
            spawner, Arc::new(|| {}), Arc::clone(&budget), Arc::new(|| Box::new(())),
        ));
        let start = Arc::new(Barrier::new(16));
        let callers: Vec<_> = (0..16).map(|_| {
            let lane = Arc::clone(&lane);
            let start = Arc::clone(&start);
            thread::spawn(move || {
                start.wait();
                drop(lane.enter().expect("first call admitted"));
            })
        }).collect();
        for caller in callers { caller.join().unwrap(); }
        assert_eq!(spawns.load(Ordering::SeqCst), 1);
        assert_eq!(budget.snapshot().v8_background_lane, LANE_RESERVATION_BYTES);
        lane.stop();
        finished_rx.recv_timeout(Duration::from_secs(2)).expect("lane exits");
        assert_eq!(budget.snapshot().v8_background_lane, 0);
    }

    #[test]
    fn stop_wins_a_pending_first_call() {
        let (scheduled_tx, scheduled_rx) = mpsc::channel::<Box<dyn FnOnce() + Send>>();
        let spawner: BackgroundThreadSpawner = Arc::new(move |work| {
            scheduled_tx.send(work).map_err(|_| anyhow::anyhow!("worker receiver closed"))?;
            Ok(())
        });
        let budget = ResourceBudget::with_memory_limit(8 * 1024 * 1024);
        let lane = Arc::new(LazyBackgroundLane::new(
            spawner, Arc::new(|| {}), Arc::clone(&budget), Arc::new(|| Box::new(())),
        ));
        let caller_lane = Arc::clone(&lane);
        let caller = thread::spawn(move || caller_lane.enter().is_err());
        let scheduled = scheduled_rx.recv_timeout(Duration::from_secs(2)).expect("worker accepted");
        lane.stop();
        scheduled();
        assert!(caller.join().unwrap(), "stopped lane must reject first call");
        assert!(!lane.is_initialized());
        assert_eq!(budget.snapshot().v8_background_lane, 0);
    }

    #[test]
    fn admission_failure_releases_partial_lane() {
        let budget = ResourceBudget::with_memory_limit(8 * 1024 * 1024);
        let lane = LazyBackgroundLane::new(
            Arc::new(|_work| anyhow::bail!("global lane admission full")),
            Arc::new(|| {}), Arc::clone(&budget), Arc::new(|| Box::new(())),
        );
        assert!(lane.enter().is_err());
        assert!(!lane.is_initialized());
        assert_eq!(budget.snapshot().v8_background_lane, 0);
    }
}
