use std::collections::{HashMap, HashSet};
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
use std::ffi::c_void;
use std::sync::Arc;
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
use std::sync::atomic::AtomicU64;
use std::sync::atomic::{AtomicBool, Ordering};

#[cfg(all(target_arch = "wasm32", feature = "js"))]
use std::sync::atomic::{AtomicU32, Ordering as AtomicOrdering};

#[cfg(all(target_arch = "wasm32", feature = "js"))]
use wasmer::TypedFunction;
use wasmer::{Memory, Table};

use crate::budget::{EnvHeapCharge, Pool, ResourceBudget};
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
use crate::budget::{EnvRejected, HeapReservation, RequestedHeap};
use crate::snapi::SnapiEnv;
#[cfg(all(target_arch = "wasm32", feature = "js"))]
use crate::snapi::snapi_bridge_unofficial_release_env;

#[cfg(all(target_arch = "wasm32", feature = "js"))]
use crate::{guest::callback::CallbackInvocationCtx, snapi::snapi_bridge_swap_active_callback_ctx};

#[cfg(all(target_arch = "wasm32", feature = "js"))]
pub(crate) struct HostBufferCopy {
    pub(crate) owner_id: u64,
    pub(crate) guest_env: u32,
    pub(crate) handle_id: u32,
    pub(crate) host_reference_id: u32,
    pub(crate) backing_store_token: u64,
    pub(crate) guest_ptr: u32,
    pub(crate) byte_len: usize,
    pub(crate) guest_allocation_recyclable: bool,
    pub(crate) persistent: bool,
    pub(crate) reference_holds: u32,
    pub(crate) needs_flush: bool,
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
pub(crate) struct HostBufferLease {
    pub(crate) guest_ptr: u32,
    pub(crate) byte_offset: u32,
    pub(crate) byte_len: u32,
    pub(crate) writable: bool,
    pub(crate) host_reference_id: u32,
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
pub(crate) struct GuestBackingStoreMapping {
    pub(crate) host_addr: u64,
    pub(crate) guest_ptr: u32,
    pub(crate) byte_len: usize,
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub(crate) struct NativeBufferLease {
    pub(crate) guest_ptr: u32,
    pub(crate) host_ptr: u64,
    pub(crate) byte_len: usize,
    pub(crate) writable: bool,
    pub(crate) copied: bool,
}

// FunctionEnv is cloned for WASIX workers, while Edge's guest-side native
// globals are shared. Keep JS-backend env IDs unique across those clones.
#[cfg(all(target_arch = "wasm32", feature = "js"))]
static NEXT_NAPI_ENV_ID: AtomicU32 = AtomicU32::new(1);

#[cfg(all(target_arch = "wasm32", feature = "js"))]
fn next_js_napi_env_id() -> u32 {
    loop {
        let id = NEXT_NAPI_ENV_ID.fetch_add(1, AtomicOrdering::Relaxed);
        if id != 0 {
            return id;
        }
    }
}

/// Bookkeeping for one live V8 env's heap charge: the initial ceiling plus a
/// owned [`EnvHeapCharge`] box the near-heap-limit callback grows. Moving the
/// handle never moves its callback pointee; `None` is an unbudgeted provider.
struct EnvHeapChargeHandle {
    ceiling: u64,
    tracker: Option<Box<EnvHeapCharge>>,
}

/// An uncommitted declaration is charged without exposing it to nested releases.
#[cfg(any(test, not(all(target_arch = "wasm32", feature = "js"))))]
pub(crate) struct DeclaredExternalCharge {
    budget: Arc<ResourceBudget>,
    bytes: u64,
}

#[cfg(any(test, not(all(target_arch = "wasm32", feature = "js"))))]
impl DeclaredExternalCharge {
    pub(crate) fn reserve(budget: Arc<ResourceBudget>, bytes: u64) -> Option<Self> {
        budget.try_charge(Pool::V8External, bytes).ok()?;
        Some(Self { budget, bytes })
    }

    pub(crate) fn commit(mut self) {
        self.bytes = 0;
    }
}

#[cfg(any(test, not(all(target_arch = "wasm32", feature = "js"))))]
impl Drop for DeclaredExternalCharge {
    fn drop(&mut self) {
        let bytes = std::mem::take(&mut self.bytes);
        let released = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            self.budget.uncharge(Pool::V8External, bytes);
        }));
        if let Err(panic) = released {
            if std::thread::panicking() {
                // ResourceBudget removes its counters before calling the
                // accountant. Preserve the already-unwinding first failure.
                let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| drop(panic)));
            } else {
                std::panic::resume_unwind(panic);
            }
        }
    }
}

