//! Opaque V8 background task queue for an embedder-managed execution lane.

use std::{cell::RefCell, ffi::c_void, ptr::NonNull, sync::Arc};

use anyhow::{Result, bail};

use crate::budget::{Pool, ResourceBudget};

/// Called for each actual V8 background task. Dropping the returned guard
/// ends that task's metered active interval.
pub type BackgroundTaskScope = Arc<dyn Fn() -> Box<dyn Send> + Send + Sync>;

/// Called by the provider on the first guest environment creation. The
/// embedder owns lazy admission, worker startup, stop, and completion.
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
    fn snapi_v8_lane_set_page_accountant(
        handle: *mut c_void,
        context: *mut c_void,
        charge: unsafe extern "C" fn(*mut c_void, u64) -> bool,
        uncharge: unsafe extern "C" fn(*mut c_void, u64),
        release: unsafe extern "C" fn(*mut c_void),
    ) -> bool;
}

pub struct ManagedV8Lane {
    handle: NonNull<c_void>,
    _callbacks: Box<LaneCallbacks>,
}

struct LaneCallbacks {
    task_scope: BackgroundTaskScope,
    on_overload: Arc<dyn Fn() + Send + Sync>,
}

// The C++ lane synchronizes all queue access. Its handle remains allocated
// while either the context or its dedicated worker owns this Arc.
unsafe impl Send for ManagedV8Lane {}
unsafe impl Sync for ManagedV8Lane {}

impl std::fmt::Debug for ManagedV8Lane {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ManagedV8Lane").finish_non_exhaustive()
    }
}

impl ManagedV8Lane {
    /// Allocate only the provider queue adapter. The embedder must first
    /// reserve its memory charge and native-thread admission.
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
            bail!("failed to allocate V8 background lane");
        };
        Ok(Arc::new(Self {
            handle,
            _callbacks: callbacks,
        }))
    }

    /// Close queue admission and wake the worker. Idempotent.
    pub fn stop(&self) {
        unsafe { snapi_v8_lane_stop(self.handle.as_ptr()) }
    }

    /// Run the queue on the embedder's dedicated, instance-accounted thread.
    /// Returns once `stop` is called and the current task finishes.
    pub fn run(&self) {
        unsafe { snapi_v8_lane_run(self.handle.as_ptr()) }
    }

    /// Charge commits of page-backed V8 buffers (resizable `ArrayBuffer`s,
    /// growable `SharedArrayBuffer`s) reserved while this lane is bound to
    /// `budget`. The first budget attached to a lane is kept; every context
    /// sharing a lane shares its application's budget.
    pub(crate) fn attach_page_accountant(&self, budget: &Arc<ResourceBudget>) {
        let context = Arc::into_raw(Arc::clone(budget))
            .cast_mut()
            .cast::<c_void>();
        let attached = unsafe {
            snapi_v8_lane_set_page_accountant(
                self.handle.as_ptr(),
                context,
                charge_backing_pages,
                uncharge_backing_pages,
                release_page_accountant,
            )
        };
        if !attached {
            // SAFETY: the lane did not take ownership of the reference.
            drop(unsafe { Arc::from_raw(context.cast::<ResourceBudget>()) });
        }
    }

    /// Bind this queue to the calling thread while V8 creates or enters an
    /// isolate. The guard restores the previous binding on drop.
    pub(crate) fn enter(self: &Arc<Self>) -> ManagedV8LaneScope {
        let id = CURRENT_LANES.with(|scopes| {
            let mut scopes = scopes.borrow_mut();
            let previous = unsafe { snapi_v8_lane_swap_current(self.handle.as_ptr()) };
            if scopes.entries.is_empty() {
                scopes.base = previous;
            }
            let id = scopes.next_id;
            scopes.next_id = scopes
                .next_id
                .checked_add(1)
                .expect("V8 lane scope ID overflow");
            scopes.entries.push((id, Arc::clone(self)));
            id
        });
        ManagedV8LaneScope {
            id,
            _lane: Arc::clone(self),
            _thread_bound: std::marker::PhantomData,
        }
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

// The page-accountant context is an `Arc<ResourceBudget>` reference owned by
// the native lane and its tracked regions; see `attach_page_accountant`.
unsafe extern "C" fn charge_backing_pages(context: *mut c_void, bytes: u64) -> bool {
    let budget = unsafe { &*context.cast::<ResourceBudget>() };
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        budget.try_charge_soft(Pool::V8BackingPages, bytes).is_ok()
    }))
    .unwrap_or(false)
}

unsafe extern "C" fn uncharge_backing_pages(context: *mut c_void, bytes: u64) {
    let budget = unsafe { &*context.cast::<ResourceBudget>() };
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        budget.uncharge(Pool::V8BackingPages, bytes);
    }));
}

