pub mod budget;
#[cfg(feature = "cli")]
pub mod cli;
mod ctx;
mod env;
mod guest;
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
mod guest_heap;
mod id_hash;
mod snapi;
#[cfg(all(target_arch = "wasm32", feature = "js"))]
mod snapi_js;
#[cfg(not(target_arch = "wasm32"))]
mod snapshot;
#[cfg(feature = "wasix")]
mod wasix;
use std::fmt::Display;

pub const NAPI_MODULE_NAME: &str = "napi";
pub const NAPI_EXTENSION_WASMER_MODULE_PREFIX: &str = "napi_extension_wasmer_v";
pub const NAPI_EXTENSION_WASMER_MODULE_NAME: &str = "napi_extension_wasmer_v0";

#[cfg(not(target_arch = "wasm32"))]
pub use budget::{BudgetedMemory, BudgetedTunables, budgeted_tunables};
pub use budget::{
    EnvRejected, HeapReservation, NapiMemoryAccountant, OverBudget, Pool, RequestedHeap,
    ResourceBudget, ResourceUsage,
};
pub use ctx::{
    NapiCtx, NapiCtxBuilder, NapiInstantiationState, NapiLimits, NapiRuntimeControl,
    NapiRuntimeHooks, NapiSession,
};
#[cfg(not(target_arch = "wasm32"))]
pub use snapshot::SnapshotEnvironment;
use enum_iterator::Sequence;
pub(crate) use env::NapiEnv;
#[cfg(all(target_arch = "wasm32", feature = "js"))]
pub(crate) use env::{GuestBackingStoreMapping, HostBufferCopy};

/// Host capabilities required by the JavaScript-backed N-API implementation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HostJsCapabilities {
    /// Whether the JavaScript engine exposes the JS Promise Integration API.
    pub jspi: bool,
}

/// Reports capabilities of the JavaScript host backend.
///
/// Returns `None` when the crate is not compiled for wasm32 with the `js`
/// feature. Promise-aware EdgeJS entry points must require `jspi == true`.
pub fn host_js_capabilities() -> Option<HostJsCapabilities> {
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    {
        return Some(HostJsCapabilities {
            jspi: snapi_js::has_jspi(),
        });
    }
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    {
        None
    }
}

/// Sets how many background worker threads V8 gets, process-wide.
///
/// V8 sizes this pool from the host's processor count, which suits a process
/// running one JS app. A host running many gets a pool whose threads compete
/// with every tenant's foreground JS, and whose work is attributed to no one:
/// GC marking and sweeping, parallel scavenging, and background compilation
/// all land there.
///
/// Passing `None` keeps V8's default. Lowering it doesn't strand work — V8's
/// parallel jobs are cooperative and the posting thread participates, so the
/// work migrates onto whichever thread asked for it.
///
/// A V8 job that nobody waits for, such as concurrent marking or sweeping or
/// an optimizing compile, uses at most `count - 1` of these threads, leaving
/// one for other background work. A job that V8 waits for (priority
/// `kUserBlocking`, or joined) uses all of them. With a count of 1, every job
/// gets the one thread.
///
/// The pool is built with the process-wide platform, when the first V8 isolate
/// is created, so this must be called before then. Afterwards, asking for the
/// size already in place still succeeds — several components may configure the
/// runtime from one setting — and asking for a different one returns
/// [`WorkerThreadsAlreadyFixed`].
pub fn set_v8_worker_threads(count: Option<u32>) -> Result<(), WorkerThreadsAlreadyFixed> {
    // Safety: plain integer in, no pointers; the callee only stores it.
    let applied = unsafe { snapi::snapi_bridge_set_v8_worker_thread_count(count.unwrap_or(0)) };
    if applied == 0 {
        return Err(WorkerThreadsAlreadyFixed);
    }
    Ok(())
}

/// The V8 platform already exists, so its worker pool can no longer be resized.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WorkerThreadsAlreadyFixed;

impl Display for WorkerThreadsAlreadyFixed {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(
            "the V8 worker thread count must be set before the first isolate is created, and \
             the V8 platform already exists",
        )
    }
}

impl std::error::Error for WorkerThreadsAlreadyFixed {}

/// The callback records of the V8 envs this thread owns, summed: a
/// diagnostic for tests and traces. Each function a guest makes through
/// N-API holds a registration and a binding for its callback, and a payload
/// in the engine; all three go when V8 collects the function.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CallbackRecordCounts {
    /// Callback registrations (guest function pointer and data).
    pub registrations: u64,
    /// Bindings: the data of live functions.
    pub bindings: u64,
    /// The engine's function callback payloads.
    pub payloads: u64,
    /// Bindings made over the envs' lives.
    pub made: u64,
    /// Bindings freed while these envs were live.
    pub freed: u64,
    /// Envs counted.
    pub envs: u64,
}

/// See [`CallbackRecordCounts`].
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub fn callback_record_counts() -> CallbackRecordCounts {
    let mut out = [0u64; 6];
    // Safety: the callee writes six u64s into `out`.
    unsafe { snapi::snapi_bridge_callback_record_counts(out.as_mut_ptr()) };
    CallbackRecordCounts {
        registrations: out[0],
        bindings: out[1],
        payloads: out[2],
        made: out[3],
        freed: out[4],
        envs: out[5],
    }
}