// No Store/T reference crosses provider disposal or an accountant callback.
// This plan is owner-thread-only, as are the physical native providers.
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
struct NativeRetiredProvider {
    state: Option<SnapiEnv>,
    // V8 is gone and bridge membership is erased, but its allocation cannot
    // be reused until the original held cell has removed its guest-id rows.
    retired_state: Option<SnapiEnv>,
    charge: Option<EnvHeapChargeHandle>,
    external: u64,
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub(crate) struct NativeRetirement {
    providers: Vec<NativeRetiredProvider>,
    registry: Arc<std::sync::Mutex<HashSet<usize>>>,
    budget: Arc<ResourceBudget>,
    leases: Vec<NativeBufferLease>,
    heap: Option<Arc<crate::guest_heap::GuestHeap>>,
    refill: Option<Arc<dyn crate::NativeGuestRefill>>,
    memory: Option<Memory>,
    table: Option<Table>,
    external: HashMap<u32, u64>,
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
impl NativeRetirement {
    pub(crate) fn dispose(&mut self) -> i32 {
        // Explicit release removes original-cell resolution rows after this
        // call and before dropping the plan's now-inert state allocation.
        self.dispose_with_mode(false)
    }
    #[cfg(test)]
    pub(crate) fn forget_retired_states_for_probe(&mut self) {
        // Only the allocation-count sensitivity uses this if its mutant has
        // already freed the shell. Never destroy a pointer disproved by it.
        for provider in &mut self.providers {
            provider.retired_state = None;
        }
    }

    fn dispose_with_mode(&mut self, cleanup: bool) -> i32 {
        struct Notification<'a> {
            registry: &'a std::sync::Mutex<HashSet<usize>>,
            state: usize,
        }
        unsafe extern "C" fn before_destroy(data: *mut c_void) {
            // Only scalar registry bookkeeping is allowed here. The CPP caller
            // holds no g_mu, and nothing unwinds through its destructor.
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let notification = unsafe { &*data.cast::<Notification<'_>>() };
                notification
                    .registry
                    .lock()
                    .unwrap_or_else(|poison| poison.into_inner())
                    .remove(&notification.state);
            }));
        }
        let mut status = 0;
        for provider in &mut self.providers {
            if let Some(state) = provider.state.take() {
                let mut notification = Notification {
                    registry: &self.registry,
                    state: state as usize,
                };
                let result = unsafe {
                    crate::snapi::snapi_bridge_finish_retirement(
                        state,
                        i32::from(cleanup),
                        before_destroy,
                        (&mut notification as *mut Notification<'_>).cast(),
                    )
                };
                // The private CPP protocol always consumes the provider,
                // including managed-admission fallback. Its inert shell stays
                // uniquely owned here across SDK wake/reentry until plan Drop.
                provider.retired_state = Some(state);
                if status == 0 {
                    status = result;
                }
            }
        }
        status
    }
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
impl NativeRetirement {
    fn finish(&mut self) {
        use std::panic::{AssertUnwindSafe, catch_unwind, resume_unwind};
        let unwinding = std::thread::panicking();
        let mut first = None;
        let mut cleanup = |f: &mut dyn FnMut()| {
            if let Err(panic) = catch_unwind(AssertUnwindSafe(f)) {
                if first.is_none() {
                    first = Some(panic);
                } else {
                    let _ = catch_unwind(AssertUnwindSafe(|| drop(panic)));
                }
            }
        };
        // Finish every provider before releasing any tracker or reservation.
        cleanup(&mut || {
            self.dispose_with_mode(true);
        });
        for provider in &mut self.providers {
            if let Some(state) = provider.retired_state.take() {
                // Host plans cleared rows before disposal; explicit release's
                // held original temporary did so before this final plan Drop.
                unsafe {
                    crate::snapi::snapi_bridge_destroy_retired_state(state);
                }
            }
            if let Some(handle) = provider.charge.take() {
                let granted = handle
                    .tracker
                    .as_ref()
                    .map_or(0, |tracker| tracker.granted.load(Ordering::Acquire));
                drop(handle.tracker);
                cleanup(&mut || self.budget.uncharge(Pool::V8HeapReserved, granted));
                cleanup(&mut || self.budget.release_env(handle.ceiling));
            }
            let bytes = std::mem::take(&mut provider.external);
            cleanup(&mut || self.budget.uncharge(Pool::V8External, bytes));
        }
        for (_, bytes) in self.external.drain() {
            cleanup(&mut || self.budget.uncharge(Pool::V8External, bytes));
        }
        for lease in self.leases.drain(..) {
            if lease.copied
                && let Some(heap) = self.heap.as_ref()
            {
                cleanup(&mut || {
                    heap.free_offset(lease.guest_ptr);
                });
            }
        }
        cleanup(&mut || drop(self.refill.take()));
        cleanup(&mut || drop(self.heap.take()));
        cleanup(&mut || {
            let _ = self.memory.take();
        });
        cleanup(&mut || {
            let _ = self.table.take();
        });
        if let Some(panic) = first {
            if unwinding {
                let _ = catch_unwind(AssertUnwindSafe(|| drop(panic)));
            } else {
                resume_unwind(panic);
            }
        }
    }
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
impl Drop for NativeRetirement {
    fn drop(&mut self) {
        if unsafe { crate::snapi::snapi_v8_allocator_active() } != 0
            && self
                .providers
                .iter()
                .any(|provider| provider.state.is_some())
        {
            // All logical rows were sealed/taken before constructing the plan.
            // Move only owned resources; the empty shell cannot run a callback.
            let plan = Self {
                providers: std::mem::take(&mut self.providers),
                registry: self.registry.clone(),
                budget: self.budget.clone(),
                leases: std::mem::take(&mut self.leases),
                heap: self.heap.take(),
                refill: self.refill.take(),
                memory: self.memory.take(),
                table: self.table.take(),
                external: std::mem::take(&mut self.external),
            };
            match crate::native_frame::defer_retirement(plan) {
                None => return,
                Some(_plan) => {
                    // The private default V8 worker pool can first enter Rust
                    // only through the covered depth-vetoed FFIs. It cannot
                    // acquire a provider-bearing Store there. Every supported
                    // foreground path owns its physical retirement holder.
                    // Destructive raw Store collection transfer is outside this
                    // profile. A violated internal custody protocol must never
                    // dispose V8 in an allocator or free its retained mapping.
                    std::process::abort();
                }
            }
        }
        crate::native_frame::host_cleanup(|| self.finish());
    }
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
#[derive(Clone, Copy, Debug)]
pub(crate) struct NativeEnvIds {
    pub(crate) env: u32,
    pub(crate) scope: u32,
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub(crate) struct NativeCreationRequest {
    ids: NativeEnvIds,
    budget: Arc<ResourceBudget>,
    max_envs: Option<usize>,
    registry: Arc<std::sync::Mutex<HashSet<usize>>>,
    stopped: Arc<AtomicBool>,
    heap: Option<Arc<crate::guest_heap::GuestHeap>>,
    refill: Option<Arc<dyn crate::NativeGuestRefill>>,
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
impl NativeCreationRequest {
    pub(crate) fn matches(&self, env: &NapiEnv) -> bool {
        env.creation_enabled()
            && Arc::ptr_eq(&self.budget, &env.budget)
            && Arc::ptr_eq(&self.registry, &env.env_registry)
            && !self.stopped.load(Ordering::Acquire)
    }

    pub(crate) fn reserve(self, requested: RequestedHeap) -> Result<NativeCreation, EnvRejected> {
        let reservation = self.budget.try_reserve_env(requested, self.max_envs)?;
        let retirement = NativeRetirement {
            providers: vec![NativeRetiredProvider {
                state: None,
                retired_state: None,
                charge: Some(EnvHeapChargeHandle {
                    ceiling: reservation.ceiling_bytes,
                    tracker: None,
                }),
                external: 0,
            }],
            registry: self.registry.clone(),
            budget: self.budget.clone(),
            leases: Vec::new(),
            heap: self.heap.clone(),
            refill: None,
            memory: None,
            table: None,
            external: HashMap::new(),
        };
        let mut creation = NativeCreation {
            request: self,
            reservation,
            retirement,
            value_limit: None,
        };
        // The reservation already has an owned Drop path before this custom
        // accountant query. No NapiEnv/Store reference is exposed to the callback.
        if creation.reservation.clamped {
            creation.value_limit = creation.request.budget.value_handle_limit();
            creation.retirement.providers[0]
                .charge
                .as_mut()
                .unwrap()
                .tracker = Some(Box::new(EnvHeapCharge {
                budget: creation.request.budget.clone(),
                env: 0,
                host_stopped: creation.request.stopped.clone(),
                unwind_slack_available: AtomicBool::new(true),
                granted: AtomicU64::new(0),
            }));
        }
        Ok(creation)
    }
}

/// Pending provider, reservation and callback box share the existing ordered
/// retirement owner. No Store, original FunctionEnvMut or T pointer is stored.
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub(crate) struct NativeCreation {
    pub(crate) request: NativeCreationRequest,
    reservation: HeapReservation,
    retirement: NativeRetirement,
    value_limit: Option<u64>,
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
impl NativeCreation {
    pub(crate) fn ids(&self) -> NativeEnvIds {
        self.request.ids
    }
    fn state(&self) -> SnapiEnv {
        self.retirement.providers[0].state.unwrap()
    }

    /// The private bridge returns an already-closed owned token. Ordinary
    /// provider entry cannot observe it before callback-free publication.
    pub(crate) fn create(&mut self, version: i32, total: u64, constrained: u64, stack: u32) -> i32 {
        let allocator = self.request.heap.as_ref().map_or(std::ptr::null(), |heap| {
            heap.make_alloc_ctx(self.request.refill.as_ref().map(Arc::downgrade))
                .cast_const()
        });
        let mut state = std::ptr::null_mut();
        let status = unsafe {
            crate::snapi::snapi_bridge_unofficial_create_env_with_options(
                version,
                total,
                constrained,
                self.reservation.max_young,
                self.reservation.max_old,
                self.reservation.code_range,
                stack,
                allocator,
                &mut state,
            )
        };
        if !state.is_null() {
            self.retirement.providers[0].state = Some(state);
        }
        if status != 0 || state.is_null() {
            return if status == 0 { 1 } else { status };
        }
        let tracker = self.retirement.providers[0]
            .charge
            .as_mut()
            .unwrap()
            .tracker
            .as_mut()
            .map_or(std::ptr::null(), |tracker| {
                tracker.env = state as usize;
                (&**tracker as *const EnvHeapCharge).cast::<c_void>()
            });
        unsafe {
            crate::snapi::snapi_bridge_prepare_created_env(
                state,
                tracker,
                self.value_limit.unwrap_or(0),
            )
        }
    }

    /// Called only after all fallible work, guest output writes and map moves
    /// succeeded. This private primitive cannot invoke Rust/JS or fail.
    pub(crate) fn publish(&mut self) {
        let state = self.state();
        unsafe {
            crate::snapi::snapi_bridge_publish_created_env(state);
        }
        self.retirement.providers[0].state = None;
        if self.request.stopped.load(Ordering::Acquire) {
            // A stop concurrent with final map publication sees this row under
            // the same registry lock; this idempotent check also covers it here.
            unsafe {
                crate::snapi::snapi_bridge_unofficial_terminate_execution(state);
            }
        }
    }
}

pub(crate) struct NapiEnv {
    /// The app's shared accountant. V8 env (isolate) heap ceilings and the
    /// `max_envs` isolate count are charged against it; workers share the same
    /// `Arc` so the app-wide budget stays honest across stores.
    pub(crate) budget: Arc<ResourceBudget>,
    /// Per-app cap on live V8 isolates (`None` = unlimited).
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) max_envs: Option<usize>,
    env_registry: Arc<std::sync::Mutex<HashSet<usize>>>,
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    host_stopped: Arc<AtomicBool>,
    /// Heap charge per live V8 env, keyed by guest env id, so teardown releases
    /// exactly what creation charged plus what the callback later granted.
    env_heap_charges: HashMap<u32, EnvHeapChargeHandle>,
    /// External memory the guest has declared via `napi_adjust_external_memory`,
    /// charged to [`Pool::V8External`]. Tracked so a negative adjustment can
    /// only release what this env actually declared, never more, and so
    /// teardown releases the remainder.
    external_declared: HashMap<u32, u64>,
    pub(crate) configured: bool,
    deactivated: bool,
    /// Depth of nested guest↔host callback crossings on this thread, bounded to
    /// keep guest→host→guest recursion from overflowing the host native stack
    /// (an uncatchable SIGSEGV). See [`NapiEnv::enter_callback`].
    #[cfg(any(test, not(all(target_arch = "wasm32", feature = "js"))))]
    callback_depth: u32,
    pub(crate) memory: Option<Memory>,
    /// Host-side allocator over the guest's linear memory. `None` only before
    /// `NapiSession::configure_instance` runs; that call fails instantiation
    /// outright if a `GuestHeap` can't be built, so any env reachable from
    /// guest code is guaranteed to have one — it is the only allocation path
    /// for guest-visible V8 memory, never a best-effort fallback.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) guest_heap: Option<Arc<crate::guest_heap::GuestHeap>>,
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) native_guest_refill: Option<Arc<dyn crate::NativeGuestRefill>>,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) malloc_fn: Option<TypedFunction<i32, i32>>,
    pub(crate) table: Option<Table>,
    pub(crate) default_napi_env_id: Option<u32>,
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) next_napi_env_id: u32,
    pub(crate) next_napi_scope_id: u32,
    pub(crate) napi_envs: HashMap<u32, usize>,
    pub(crate) napi_state_to_guest_env: HashMap<usize, u32>,
    pub(crate) napi_scopes: HashMap<u32, u32>,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) guest_buffer_pool: Vec<(u32, usize)>,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) guest_buffer_capacities: HashMap<u32, usize>,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) guest_buffer_allocated_bytes: usize,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) guest_data_ptrs: HashMap<(u32, u32), u32>,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) guest_data_backing_stores: HashMap<(u32, u64), GuestBackingStoreMapping>,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) host_buffer_copies: Vec<HostBufferCopy>,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) host_buffer_copy_frames: Vec<u64>,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) next_host_buffer_owner_id: u64,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) host_buffer_handle_scopes: Vec<(u32, u32, u64)>,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) host_buffer_reference_holds: HashMap<(u32, u32), u32>,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) next_buffer_lease_id: u32,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) host_buffer_leases: HashMap<(u32, u32), HostBufferLease>,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) persistent_callback_contexts: HashMap<u32, Box<CallbackInvocationCtx>>,
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) native_buffer_leases: HashMap<(u32, u32), NativeBufferLease>,
}

