//! Opaque V8 background task queue for an embedder-managed execution lane.

use std::{ffi::c_void, ptr::NonNull, sync::Arc};

use anyhow::{Result, bail};

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

    /// Bind this queue to the calling thread while V8 creates or enters an
    /// isolate. The guard restores the previous binding on drop.
    pub fn enter(self: &Arc<Self>) -> ManagedV8LaneScope {
        let previous = unsafe { snapi_v8_lane_swap_current(self.handle.as_ptr()) };
        ManagedV8LaneScope {
            previous,
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

pub struct ManagedV8LaneScope {
    previous: *mut c_void,
    _lane: Arc<ManagedV8Lane>,
    // Restoring a V8 platform binding is meaningful only on the thread that
    // entered it. Keep this scope statically non-Send even if pointer auto
    // traits change in a future compiler.
    _thread_bound: std::marker::PhantomData<std::rc::Rc<()>>,
}

impl Drop for ManagedV8LaneScope {
    fn drop(&mut self) {
        unsafe { snapi_v8_lane_swap_current(self.previous) };
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
    fn zero_queue_capacity_is_rejected() {
        assert!(ManagedV8Lane::new(0, Arc::new(|| Box::new(())), Arc::new(|| {})).is_err());
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