unsafe extern "C" fn release_page_accountant(context: *mut c_void) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        drop(unsafe { Arc::from_raw(context.cast::<ResourceBudget>()) });
    }));
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

unsafe extern "C" fn signal_overload(context: *mut c_void) {
    if context.is_null() {
        return;
    }
    let callbacks = unsafe { &*context.cast::<LaneCallbacks>() };
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        (callbacks.on_overload)();
    }));
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

pub(crate) struct ManagedV8LaneScope {
    id: u64,
    _lane: Arc<ManagedV8Lane>,
    // Restoring a V8 platform binding is meaningful only on the thread that
    // entered it. Keep this scope statically non-Send even if pointer auto
    // traits change in a future compiler.
    _thread_bound: std::marker::PhantomData<std::rc::Rc<()>>,
}

#[derive(Default)]
struct ThreadLaneScopes {
    next_id: u64,
    base: *mut c_void,
    entries: Vec<(u64, Arc<ManagedV8Lane>)>,
}

thread_local! {
    // The Arc entries keep every previous lane alive while a nested scope can
    // restore it. Removing a non-top scope must not change the active binding.
    static CURRENT_LANES: RefCell<ThreadLaneScopes> = RefCell::new(ThreadLaneScopes::default());
}

impl Drop for ManagedV8LaneScope {
    fn drop(&mut self) {
        let _ = CURRENT_LANES.try_with(|scopes| {
            let mut scopes = scopes.borrow_mut();
            let Some(index) = scopes.entries.iter().position(|(id, _)| *id == self.id) else {
                return;
            };
            let was_top = index + 1 == scopes.entries.len();
            let removed = scopes.entries.remove(index);
            if was_top {
                let previous = scopes
                    .entries
                    .last()
                    .map_or(scopes.base, |(_, lane)| lane.handle.as_ptr());
                unsafe { snapi_v8_lane_swap_current(previous) };
            }
            if scopes.entries.is_empty() {
                scopes.base = std::ptr::null_mut();
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

    #[test]
    fn page_accountant_charges_softly_and_releases_its_budget_reference() {
        let budget = ResourceBudget::with_memory_limit(8 * 4096);
        let lane = ManagedV8Lane::new(1, Arc::new(|| Box::new(())), Arc::new(|| {})).unwrap();
        lane.attach_page_accountant(&budget);
        // A second attach is refused and must not leak its reference.
        lane.attach_page_accountant(&budget);
        assert_eq!(Arc::strong_count(&budget), 2);
        let context = Arc::as_ptr(&budget).cast_mut().cast();
        assert!(unsafe { charge_backing_pages(context, 6 * 4096) });
        assert!(!unsafe { charge_backing_pages(context, 4 * 4096) });
        assert_eq!(budget.snapshot().v8_backing_pages, 6 * 4096);
        unsafe { uncharge_backing_pages(context, 6 * 4096) };
        assert_eq!(budget.snapshot().v8_backing_pages, 0);
        drop(lane);
        assert_eq!(Arc::strong_count(&budget), 1);
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

    fn post(lane: &ManagedV8Lane, task: TestTask) {
        let data = Box::into_raw(Box::new(task));
        if !unsafe {
            snapi_v8_lane_post_test_task(lane.handle.as_ptr(), run_test_task, data.cast())
        } {
            drop(unsafe { Box::from_raw(data) });
            panic!("task admission rejected");
        }
    }

    #[test]
    fn blocked_lane_does_not_stall_another_and_scopes_are_balanced() {
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
        let release = Arc::new((Mutex::new(false), Condvar::new()));
        let (first_tx, first_rx) = mpsc::channel();
        post(
            &first,
            TestTask {
                started: first_tx,
                release: Some(Arc::clone(&release)),
            },
        );
        first_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("first task starts");
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
            .expect("second instance progresses");
        first.stop();
        second.stop();
        {
            let (lock, changed) = &*release;
            *lock.lock().unwrap() = true;
            changed.notify_all();
        }
        first_worker.join().unwrap();
        second_worker.join().unwrap();
        assert_eq!(entered.load(Ordering::SeqCst), 2);
        assert_eq!(exited.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn queue_overload_signals_embedder_before_blocked_task_returns() {
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
        let release = Arc::new((Mutex::new(false), Condvar::new()));
        let (started_tx, started_rx) = mpsc::channel();
        post(
            &lane,
            TestTask {
                started: started_tx,
                release: Some(Arc::clone(&release)),
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
        {
            let (lock, changed) = &*release;
            *lock.lock().unwrap() = true;
            changed.notify_all();
        }
        worker.join().unwrap();
    }
}