impl NapiEnv {
    pub(crate) fn new(
        budget: Arc<ResourceBudget>,
        _max_envs: Option<usize>,
        env_registry: Arc<std::sync::Mutex<HashSet<usize>>>,
        _host_stopped: Arc<AtomicBool>,
    ) -> Self {
        Self {
            budget,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            max_envs: _max_envs,
            env_registry,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            host_stopped: _host_stopped,
            env_heap_charges: HashMap::new(),
            external_declared: HashMap::new(),
            configured: false,
            deactivated: false,
            #[cfg(any(test, not(all(target_arch = "wasm32", feature = "js"))))]
            callback_depth: 0,
            memory: None,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            guest_heap: None,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            native_guest_refill: None,
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            malloc_fn: None,
            table: None,
            default_napi_env_id: None,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            next_napi_env_id: 0,
            next_napi_scope_id: 0,
            napi_envs: HashMap::new(),
            napi_state_to_guest_env: HashMap::new(),
            napi_scopes: HashMap::new(),
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            guest_buffer_pool: Vec::new(),
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            guest_buffer_capacities: HashMap::new(),
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            guest_buffer_allocated_bytes: 0,
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            guest_data_ptrs: HashMap::new(),
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            guest_data_backing_stores: HashMap::new(),
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            host_buffer_copies: Vec::new(),
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            host_buffer_copy_frames: Vec::new(),
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            next_host_buffer_owner_id: 1,
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            host_buffer_handle_scopes: Vec::new(),
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            host_buffer_reference_holds: HashMap::new(),
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            next_buffer_lease_id: 1,
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            host_buffer_leases: HashMap::new(),
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            persistent_callback_contexts: HashMap::new(),
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            native_buffer_leases: HashMap::new(),
        }
    }

    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) fn next_host_buffer_owner(&mut self) -> u64 {
        loop {
            let id = self.next_host_buffer_owner_id.max(1);
            self.next_host_buffer_owner_id = id.wrapping_add(1).max(1);
            if !self.host_buffer_copy_frames.contains(&id)
                && !self
                    .host_buffer_handle_scopes
                    .iter()
                    .any(|(_, _, owner_id)| *owner_id == id)
            {
                return id;
            }
        }
    }

    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) fn current_host_buffer_owner(&self) -> u64 {
        self.host_buffer_handle_scopes
            .last()
            .map(|(_, _, owner_id)| *owner_id)
            .or_else(|| self.host_buffer_copy_frames.last().copied())
            .unwrap_or(0)
    }

    /// Reserve nonreused positive guest ids before any accountant or provider
    /// callout. Exhaustion leaves all ownership maps unchanged.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn prepare_native_creation(&mut self) -> Option<NativeCreationRequest> {
        if !self.creation_enabled() || self.host_stopped.load(Ordering::Acquire) {
            return None;
        }
        let env = self.next_napi_env_id.max(1);
        let scope = self.next_napi_scope_id.max(1);
        if env > i32::MAX as u32 || scope > i32::MAX as u32 {
            return None;
        }
        self.next_napi_env_id = env + 1;
        self.next_napi_scope_id = scope + 1;
        Some(NativeCreationRequest {
            ids: NativeEnvIds { env, scope },
            budget: self.budget.clone(),
            max_envs: self.max_envs,
            registry: self.env_registry.clone(),
            stopped: self.host_stopped.clone(),
            heap: self.guest_heap.clone(),
            refill: self.native_guest_refill.clone(),
        })
    }

    /// Only primitive map moves occur after the final registry/stop check.
    /// CPP remains privately closed until the caller ends this T borrow and
    /// publishes the owned token; foreign terminate may already see this row.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn commit_isolate(
        &mut self,
        creation: &mut NativeCreation,
        make_default: bool,
    ) -> bool {
        if !creation.request.matches(self) {
            return false;
        }
        if self.napi_envs.try_reserve(1).is_err()
            || self.napi_state_to_guest_env.try_reserve(1).is_err()
            || self.napi_scopes.try_reserve(1).is_err()
            || self.env_heap_charges.try_reserve(1).is_err()
        {
            return false;
        }
        let mut registry = self
            .env_registry
            .lock()
            .expect("poisoned N-API env registry");
        if registry.try_reserve(1).is_err() || self.host_stopped.load(Ordering::Acquire) {
            return false;
        }
        let state = creation.state();
        let ids = creation.request.ids;
        // Checked nonreused ids cannot replace another provider's owners.
        assert!(
            !self.napi_envs.contains_key(&ids.env) && !self.napi_scopes.contains_key(&ids.scope)
        );
        self.napi_envs.insert(ids.env, state as usize);
        self.napi_state_to_guest_env.insert(state as usize, ids.env);
        self.napi_scopes.insert(ids.scope, ids.env);
        self.env_heap_charges.insert(
            ids.env,
            creation.retirement.providers[0].charge.take().unwrap(),
        );
        registry.insert(state as usize);
        if make_default {
            self.default_napi_env_id = Some(ids.env);
        }
        true
    }

    /// Record a successful provider adjustment without invoking the accountant.
    #[cfg(any(test, not(all(target_arch = "wasm32", feature = "js"))))]
    pub(crate) fn record_declared_external(&mut self, id: u32, bytes: u64) {
        let committed = self.external_declared.entry(id).or_default();
        *committed = committed
            .checked_add(bytes)
            .expect("accepted provider declaration fits its signed external counter");
    }

    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn has_declared_external(&self, id: u32, bytes: u64) -> bool {
        bytes <= self.external_declared.get(&id).copied().unwrap_or(0)
    }

    /// Take only committed declarations; the caller releases the charge after
    /// ending this borrow. A pending positive adjustment cannot be consumed.
    #[cfg(any(test, not(all(target_arch = "wasm32", feature = "js"))))]
    pub(crate) fn take_declared_external(&mut self, id: u32, bytes: u64) -> u64 {
        let Some(committed) = self.external_declared.get_mut(&id) else {
            return 0;
        };
        let release = bytes.min(*committed);
        *committed -= release;
        if *committed == 0 {
            self.external_declared.remove(&id);
        }
        release
    }

    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn host_stop_flag(&self) -> Arc<AtomicBool> {
        self.host_stopped.clone()
    }

    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn host_stopped(&self) -> bool {
        self.host_stopped.load(Ordering::Acquire)
    }

    pub(crate) fn creation_enabled(&self) -> bool {
        self.configured && !self.deactivated
    }

    /// Claim one level of guest↔host callback reentrancy. Returns `false` (the
    /// callback must be refused) once [`MAX_CALLBACK_DEPTH`] is reached, so a
    /// runaway recursion across the FFI boundary cannot overflow the host native
    /// stack. Pair every `true` with exactly one [`leave_callback`].
    ///
    /// [`MAX_CALLBACK_DEPTH`]: crate::guest::MAX_CALLBACK_DEPTH
    /// [`leave_callback`]: NapiEnv::leave_callback
    #[cfg(any(test, not(all(target_arch = "wasm32", feature = "js"))))]
    pub(crate) fn enter_callback(&mut self) -> bool {
        if self.callback_depth >= crate::guest::MAX_CALLBACK_DEPTH {
            return false;
        }
        self.callback_depth += 1;
        true
    }

    /// Release one level claimed by a `true` [`enter_callback`].
    ///
    /// [`enter_callback`]: NapiEnv::enter_callback
    #[cfg(any(test, not(all(target_arch = "wasm32", feature = "js"))))]
    pub(crate) fn leave_callback(&mut self) {
        self.callback_depth = self.callback_depth.saturating_sub(1);
    }

    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) fn register_napi_env(&mut self, env: SnapiEnv) -> (u32, u32) {
        let env_id = next_js_napi_env_id();

        let scope_id = self.next_napi_scope_id.max(1);
        self.next_napi_scope_id = scope_id.saturating_add(1);

        self.napi_envs.insert(env_id, env as usize);
        self.napi_state_to_guest_env.insert(env as usize, env_id);
        self.napi_scopes.insert(scope_id, env_id);
        (env_id, scope_id)
    }

    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    fn discard_buffer_leases_for_env(&mut self, env_id: u32) {
        #[cfg(all(target_arch = "wasm32", feature = "js"))]
        {
            let lease_ids: Vec<(u32, u32)> = self
                .host_buffer_leases
                .keys()
                .filter(|(lease_env_id, _)| *lease_env_id == env_id)
                .copied()
                .collect();
            for lease_id in lease_ids {
                let Some(lease) = self.host_buffer_leases.remove(&lease_id) else {
                    continue;
                };
                if lease.guest_ptr != 0
                    && let Some(capacity) = self.guest_buffer_capacities.remove(&lease.guest_ptr)
                {
                    self.guest_buffer_pool.push((lease.guest_ptr, capacity));
                }
            }
        }
    }

    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) fn begin_unregister_napi_scope(&mut self, scope_id: u32) -> Option<(u32, SnapiEnv)> {
        let env_id = self.napi_scopes.remove(&scope_id)?;
        if self.default_napi_env_id == Some(env_id) {
            self.default_napi_env_id = None;
        }
        let env = *self.napi_envs.get(&env_id)?;
        // Remove from the shared registry before any env- or isolate-owned
        // state is reclaimed. This synchronizes against terminate_all, which
        // holds the same mutex while calling into V8.
        self.env_registry
            .lock()
            .expect("poisoned N-API env registry")
            .remove(&env);

        // Leases are environment-owned resources. Explicit release publishes
        // writes; environment teardown only discards snapshots and returns any
        // guest allocations because the JavaScript value is being destroyed.
        self.discard_buffer_leases_for_env(env_id);
        Some((env_id, env as SnapiEnv))
    }

    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) fn finish_unregister_napi_env(&mut self, env_id: u32, env: SnapiEnv) {
        // Release the heap ceiling + any callback-granted growth + isolate slot
        // this env reserved.
        if let Some(handle) = self.env_heap_charges.remove(&env_id) {
            let granted = handle
                .tracker
                .as_ref()
                .map_or(0, |tracker| tracker.granted.load(Ordering::Acquire));
            drop(handle.tracker);
            self.budget.uncharge(Pool::V8HeapReserved, granted);
            self.budget.release_env(handle.ceiling);
        }
        self.napi_envs.remove(&env_id);
        #[cfg(all(target_arch = "wasm32", feature = "js"))]
        unsafe {
            snapi_bridge_swap_active_callback_ctx(env, std::ptr::null_mut());
            self.persistent_callback_contexts.remove(&env_id);
        }
        self.napi_state_to_guest_env.remove(&(env as usize));
    }

    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) fn unregister_napi_scope(&mut self, scope_id: u32) -> Option<SnapiEnv> {
        let (env_id, env) = self.begin_unregister_napi_scope(scope_id)?;
        self.finish_unregister_napi_env(env_id, env);
        Some(env)
    }

    pub(crate) fn resolve_napi_env(&self, guest_env: i32) -> SnapiEnv {
        let env_id = if guest_env > 0 {
            guest_env as u32
        } else {
            return std::ptr::null_mut();
        };
        self.napi_envs
            .get(&env_id)
            .map(|env| *env as SnapiEnv)
            .unwrap_or(std::ptr::null_mut())
    }

    /// Preallocate and atomically acquire every CPP token before changing ids.
    /// Between the successful begin and publication only callback-free moves
    /// into preallocated storage are permitted.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn take_native_retirement(&mut self, scope: Option<u32>) -> Option<NativeRetirement> {
        let mut ids: Vec<u32> = match scope {
            Some(scope) => vec![*self.napi_scopes.get(&scope)?],
            None => self.napi_envs.keys().copied().collect(),
        };
        ids.sort_unstable_by(|a, b| b.cmp(a));
        let states: Vec<_> = ids
            .iter()
            .map(|id| self.napi_envs[id] as SnapiEnv)
            .collect();
        let lease_ids: Vec<_> = self
            .native_buffer_leases
            .keys()
            .filter(|(id, _)| scope.is_none() && ids.contains(id))
            .copied()
            .collect();
        let mut retirement = NativeRetirement {
            providers: ids
                .iter()
                .map(|_| NativeRetiredProvider {
                    state: None,
                    retired_state: None,
                    charge: None,
                    external: 0,
                })
                .collect(),
            registry: self.env_registry.clone(),
            budget: self.budget.clone(),
            leases: Vec::with_capacity(lease_ids.len()),
            heap: self.guest_heap.clone(),
            refill: None,
            memory: None,
            table: None,
            external: HashMap::new(),
        };
        if unsafe { crate::snapi::snapi_bridge_begin_retirement(states.as_ptr(), states.len()) }
            != 0
        {
            return None;
        }
        // All allocations and validation preceded begin. There are no user
        // destructors/callbacks/awaits between this point and the owned plan.
        for ((id, state), provider) in ids.iter().zip(states).zip(&mut retirement.providers) {
            provider.state = Some(state);
            provider.charge = self.env_heap_charges.remove(id);
            if scope.is_none() {
                provider.external = self.external_declared.remove(id).unwrap_or(0);
            }
        }
        for key in lease_ids {
            if let Some(lease) = self.native_buffer_leases.remove(&key) {
                retirement.leases.push(lease);
            }
        }
        if let Some(scope) = scope {
            let id = self.napi_scopes.remove(&scope);
            if self.default_napi_env_id == id {
                self.default_napi_env_id = None;
            }
            // Keep env mapping available to already-entered explicit finalizers.
        } else {
            self.configured = false;
            self.deactivated = true;
            self.default_napi_env_id = None;
            self.napi_scopes.clear();
            self.napi_envs.clear();
            self.napi_state_to_guest_env.clear();
            retirement.external = std::mem::take(&mut self.external_declared);
            retirement.heap = self.guest_heap.take();
            retirement.refill = self.native_guest_refill.take();
            retirement.memory = self.memory.take();
            retirement.table = self.table.take();
        }
        Some(retirement)
    }

    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn take_native_scope(&mut self, scope: u32) -> Option<NativeRetirement> {
        self.take_native_retirement(Some(scope))
    }

    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn finish_native_scope(
        &mut self,
        id: u32,
        state: SnapiEnv,
        retirement: &mut NativeRetirement,
    ) {
        // Explicit finalizers may adjust declarations and acquire new leases.
        // Extract the FINAL ownership from the same original cell after CPP,
        // before invoking any accountant or guest-heap release callback.
        retirement.providers[0].external = self.external_declared.remove(&id).unwrap_or(0);
        let keys: Vec<_> = self
            .native_buffer_leases
            .keys()
            .filter(|(env, _)| *env == id)
            .copied()
            .collect();
        for key in keys {
            if let Some(lease) = self.native_buffer_leases.remove(&key) {
                retirement.leases.push(lease);
            }
        }
        self.napi_envs.remove(&id);
        self.napi_state_to_guest_env.remove(&(state as usize));
    }

    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn take_native_deactivation(&mut self) -> Option<NativeRetirement> {
        if self.deactivated {
            return None;
        }
        Some(
            self.take_native_retirement(None)
                .expect("cannot deactivate an N-API session with active provider frames"),
        )
    }

    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) fn deactivate(&mut self) {
        if self.deactivated {
            return;
        }
        for env in self.napi_envs.values() {
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            let ready = unsafe { crate::snapi_js::can_deactivate(*env as SnapiEnv) };
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            let ready = unsafe {
                crate::snapi::snapi_bridge_unofficial_can_release_env(*env as SnapiEnv) == 1
            };
            assert!(
                ready,
                "cannot deactivate an N-API session with active provider frames"
            );
        }
        self.deactivated = true;
        self.configured = false;
        #[cfg(all(target_arch = "wasm32", feature = "js"))]
        for env in self.napi_envs.values() {
            unsafe { crate::snapi_js::retire_queued_callbacks(*env as SnapiEnv) };
        }
        let mut scope_ids: Vec<u32> = self.napi_scopes.keys().copied().collect();
        scope_ids.sort_unstable_by(|left, right| right.cmp(left));
        for scope_id in scope_ids {
            if let Some(env) = self
                .napi_scopes
                .get(&scope_id)
                .and_then(|env_id| self.napi_envs.get(env_id))
            {
                // Retirement is host-only: provider disposal must not reenter
                // guest finalizers through a previously installed context.
                unsafe {
                    crate::snapi::snapi_bridge_swap_active_callback_ctx(
                        *env as SnapiEnv,
                        std::ptr::null_mut(),
                    );
                }
            }
            if let Some(env) = self.unregister_napi_scope(scope_id) {
                unsafe {
                    let _ = snapi_bridge_unofficial_release_env(env);
                }
            }
        }
        // Release any external memory the guest declared but did not take back.
        for (_, bytes) in self.external_declared.drain() {
            self.budget.uncharge(Pool::V8External, bytes);
        }
        self.default_napi_env_id = None;
        self.memory = None;
        self.table = None;
        #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
        {
            self.native_buffer_leases.clear();
            self.guest_heap = None;
        }
        #[cfg(all(target_arch = "wasm32", feature = "js"))]
        {
            self.malloc_fn = None;
            self.persistent_callback_contexts.clear();
            self.guest_buffer_pool.clear();
            self.guest_buffer_capacities.clear();
            self.guest_buffer_allocated_bytes = 0;
            self.guest_data_ptrs.clear();
            self.guest_data_backing_stores.clear();
            self.host_buffer_copies.clear();
            self.host_buffer_copy_frames.clear();
            self.host_buffer_handle_scopes.clear();
            self.host_buffer_reference_holds.clear();
            self.host_buffer_leases.clear();
        }
    }
}

