//! Embedder-managed V8 background lanes.
//!
//! A managed embedder runs each instance's V8 background work (garbage
//! collection, compilation, job workers) on one thread it owns, places and
//! bills, instead of on a process-wide pool. The provider supplies the queue
//! ([`ManagedV8Lane`]) and routes V8 work to it; the embedder decides when the
//! lane starts, which thread runs it, and when it stops.

use std::{cell::RefCell, ffi::c_void, ptr::NonNull, sync::Arc};

use anyhow::{Result, bail};

/// Called before each V8 background task runs on a lane. Dropping the
/// returned guard ends the task, for example its metered CPU interval.
pub type BackgroundTaskScope = Arc<dyn Fn() -> Box<dyn Send> + Send + Sync>;

/// Called when a guest first creates an environment, to get the instance's
/// lane. The embedder owns admission, worker startup, stop and completion,
/// and returns the same lane to every store of the instance. An error fails
/// that environment creation.
pub type ManagedV8LaneActivator = Arc<dyn Fn() -> Result<Arc<ManagedV8Lane>> + Send + Sync>;

unsafe extern "C" {
    fn snapi_v8_lane_new(
        max_queued_tasks: usize,
        scope_context: *mut c_void,
        enter_scope: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
        leave_scope: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
        on_overload: unsafe extern "C" fn(*mut c_void),
    ) -> *mut c_void;
    fn snapi_v8_lane_run(handle: *mut c_void);
    fn snapi_v8_lane_stop(handle: *mut c_void);
    fn snapi_v8_lane_delete(handle: *mut c_void);
    fn snapi_v8_lane_swap_current(handle: *mut c_void) -> *mut c_void;
    fn snapi_v8_use_managed_runtime() -> bool;
    fn snapi_v8_managed_runtime_requested() -> bool;
    fn snapi_v8_managed_runtime_usable() -> bool;
}

/// Whether this process has declared managed instances.
pub(crate) fn managed_runtime_requested() -> bool {
    unsafe { snapi_v8_managed_runtime_requested() }
}

/// Whether a managed environment can be created: V8 has not started, or
/// started in managed mode.
pub(crate) fn managed_runtime_usable() -> bool {
    unsafe { snapi_v8_managed_runtime_usable() }
}

/// Declares that this process runs instances on lanes, so that V8 starts
/// without the background work an isolate group shares. False if V8 already
/// started without that: managed environments are refused then.
pub(crate) fn use_managed_runtime() -> bool {
    unsafe { snapi_v8_use_managed_runtime() }
}

/// The V8 background task queue of one instance.
///
/// Every V8 worker task, delayed worker task and job worker that the
/// instance's environments post runs on the thread that calls [`run`], one
/// at a time. The queue is bounded: a post past `max_queued_tasks` stops the
/// lane and calls `on_overload`, as does a task whose scope cannot be
/// entered. Either way the instance's JavaScript can no longer make progress,
/// so `on_overload` should stop the instance; it must not wait on V8.
///
/// Size `max_queued_tasks` for legitimate bursts, not only for abuse: the
/// bound counts delayed tasks too, and V8 queues work per operation. Each
/// asynchronous `WebAssembly.compile` posts a decode task, and each compiled
/// module adds compile-job workers and a delayed code-caching task, all run
/// one at a time here.
///
/// Keep the lane alive while any environment created with it may still be
/// alive: the provider holds it through its environments.
///
/// [`run`]: ManagedV8Lane::run
pub struct ManagedV8Lane {
    handle: NonNull<c_void>,
    _callbacks: Box<LaneCallbacks>,
}

struct LaneCallbacks {
    task_scope: BackgroundTaskScope,
    on_overload: Arc<dyn Fn() + Send + Sync>,
}

// The native lane synchronizes all queue access. Its handle stays allocated
// while any Arc of this lane is alive.
unsafe impl Send for ManagedV8Lane {}
unsafe impl Sync for ManagedV8Lane {}

impl std::fmt::Debug for ManagedV8Lane {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ManagedV8Lane").finish_non_exhaustive()
    }
}

