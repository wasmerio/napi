//! Opaque V8 background task queue for an embedder-managed execution lane.

use std::{cell::RefCell, ffi::c_void, ptr::NonNull, sync::Arc};

use anyhow::{Result, bail};

use crate::{
    NapiRuntimeControl, WasmLimits,
    budget::{NapiLimitExceeded, Pool, ResourceBudget},
};

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
    fn snapi_v8_lane_set_wasm_accounting(
        handle: *mut c_void,
        accounting: *const SnapiWasmAccounting,
    ) -> bool;
    fn snapi_v8_lane_wasm_usage(handle: *mut c_void, out: *mut WasmLaneUsage);
    fn snapi_v8_wasm_process_stats(out: *mut WasmProcessStats);
}

/// `SNAPI_V8_WASM_CODE_LIMIT_*` in `edge_v8_platform.h`.
const WASM_CODE_LIMIT_BUDGET: u32 = 1;
const WASM_CODE_LIMIT_MEMORY: u32 = 2;
const WASM_CODE_LIMIT_PROCESS: u32 = 3;

/// Mirrors `snapi_v8_wasm_accounting` in `edge_v8_platform.h`.
#[repr(C)]
struct SnapiWasmAccounting {
    size: u32,
    max_memories: u32,
    max_reserved_bytes: u64,
    code_budget_bytes: u64,
    charge_memory: unsafe extern "C" fn(*mut c_void, u64) -> bool,
    uncharge_memory: unsafe extern "C" fn(*mut c_void, u64),
    charge_code: unsafe extern "C" fn(*mut c_void, u64) -> bool,
    uncharge_code: unsafe extern "C" fn(*mut c_void, u64),
    on_code_limit: unsafe extern "C" fn(*mut c_void, u32, u64, u64),
}

/// Metered WebAssembly in one context ([`crate::WasmPolicy::EnabledMetered`]).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WasmLaneUsage {
    /// Live wasm memories.
    pub memories: u64,
    /// Address space reserved by those memories (not charged).
    pub reserved_bytes: u64,
    /// Committed wasm code charged to the context.
    pub code_bytes: u64,
}

/// Process-wide metered WebAssembly counters.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WasmProcessStats {
    /// Committed wasm code in every metered code space of the process.
    pub code_committed_bytes: u64,
    /// Process-wide soft code budget (0 if wasm code is not metered).
    pub code_budget_bytes: u64,
    /// Process-wide hard code limit, twice the soft budget (0 if wasm code
    /// is not metered).
    pub code_hard_limit_bytes: u64,
    /// Wasm memory reservations refused by a per-context cap.
    pub memory_cap_denials: u64,
    /// Compilations refused by a context or process code budget.
    pub codegen_denials: u64,
    /// Contexts stopped for their wasm code.
    pub code_limit_stops: u64,
    /// Wasm code V8 decommitted (cumulative bytes).
    pub code_decommitted_bytes: u64,
    /// Committed bytes of metered wasm memories in the process.
    pub memory_committed_bytes: u64,
    /// Live metered wasm memories in the process.
    pub memories: u64,
}

/// Process-wide metered WebAssembly counters (all zero unless
/// [`crate::configure_wasm_engine`] was applied).
pub fn wasm_process_stats() -> WasmProcessStats {
    let mut stats = WasmProcessStats::default();
    unsafe { snapi_v8_wasm_process_stats(&mut stats) };
    stats
}

/// The native lane's page-accountant context: the budget charged for pages
/// V8 commits while the lane is bound, and, for metered WebAssembly, the
/// control that stops the context when its code exceeds its limits.
struct LaneAccountant {
    budget: Arc<ResourceBudget>,
    control: Option<NapiRuntimeControl>,
}