impl Drop for NapiEnv {
    fn drop(&mut self) {
        #[cfg(all(target_arch = "wasm32", feature = "js"))]
        self.deactivate();
        #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
        drop(self.take_native_deactivation());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MIB: u64 = 1024 * 1024;

    #[test]
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn native_creation_ids_exhaust_without_reusing_or_publishing() {
        let budget = ResourceBudget::unlimited();
        let mut env = NapiEnv::new(
            budget.clone(),
            None,
            Arc::new(std::sync::Mutex::new(HashSet::new())),
            Arc::new(AtomicBool::new(false)),
        );
        env.configured = true;
        env.next_napi_env_id = i32::MAX as u32;
        env.next_napi_scope_id = i32::MAX as u32;
        let request = env.prepare_native_creation().unwrap();
        assert_eq!(request.ids.env, i32::MAX as u32);
        assert_eq!(request.ids.scope, i32::MAX as u32);
        assert!(env.prepare_native_creation().is_none());
        assert_eq!(env.next_napi_env_id, i32::MAX as u32 + 1);
        assert_eq!(env.next_napi_scope_id, i32::MAX as u32 + 1);
        env.next_napi_env_id = 1;
        assert!(
            env.prepare_native_creation().is_none(),
            "scope exhaustion is independent"
        );
        assert!(env.napi_envs.is_empty());
        assert!(env.napi_scopes.is_empty());
        assert!(env.env_heap_charges.is_empty());
        assert_eq!(budget.snapshot().live_isolates, 0);
        assert_eq!(budget.snapshot().v8_heap_reserved, 0);
    }

    #[test]
    fn declared_external_charges_denies_and_clamps() {
        let budget = ResourceBudget::with_memory_limit(10 * MIB);
        let mut env = NapiEnv::new(
            Arc::clone(&budget),
            None,
            Arc::new(std::sync::Mutex::new(HashSet::new())),
            Arc::new(AtomicBool::new(false)),
        );

        let charge = DeclaredExternalCharge::reserve(budget.clone(), 6 * MIB).unwrap();
        env.record_declared_external(1, 6 * MIB);
        charge.commit();
        assert_eq!(budget.snapshot().v8_external, 6 * MIB);

        // Over budget: denied, nothing charged.
        assert!(DeclaredExternalCharge::reserve(budget.clone(), 6 * MIB).is_none());
        assert_eq!(budget.snapshot().v8_external, 6 * MIB);

        // A negative delta larger than declared is clamped to what was declared,
        // so it can never underflow the pool.
        let release = env.take_declared_external(1, 100 * MIB);
        budget.uncharge(Pool::V8External, release);
        assert_eq!(budget.snapshot().v8_external, 0);
        assert!(env.external_declared.is_empty());

        // Further release is a no-op.
        let release = env.take_declared_external(1, MIB);
        budget.uncharge(Pool::V8External, release);
        assert_eq!(budget.snapshot().v8_external, 0);
    }

    #[test]
    fn declared_external_released_on_drop() {
        let budget = ResourceBudget::with_memory_limit(10 * MIB);
        {
            let mut env = NapiEnv::new(
                Arc::clone(&budget),
                None,
                Arc::new(std::sync::Mutex::new(HashSet::new())),
                Arc::new(AtomicBool::new(false)),
            );
            let charge = DeclaredExternalCharge::reserve(budget.clone(), 4 * MIB).unwrap();
            env.record_declared_external(1, 4 * MIB);
            charge.commit();
            assert_eq!(budget.snapshot().v8_external, 4 * MIB);
        }
        assert_eq!(
            budget.snapshot().v8_external,
            0,
            "drop releases declared external"
        );
    }

    #[test]
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn external_rollback_preserves_the_first_failure_and_releases_once() {
        use crate::budget::NapiMemoryAccountant;
        use std::panic::{AssertUnwindSafe, catch_unwind, panic_any};
        struct PanickingAccountant(AtomicU64);
        impl NapiMemoryAccountant for PanickingAccountant {
            fn memory_limit(&self) -> u64 {
                u64::MAX
            }
            fn memory_charged(&self) -> u64 {
                self.0.load(Ordering::SeqCst)
            }
            fn try_charge(&self, bytes: u64) -> bool {
                self.0.fetch_add(bytes, Ordering::SeqCst);
                true
            }
            fn uncharge(&self, bytes: u64) {
                self.0.fetch_sub(bytes, Ordering::SeqCst);
                panic_any("secondary accountant panic");
            }
        }
        #[derive(Debug)]
        struct Original(Arc<()>);
        impl std::fmt::Display for Original {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                f.write_str("original adjustment failure")
            }
        }
        impl std::error::Error for Original {}
        let accountant = Arc::new(PanickingAccountant(AtomicU64::new(0)));
        let budget = ResourceBudget::with_accountant(accountant.clone());
        let original = Arc::new(());
        // Exercise the exact owned-result reconciliation used after CPP exit;
        // this unit proof does not fabricate execution of a V8 callback.
        let charge = DeclaredExternalCharge::reserve(budget.clone(), 64).unwrap();
        let first = wasmer::RuntimeError::user(Box::new(Original(original.clone())));
        let error = crate::guest::napi::settle_external_adjustment(
            Some(charge),
            &budget,
            1,
            0,
            Ok(Err(first)),
        )
        .unwrap_err();
        assert!(Arc::ptr_eq(
            &error.downcast_ref::<Original>().unwrap().0,
            &original
        ));
        assert_eq!(budget.snapshot().v8_external, 0);
        assert_eq!(accountant.memory_charged(), 0);

        let charge = DeclaredExternalCharge::reserve(budget.clone(), 64).unwrap();
        let payload = original.clone();
        let panic = catch_unwind(AssertUnwindSafe(|| {
            crate::guest::napi::settle_external_adjustment(
                Some(charge),
                &budget,
                1,
                0,
                Err(Box::new(Original(payload))),
            )
            .unwrap();
        }))
        .unwrap_err();
        assert!(Arc::ptr_eq(
            &panic.downcast_ref::<Original>().unwrap().0,
            &original
        ));
        assert_eq!(budget.snapshot().v8_external, 0);
        assert_eq!(accountant.memory_charged(), 0);

        let payload = original.clone();
        let panic = catch_unwind(AssertUnwindSafe(|| {
            let _charge = DeclaredExternalCharge::reserve(budget.clone(), 64).unwrap();
            panic_any(Original(payload));
        }))
        .unwrap_err();
        assert!(Arc::ptr_eq(
            &panic.downcast_ref::<Original>().unwrap().0,
            &original
        ));
        assert_eq!(budget.snapshot().v8_external, 0);
        assert_eq!(accountant.memory_charged(), 0);
    }

    #[test]
    fn callback_reentrancy_is_bounded() {
        let mut env = NapiEnv::new(
            ResourceBudget::unlimited(),
            None,
            Arc::new(std::sync::Mutex::new(HashSet::new())),
            Arc::new(AtomicBool::new(false)),
        );
        let max = crate::guest::MAX_CALLBACK_DEPTH;

        // Reentrancy is allowed up to the limit, then refused.
        for _ in 0..max {
            assert!(env.enter_callback());
        }
        assert!(
            !env.enter_callback(),
            "past the limit the callback is refused"
        );

        // Unwinding one level frees exactly one slot.
        env.leave_callback();
        assert!(env.enter_callback());

        // Fully unwind; an extra leave saturates instead of underflowing.
        for _ in 0..max {
            env.leave_callback();
        }
        env.leave_callback();
        assert!(env.enter_callback());
    }
}