/// What [`set_foreground_task_waker`] installed.
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
type ForegroundTaskWaker = std::sync::Arc<dyn Fn() + Send + Sync>;

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
static FOREGROUND_TASK_WAKER: std::sync::RwLock<Option<ForegroundTaskWaker>> =
    std::sync::RwLock::new(None);

/// What the V8 platform calls while a waker is installed.
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
extern "C" fn wake_for_foreground_task() {
    // A panic must not unwind into V8, so the clone and its drop run inside
    // this guard. The call has a guard of its own: the clone can be the last
    // reference to the waker, and if its drop ran while a panic in the waker
    // unwound, a panicking destructor among its captures would abort the
    // process.
    let result = std::panic::catch_unwind(|| {
        // The lock is released before the call, so the waker may replace
        // itself.
        let waker = match FOREGROUND_TASK_WAKER.read() {
            Ok(slot) => slot.clone(),
            Err(poisoned) => poisoned.into_inner().clone(),
        };
        if let Some(waker) = waker {
            let called = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| waker()));
            // Dropping a payload could panic in turn: leak it instead.
            if let Err(payload) = called {
                std::mem::forget(payload);
            }
        }
    });
    if let Err(payload) = result {
        std::mem::forget(payload);
    }
}

/// Sets what is called, process-wide, when V8 queues a foreground task that
/// only a guest's next event-loop checkpoint runs.
///
/// A guest's foreground tasks wait in a queue that
/// `unofficial_napi_event_loop_checkpoint` runs, one task per checkpoint. A
/// guest can't have them handed to a hook of its own instead: the bridge gives
/// `unofficial_napi_attach_env` no native hooks. V8 posts these tasks from any
/// thread: to resolve an `Atomics.waitAsync` that another thread's
/// `Atomics.notify` woke, to finish a `WebAssembly.compile`, to run
/// `FinalizationRegistry` callbacks. A guest that sleeps until its next host
/// event is not told, so such a task waits until something else wakes the
/// guest. A host passes a waker that ends that sleep; the guest's next turn
/// takes the checkpoint that runs the task.
///
/// The waker is called after an immediate task is queued, on the thread that
/// posted it: another guest's thread, a V8 worker thread, or the guest's own
/// thread in the middle of a guest call or a garbage collection. V8 may hold
/// its own locks then. So the waker must be quick, must not call into V8 or
/// run guest code, and must not take a lock that the host holds around guest
/// calls. It is called for every isolate's tasks, so a host running several
/// guests wakes them all, and a guest with nothing to run waits again. Delayed
/// tasks don't call it: each runs at the first checkpoint after its delay.
/// While a guest's env terminates, its checkpoint refuses and runs no task, and
/// calls no waker, so the guest does not spin;
/// `unofficial_napi_cancel_terminate_execution` calls the waker again, on the
/// guest's thread, when tasks still wait. A panic in the waker is caught, and
/// its payload leaked, so that it never unwinds into V8.
///
/// `None` removes the waker. A waker may call this function itself. A call
/// that was already under way may still run the waker it replaced after this
/// returns, and then drop the last reference to it on the posting thread, so
/// what a waker captures must not call into V8 when it is dropped either.
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub fn set_foreground_task_waker(waker: Option<std::sync::Arc<dyn Fn() + Send + Sync>>) {
    let installed = waker.is_some();
    let previous = {
        let mut slot = match FOREGROUND_TASK_WAKER.write() {
            Ok(slot) => slot,
            Err(poisoned) => poisoned.into_inner(),
        };
        let previous = std::mem::replace(&mut *slot, waker);
        // Under the same lock, so that concurrent calls leave the slot and the
        // platform's pointer in agreement. Without a waker, the platform gets
        // no function at all and calls nothing.
        // Safety: a plain function that lives as long as the process.
        unsafe {
            snapi::snapi_bridge_set_foreground_task_waker(
                installed.then_some(wake_for_foreground_task as extern "C" fn()),
            );
        }
        previous
    };
    // Dropped once unlocked: what it captured may call back in.
    drop(previous);
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Sequence)]
pub enum NapiVersion {
    V10,
    Unknown,
}

impl Display for NapiVersion {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            NapiVersion::Unknown => write!(f, "napi_unknown"),
            NapiVersion::V10 => write!(f, "napi_v10"),
        }
    }
}

impl NapiVersion {
    pub const fn is_compatible_with(self, other: Self) -> bool {
        matches!(
            (self, other),
            (Self::V10, Self::V10) | (Self::Unknown, Self::V10)
        )
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NapiWasmerExtensionVersion {
    V0,
    Unknown,
}

impl NapiWasmerExtensionVersion {
    pub const fn is_compatible_with(self, other: Self) -> bool {
        matches!((self, other), (Self::V0, Self::V0))
    }
}

pub fn module_needs_napi(
    module: &wasmer::Module,
) -> (Option<NapiVersion>, Option<NapiWasmerExtensionVersion>) {
    NapiCtx::module_needs_napi(module)
}