/// Metered WebAssembly for a lane's context.
pub(crate) struct LaneWasm {
    pub(crate) limits: WasmLimits,
    pub(crate) control: NapiRuntimeControl,
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
    /// `budget`, and with `wasm` meter WebAssembly memory and code. The first
    /// budget attached to a lane is kept; every context sharing a lane
    /// shares its application's budget.
    pub(crate) fn attach_page_accountant(
        &self,
        budget: &Arc<ResourceBudget>,
        wasm: Option<LaneWasm>,
    ) {
        let limits = wasm.as_ref().map(|wasm| wasm.limits);
        let accountant = Arc::new(LaneAccountant {
            budget: Arc::clone(budget),
            control: wasm.map(|wasm| wasm.control),
        });
        let context = Arc::into_raw(accountant).cast_mut().cast::<c_void>();
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
            drop(unsafe { Arc::from_raw(context.cast::<LaneAccountant>()) });
            return;
        }
        let Some(limits) = limits else {
            return;
        };
        let accounting = SnapiWasmAccounting {
            size: std::mem::size_of::<SnapiWasmAccounting>() as u32,
            max_memories: limits.max_memories,
            max_reserved_bytes: limits.reserved_bytes_cap(budget.memory_limit()),
            code_budget_bytes: limits.code_budget_bytes,
            charge_memory: charge_wasm_memory,
            uncharge_memory: uncharge_wasm_memory,
            charge_code: charge_wasm_code,
            uncharge_code: uncharge_wasm_code,
            on_code_limit: on_wasm_code_limit,
        };
        // Only fails for a lane whose accountant already meters wasm;
        // environment creation with metered WebAssembly then still fails
        // closed if it does not.
        let _ = unsafe { snapi_v8_lane_set_wasm_accounting(self.handle.as_ptr(), &accounting) };
    }

    /// Metered WebAssembly usage of this lane's context (zero without
    /// [`crate::WasmPolicy::EnabledMetered`]).
    pub fn wasm_usage(&self) -> WasmLaneUsage {
        let mut usage = WasmLaneUsage::default();
        unsafe { snapi_v8_lane_wasm_usage(self.handle.as_ptr(), &mut usage) };
        usage
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

// The page-accountant context is an `Arc<LaneAccountant>` reference owned by
// the native lane and its tracked regions; see `attach_page_accountant`.
// Every callback may run on any thread, inside V8, and must not call back
// into V8.
fn lane_accountant<'a>(context: *mut c_void) -> &'a LaneAccountant {
    // SAFETY: the native side holds the reference while it calls back.
    unsafe { &*context.cast::<LaneAccountant>() }
}

fn guarded<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)).unwrap_or(fallback)
}

unsafe extern "C" fn charge_backing_pages(context: *mut c_void, bytes: u64) -> bool {
    let budget = &lane_accountant(context).budget;
    guarded(false, || {
        budget.try_charge_soft(Pool::V8BackingPages, bytes).is_ok()
    })
}

unsafe extern "C" fn uncharge_backing_pages(context: *mut c_void, bytes: u64) {
    let budget = &lane_accountant(context).budget;
    guarded((), || budget.uncharge(Pool::V8BackingPages, bytes));
}

unsafe extern "C" fn charge_wasm_memory(context: *mut c_void, bytes: u64) -> bool {
    let budget = &lane_accountant(context).budget;
    guarded(false, || {
        budget.try_charge_soft(Pool::V8WasmMemory, bytes).is_ok()
    })
}

unsafe extern "C" fn uncharge_wasm_memory(context: *mut c_void, bytes: u64) {
    let budget = &lane_accountant(context).budget;
    guarded((), || budget.uncharge(Pool::V8WasmMemory, bytes));
}

// Asked softly: this runs inside V8's code allocator with its locks held, so
// the embedder must not stop the application here. A refusal stops the
// context through `on_wasm_code_limit`, which reports it to the embedder
// (`NapiLimitExceeded::WasmCodeMemory`) from a helper thread.
unsafe extern "C" fn charge_wasm_code(context: *mut c_void, bytes: u64) -> bool {
    let budget = &lane_accountant(context).budget;
    guarded(false, || {
        budget.try_charge_soft(Pool::V8WasmCode, bytes).is_ok()
    })
}

unsafe extern "C" fn uncharge_wasm_code(context: *mut c_void, bytes: u64) {
    let budget = &lane_accountant(context).budget;
    guarded((), || budget.uncharge(Pool::V8WasmCode, bytes));
}

/// Stops the context: its committed wasm code was refused by the budget,
/// exceeded the code budget, or took the process past its hard code limit.
/// Runs inside V8's code allocator with its locks held, possibly on the lane
/// thread while another thread holds bridge locks and waits for the lane's
/// current task, so it only sets the sticky stop flag here; the isolates are
/// terminated and the embedder is told from a helper thread, so embedder
/// locks are never taken under V8's.
unsafe extern "C" fn on_wasm_code_limit(
    context: *mut c_void,
    reason: u32,
    committed: u64,
    limit: u64,
) {
    let accountant = lane_accountant(context);
    guarded((), || {
        let Some(control) = accountant.control.clone() else {
            return;
        };
        control.mark_stopped();
        let budget = Arc::clone(&accountant.budget);
        let notify = move || match reason {
            WASM_CODE_LIMIT_BUDGET => {
                budget.notify_limit_exceeded(NapiLimitExceeded::WasmCodeBudget {
                    committed,
                    budget: limit,
                })
            }
            WASM_CODE_LIMIT_PROCESS => budget
                .notify_limit_exceeded(NapiLimitExceeded::WasmProcessCode { committed, limit }),
            WASM_CODE_LIMIT_MEMORY => {
                let limit = budget.memory_limit();
                budget.notify_limit_exceeded(NapiLimitExceeded::WasmCodeMemory { committed, limit })
            }
            _ => {}
        };
        let stop = move || {
            control.terminate_all();
            notify();
        };
        // If no thread can be started, the stop flag still keeps imports
        // and new isolates out, and the isolate that committed the code was
        // already terminated; reporting inline could take embedder locks
        // under V8's, so it is skipped.
        let _ = std::thread::Builder::new()
            .name("napi-wasm-stop".into())
            .spawn(stop);
    });
}