impl ManagedV8Lane {
    /// Allocates the queue. It starts no thread: the embedder runs it with
    /// [`run`](Self::run) on a thread it has admitted and accounts for.
    pub fn new(
        max_queued_tasks: usize,
        task_scope: BackgroundTaskScope,
        on_overload: Arc<dyn Fn() + Send + Sync>,
    ) -> Result<Arc<Self>> {
        let callbacks = Box::new(LaneCallbacks {
            task_scope,
            on_overload,
        });
        let handle = NonNull::new(unsafe {
            snapi_v8_lane_new(
                max_queued_tasks,
                (&*callbacks as *const LaneCallbacks).cast_mut().cast(),
                enter_task_scope,
                leave_task_scope,
                signal_overload,
            )
        });
        let Some(handle) = handle else {
            bail!("failed to allocate a V8 background lane");
        };
        Ok(Arc::new(Self {
            handle,
            _callbacks: callbacks,
        }))
    }

    /// Closes admission and wakes the lane's thread, which returns from
    /// [`run`](Self::run) after its current task. Queued tasks are dropped.
    /// Idempotent, and it never waits.
    pub fn stop(&self) {
        unsafe { snapi_v8_lane_stop(self.handle.as_ptr()) }
    }

    /// Runs the queue on the calling thread until [`stop`](Self::stop).
    pub fn run(&self) {
        unsafe { snapi_v8_lane_run(self.handle.as_ptr()) }
    }

    /// Binds this lane to the calling thread, so V8 work posted from it goes
    /// to the lane, until the returned scope is dropped.
    pub(crate) fn enter(self: &Arc<Self>) -> ManagedV8LaneScope {
        ManagedV8LaneScope::bind(Some(Arc::clone(self)))
    }
}

impl Drop for ManagedV8Lane {
    fn drop(&mut self) {
        unsafe {
            snapi_v8_lane_stop(self.handle.as_ptr());
            snapi_v8_lane_delete(self.handle.as_ptr());
        }
    }
}

unsafe extern "C" fn enter_task_scope(context: *mut c_void) -> *mut c_void {
    if context.is_null() {
        return std::ptr::null_mut();
    }
    let callbacks = unsafe { &*context.cast::<LaneCallbacks>() };
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| (callbacks.task_scope)())) {
        Ok(guard) => Box::into_raw(Box::new(guard)).cast(),
        Err(_) => std::ptr::null_mut(),
    }
}

unsafe extern "C" fn leave_task_scope(_context: *mut c_void, scope: *mut c_void) -> bool {
    if scope.is_null() {
        return false;
    }
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        drop(unsafe { Box::from_raw(scope.cast::<Box<dyn Send>>()) });
    }))
    .is_ok()
}

unsafe extern "C" fn signal_overload(context: *mut c_void) {
    if context.is_null() {
        return;
    }
    let callbacks = unsafe { &*context.cast::<LaneCallbacks>() };
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        (callbacks.on_overload)();
    }));
}

/// A lane binding of the calling thread, made by [`ManagedV8Lane::enter`] or
/// [`ManagedV8LaneScope::no_lane`]. Dropping it restores the binding below it,
/// also when scopes are dropped out of order.
pub(crate) struct ManagedV8LaneScope {
    id: u64,
    // A thread binding is restored only on the thread that made it.
    _thread_bound: std::marker::PhantomData<std::rc::Rc<()>>,
}

impl ManagedV8LaneScope {
    /// Binds no lane, for an env that must not inherit the caller's: a
    /// standalone env created from inside a call into a managed one.
    pub(crate) fn no_lane() -> Self {
        Self::bind(None)
    }

    fn bind(lane: Option<Arc<ManagedV8Lane>>) -> Self {
        let handle = lane
            .as_ref()
            .map_or(std::ptr::null_mut(), |lane| lane.handle.as_ptr());
        let id = CURRENT_LANES.with(|scopes| {
            let mut scopes = scopes.borrow_mut();
            // What was bound may come from a native bridge call this scope
            // runs inside, not from an outer scope of this stack.
            let previous = unsafe { snapi_v8_lane_swap_current(handle) };
            let id = scopes.next_id;
            scopes.next_id = scopes
                .next_id
                .checked_add(1)
                .expect("V8 lane scope ID overflow");
            scopes.entries.push(ScopeEntry { id, lane, previous });
            id
        });
        Self {
            id,
            _thread_bound: std::marker::PhantomData,
        }
    }
}

