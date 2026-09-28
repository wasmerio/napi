//! One bounded V8 background execution lane per embedding context.

use std::{
    ffi::c_void,
    ptr::NonNull,
    sync::{Arc, Condvar, Mutex, mpsc},
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
        Ok(Self { handle, _scope: scope, budget })
    }

    fn stop(&self) {
        unsafe { snapi_v8_lane_stop(self.handle.as_ptr()) }
    }

    fn run(&self) {
        unsafe { snapi_v8_lane_run(self.handle.as_ptr()) }
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
        self.budget.uncharge(Pool::V8BackgroundLane, LANE_RESERVATION_BYTES);
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
    Initializing,
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
                State::Initializing => {
                    state = self.changed.wait(state).expect("poisoned V8 lane state");
                }
                State::Uninitialized => {
                    *state = State::Initializing;
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
            let worker_lane = Arc::clone(&lane);
            let on_overload = Arc::clone(&self.on_overload);
            let (ready_tx, ready_rx) = mpsc::sync_channel(1);
            (self.spawner)(Box::new(move || {
                let _ = ready_tx.send(());
                worker_lane.run();
                if worker_lane.overloaded() {
                    on_overload();
                }
            }))?;
            if ready_rx.recv_timeout(Duration::from_secs(5)).is_err() {
                lane.stop();
                bail!("V8 background lane did not start within five seconds");
            }
            Ok::<_, anyhow::Error>(lane)
        })();

        let mut state = self.state.lock().expect("poisoned V8 lane state");
        let outcome = match result {
            Ok(lane) if matches!(*state, State::Initializing) => {
                *state = State::Ready(Arc::clone(&lane));
                Ok(lane)
            }
            Ok(lane) => {
                lane.stop();
                if let State::Stopped(slot) = &mut *state {
                    *slot = Some(lane);
                }
                bail!("V8 background lane stopped during initialization")
            }
            Err(error) => {
                if matches!(*state, State::Initializing) {
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