unsafe extern "C" fn release_page_accountant(context: *mut c_void) {
    guarded((), || {
        drop(unsafe { Arc::from_raw(context.cast::<LaneAccountant>()) });
    });
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
        lane.attach_page_accountant(&budget, None);
        // A second attach is refused and must not leak its reference.
        lane.attach_page_accountant(&budget, None);
        assert_eq!(Arc::strong_count(&budget), 2);
        let accountant = LaneAccountant {
            budget: Arc::clone(&budget),
            control: None,
        };
        let context = (&accountant as *const LaneAccountant).cast_mut().cast();
        assert!(unsafe { charge_backing_pages(context, 6 * 4096) });
        assert!(!unsafe { charge_backing_pages(context, 4 * 4096) });
        assert_eq!(budget.snapshot().v8_backing_pages, 6 * 4096);
        unsafe { uncharge_backing_pages(context, 6 * 4096) };
        assert_eq!(budget.snapshot().v8_backing_pages, 0);
        drop(accountant);
        drop(lane);
        assert_eq!(Arc::strong_count(&budget), 1);
    }

    #[test]
    fn wasm_callbacks_charge_their_pools() {
        let budget = ResourceBudget::with_memory_limit(8 * 4096);
        let accountant = LaneAccountant {
            budget: Arc::clone(&budget),
            control: None,
        };
        let context = (&accountant as *const LaneAccountant).cast_mut().cast();
        assert!(unsafe { charge_wasm_memory(context, 4 * 4096) });
        assert!(unsafe { charge_wasm_code(context, 3 * 4096) });
        assert!(!unsafe { charge_wasm_memory(context, 2 * 4096) });
        assert!(!unsafe { charge_wasm_code(context, 2 * 4096) });
        let usage = budget.snapshot();
        assert_eq!(
            (usage.v8_wasm_memory, usage.v8_wasm_code),
            (4 * 4096, 3 * 4096)
        );
        unsafe {
            uncharge_wasm_memory(context, 4 * 4096);
            uncharge_wasm_code(context, 3 * 4096);
            // Without a stop control (unmetered lanes) a limit is ignored.
            on_wasm_code_limit(context, WASM_CODE_LIMIT_BUDGET, 1, 1);
        }
        assert_eq!(budget.memory_charged(), 0);
    }

    /// Code is charged from inside V8's code allocator: an embedder's
    /// terminal `try_charge` (which may stop the application and take
    /// embedder locks) must never run there.
    #[test]
    fn wasm_code_is_charged_softly_with_its_pool() {
        #[derive(Default)]
        struct Embedder {
            terminal: AtomicUsize,
            soft: Mutex<Vec<(Pool, u64)>>,
        }
        impl crate::NapiMemoryAccountant for Embedder {
            fn memory_limit(&self) -> u64 {
                4096
            }
            fn memory_charged(&self) -> u64 {
                0
            }
            fn try_charge(&self, _bytes: u64) -> bool {
                self.terminal.fetch_add(1, Ordering::SeqCst);
                false
            }
            fn try_charge_soft_for(&self, pool: Pool, bytes: u64) -> bool {
                self.soft.lock().unwrap().push((pool, bytes));
                false
            }
            fn uncharge(&self, _bytes: u64) {}
        }
        let embedder = Arc::new(Embedder::default());
        let accountant = LaneAccountant {
            budget: ResourceBudget::with_accountant(embedder.clone()),
            control: None,
        };
        let context = (&accountant as *const LaneAccountant).cast_mut().cast();
        assert!(!unsafe { charge_wasm_code(context, 8192) });
        assert_eq!(embedder.terminal.load(Ordering::SeqCst), 0);
        assert_eq!(*embedder.soft.lock().unwrap(), [(Pool::V8WasmCode, 8192)]);
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