struct ScopeEntry {
    id: u64,
    // Keeps the bound lane alive while it may be bound or restored.
    lane: Option<Arc<ManagedV8Lane>>,
    // The binding this scope replaced, restored when it ends.
    previous: *mut c_void,
}

#[derive(Default)]
struct ThreadLaneScopes {
    next_id: u64,
    entries: Vec<ScopeEntry>,
}

thread_local! {
    // Scopes of this thread, innermost last. Native bridge calls bind lanes
    // too, lexically, between them.
    static CURRENT_LANES: RefCell<ThreadLaneScopes> = RefCell::new(ThreadLaneScopes::default());
}

impl Drop for ManagedV8LaneScope {
    fn drop(&mut self) {
        let _ = CURRENT_LANES.try_with(|scopes| {
            let mut scopes = scopes.borrow_mut();
            let Some(index) = scopes.entries.iter().position(|entry| entry.id == self.id) else {
                return;
            };
            let removed = scopes.entries.remove(index);
            if let Some(above) = scopes.entries.get_mut(index) {
                // Dropped out of order: the binding stays, and the scope above
                // now restores what this one replaced, never this lane.
                above.previous = removed.previous;
            } else {
                unsafe { snapi_v8_lane_swap_current(removed.previous) };
            }
            drop(removed);
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{
        Condvar, Mutex,
        atomic::{AtomicUsize, Ordering},
        mpsc,
    };
    use std::thread;
    use std::time::Duration;

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
            while !*ready {
                ready = changed.wait(ready).unwrap();
            }
        }
    }

    unsafe extern "C" fn run_noop_task(_data: *mut c_void) {}

    fn post(lane: &ManagedV8Lane, task: TestTask) {
        let data = Box::into_raw(Box::new(task));
        if !unsafe {
            snapi_v8_lane_post_test_task(lane.handle.as_ptr(), run_test_task, data.cast())
        } {
            drop(unsafe { Box::from_raw(data) });
            panic!("the lane refused the task");
        }
    }

    fn release(gate: &(Mutex<bool>, Condvar)) {
        let (lock, changed) = gate;
        *lock.lock().unwrap() = true;
        changed.notify_all();
    }

    #[test]
    fn zero_queue_capacity_is_rejected() {
        assert!(ManagedV8Lane::new(0, Arc::new(|| Box::new(())), Arc::new(|| {})).is_err());
    }

    #[test]
    fn dropping_nested_scopes_out_of_order_never_restores_a_released_lane() {
        let first = ManagedV8Lane::new(1, Arc::new(|| Box::new(())), Arc::new(|| {})).unwrap();
        let second = ManagedV8Lane::new(1, Arc::new(|| Box::new(())), Arc::new(|| {})).unwrap();
        let first_scope = first.enter();
        let second_scope = second.enter();
        drop(first_scope);
        drop(first);
        let active = unsafe { snapi_v8_lane_swap_current(std::ptr::null_mut()) };
        assert_eq!(active, second.handle.as_ptr());
        unsafe { snapi_v8_lane_swap_current(active) };
        drop(second_scope);
        let active = unsafe { snapi_v8_lane_swap_current(std::ptr::null_mut()) };
        assert!(active.is_null());
    }

    fn current() -> *mut c_void {
        let active = unsafe { snapi_v8_lane_swap_current(std::ptr::null_mut()) };
        unsafe { snapi_v8_lane_swap_current(active) };
        active
    }

    /// A native bridge call binds its env's lane between Rust scopes; a Rust
    /// scope inside it must give that binding back, not the outer Rust one.
    #[test]
    fn a_scope_restores_a_binding_made_outside_this_stack() {
        let outer = ManagedV8Lane::new(1, Arc::new(|| Box::new(())), Arc::new(|| {})).unwrap();
        let bridge = ManagedV8Lane::new(1, Arc::new(|| Box::new(())), Arc::new(|| {})).unwrap();
        let inner = ManagedV8Lane::new(1, Arc::new(|| Box::new(())), Arc::new(|| {})).unwrap();
        let outer_scope = outer.enter();
        // What a bridge call does natively.
        let before_bridge = unsafe { snapi_v8_lane_swap_current(bridge.handle.as_ptr()) };
        let inner_scope = inner.enter();
        assert_eq!(current(), inner.handle.as_ptr());
        drop(inner_scope);
        assert_eq!(current(), bridge.handle.as_ptr());
        unsafe { snapi_v8_lane_swap_current(before_bridge) };
        assert_eq!(current(), outer.handle.as_ptr());
        drop(outer_scope);
        assert!(current().is_null());
    }

    #[test]
    fn a_no_lane_scope_hides_the_callers_lane_until_dropped() {
        let lane = ManagedV8Lane::new(1, Arc::new(|| Box::new(())), Arc::new(|| {})).unwrap();
        let outer = lane.enter();
        assert_eq!(current(), lane.handle.as_ptr());
        let inner = ManagedV8LaneScope::no_lane();
        assert!(current().is_null());
        drop(inner);
        assert_eq!(current(), lane.handle.as_ptr());
        drop(outer);
        assert!(current().is_null());
    }

    #[test]
    fn a_blocked_lane_does_not_stall_another_and_scopes_are_balanced() {
        let entered = Arc::new(AtomicUsize::new(0));
        let exited = Arc::new(AtomicUsize::new(0));
        struct Guard(Arc<AtomicUsize>);
        impl Drop for Guard {
            fn drop(&mut self) {
                self.0.fetch_add(1, Ordering::SeqCst);
            }
        }
        let make_lane = || {
            let scope: BackgroundTaskScope = {
                let entered = Arc::clone(&entered);
                let exited = Arc::clone(&exited);
                Arc::new(move || {
                    entered.fetch_add(1, Ordering::SeqCst);
                    Box::new(Guard(Arc::clone(&exited)))
                })
            };
            ManagedV8Lane::new(256, scope, Arc::new(|| {})).unwrap()
        };
        let first = make_lane();
        let second = make_lane();
        let first_worker = {
            let lane = Arc::clone(&first);
            thread::spawn(move || lane.run())
        };
        let second_worker = {
            let lane = Arc::clone(&second);
            thread::spawn(move || lane.run())
        };
        let gate = Arc::new((Mutex::new(false), Condvar::new()));
        let (first_tx, first_rx) = mpsc::channel();
        post(
            &first,
            TestTask {
                started: first_tx,
                release: Some(Arc::clone(&gate)),
            },
        );
        first_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("the first task starts");
        let (second_tx, second_rx) = mpsc::channel();
        post(
            &second,
            TestTask {
                started: second_tx,
                release: None,
            },
        );
        second_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("the second instance progresses");
        first.stop();
        second.stop();
        release(&gate);
        first_worker.join().unwrap();
        second_worker.join().unwrap();
        assert_eq!(entered.load(Ordering::SeqCst), 2);
        assert_eq!(exited.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn queue_overload_signals_the_embedder_before_the_blocked_task_returns() {
        let overloads = Arc::new(AtomicUsize::new(0));
        let lane = ManagedV8Lane::new(256, Arc::new(|| Box::new(())), {
            let overloads = Arc::clone(&overloads);
            Arc::new(move || {
                overloads.fetch_add(1, Ordering::SeqCst);
            })
        })
        .unwrap();
        let worker = {
            let lane = Arc::clone(&lane);
            thread::spawn(move || lane.run())
        };
        let gate = Arc::new((Mutex::new(false), Condvar::new()));
        let (started_tx, started_rx) = mpsc::channel();
        post(
            &lane,
            TestTask {
                started: started_tx,
                release: Some(Arc::clone(&gate)),
            },
        );
        started_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        for _ in 0..256 {
            assert!(unsafe {
                snapi_v8_lane_post_test_task(
                    lane.handle.as_ptr(),
                    run_noop_task,
                    std::ptr::null_mut(),
                )
            });
        }
        assert!(!unsafe {
            snapi_v8_lane_post_test_task(lane.handle.as_ptr(), run_noop_task, std::ptr::null_mut())
        });
        assert_eq!(overloads.load(Ordering::SeqCst), 1);
        release(&gate);
        worker.join().unwrap();
    }
}
