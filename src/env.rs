use std::collections::{HashMap, HashSet};
use std::ffi::c_void;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};

#[cfg(all(target_arch = "wasm32", feature = "js"))]
use wasmer::TypedFunction;
use wasmer::{Function, Memory, Table};

use crate::budget::{
    EnvHeapCharge, EnvRejected, HeapReservation, Pool, RequestedHeap, ResourceBudget,
};
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
use crate::lane::{ManagedV8Lane, ManagedV8LaneActivator, ManagedV8LaneScope};
use crate::message::PendingMessages;
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
use crate::snapi::snapi_bridge_unofficial_env_alive;
use crate::snapi::{
    SnapiEnv, snapi_bridge_unofficial_release_env, snapi_bridge_unofficial_set_host_budget,
};

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
    pub(crate) covers_full_backing_store: bool,
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
fn next_js_napi_env_id() -> Option<u32> {
    NEXT_NAPI_ENV_ID
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |id| {
            (id <= i32::MAX as u32).then_some(id + 1)
        })
        .ok()
}

/// Bookkeeping for one live V8 env's heap charge: the initial ceiling plus a
/// pointer (as `usize`, so [`NapiEnv`] stays `Send`) to the boxed
/// [`EnvHeapCharge`] the near-heap-limit callback grows. `tracker == 0` means
/// the env is unbudgeted (unlimited).
struct EnvHeapChargeHandle {
    ceiling: u64,
    tracker: usize,
}

pub(crate) struct NapiEnv {
    /// The app's shared accountant. V8 env (isolate) heap ceilings and the
    /// `max_envs` isolate count are charged against it; workers share the same
    /// `Arc` so the app-wide budget stays honest across stores.
    pub(crate) budget: Arc<ResourceBudget>,
    pub(crate) pending_messages: Arc<PendingMessages>,
    /// Per-app cap on live V8 isolates (`None` = unlimited).
    pub(crate) max_envs: Option<usize>,
    /// Holds the import-session admission slot for as long as this store owns
    /// its host-function environment, even after import setup has returned.
    pub(crate) session_lease: Option<Arc<crate::ctx::SessionLease>>,
    env_registry: Arc<std::sync::Mutex<HashSet<usize>>>,
    host_stopped: Arc<AtomicBool>,
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    managed_lane_activator: Option<ManagedV8LaneActivator>,
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    managed_lane: Option<Arc<ManagedV8Lane>>,
    /// Heap charge per live V8 env, keyed by guest env id, so teardown releases
    /// exactly what creation charged plus what the callback later granted.
    env_heap_charges: HashMap<u32, EnvHeapChargeHandle>,
    /// Single-entry cache for [`NapiEnv::heap_tracker_for`]: the last native
    /// env asked about and its tracker pointer (`0` for an unlimited budget).
    /// JS entries overwhelmingly come from one env in a row, and the hook runs
    /// on every entry, so two hash lookups per entry are avoided. Invalidated
    /// when an env is unregistered.
    heap_settle_cache: (usize, usize),
    /// External memory the guest has declared via `napi_adjust_external_memory`,
    /// charged to [`Pool::V8External`]. Tracked so a negative adjustment can
    /// only release what this env actually declared, never more, and so
    /// teardown releases the remainder.
    external_declared: u64,
    /// Depth of nested guest↔host callback crossings on this thread, bounded to
    /// keep guest→host→guest recursion from overflowing the host native stack
    /// (an uncatchable SIGSEGV). See [`NapiEnv::enter_callback`].
    callback_depth: u32,
    pub(crate) memory: Option<Memory>,
    /// Host-side allocator over the guest's linear memory. Created on first
    /// environment use, so importing N-API alone does not grow guest memory.
    /// Environment creation fails if the heap cannot be built; once created,
    /// this is the only allocation path for guest-visible V8 memory.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) guest_heap: Option<Arc<crate::guest_heap::GuestHeap>>,
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) guest_heap_registration: Option<u64>,
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    pub(crate) malloc_fn: Option<TypedFunction<i32, i32>>,
    pub(crate) table: Option<Table>,
    /// Cache of resolved guest callback functions, keyed by their
    /// `__indirect_function_table` index. `Function::from_vm_funcref` (invoked
    /// by `Table::get`) unconditionally appends a new entry to the store's
    /// function arena on every call with no dedup, so re-resolving the same
    /// `wasm_fn_ptr` on every guest→host→guest callback invocation leaks
    /// memory unboundedly. `Function` clones are cheap (a store handle), so
    /// caching by table index and cloning on repeat hits avoids the
    /// unconditional store growth. See `guest::callback::call_guest_callback`.
    pub(crate) func_cache: HashMap<u32, Function>,
    pub(crate) default_napi_env_id: Option<u32>,
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    next_native_env_id: Arc<AtomicU32>,
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
        pending_messages: Arc<PendingMessages>,
        max_envs: Option<usize>,
        env_registry: Arc<std::sync::Mutex<HashSet<usize>>>,
        host_stopped: Arc<AtomicBool>,
        #[cfg(not(all(target_arch = "wasm32", feature = "js")))] next_native_env_id: Arc<AtomicU32>,
        #[cfg(not(all(target_arch = "wasm32", feature = "js")))] managed_lane_activator: Option<
            ManagedV8LaneActivator,
        >,
    ) -> Self {
        Self {
            budget,
            pending_messages,
            max_envs,
            session_lease: None,
            env_registry,
            host_stopped,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            managed_lane_activator,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            managed_lane: None,
            env_heap_charges: HashMap::new(),
            heap_settle_cache: (0, 0),
            external_declared: 0,
            callback_depth: 0,
            memory: None,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            guest_heap: None,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            guest_heap_registration: None,
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            malloc_fn: None,
            table: None,
            func_cache: HashMap::new(),
            default_napi_env_id: None,
            next_napi_scope_id: 1,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            next_native_env_id,
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

    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn enter_background_lane(&mut self) -> anyhow::Result<Option<ManagedV8LaneScope>> {
        if self.host_stopped.load(Ordering::Acquire) {
            anyhow::bail!("N-API instance has been stopped");
        }
        let Some(activate) = &self.managed_lane_activator else {
            return Ok(None);
        };
        if self.managed_lane.is_none() {
            let lane = activate()?;
            // Activation can race a host stop. Edge rejects late activation,
            // and the provider rechecks its own sticky stop flag before V8.
            if self.host_stopped.load(Ordering::Acquire) {
                anyhow::bail!("N-API instance stopped during activation");
            }
            self.managed_lane = Some(lane);
        }
        Ok(self.managed_lane.as_ref().map(ManagedV8Lane::enter))
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

    /// Reserve budget for a new V8 env before creating it: acquire an isolate
    /// slot against `max_envs` and charge its (clamped) heap ceiling. The
    /// returned constraints must be forwarded to V8. Follow with exactly one of
    /// [`commit_isolate`] (on success) or [`abort_isolate`] (on failure).
    ///
    /// [`commit_isolate`]: NapiEnv::commit_isolate
    /// [`abort_isolate`]: NapiEnv::abort_isolate
    pub(crate) fn reserve_isolate(
        &self,
        requested: RequestedHeap,
    ) -> Result<HeapReservation, EnvRejected> {
        self.budget.try_reserve_env(requested, self.max_envs)
    }

    /// Register a successfully-created env, attach its heap charge, and — under
    /// a limited budget — install the host-owned budget tracker so V8 heap
    /// growth and the bridge's per-handle bookkeeping for this isolate are
    /// charged against the budget. Teardown releases the initial ceiling and
    /// everything granted since.
    pub(crate) fn commit_isolate(
        &mut self,
        env: SnapiEnv,
        reservation: &HeapReservation,
    ) -> Option<(u32, u32)> {
        let (env_id, scope_id) = self.register_napi_env(env)?;
        {
            let mut registry = self
                .env_registry
                .lock()
                .expect("poisoned N-API env registry");
            registry.insert(env as usize);

            // If the host already stopped this app's JS, an isolate created
            // afterwards must not start running either. Checking under the
            // registry lock closes the race against a concurrent
            // `NapiRuntimeControl::terminate_all`, which sets the flag before
            // taking this lock: either it sees this env in the registry, or
            // we see its flag here.
            if self.host_stopped.load(Ordering::Acquire) {
                // SAFETY: `env` is the isolate just created and is still live.
                unsafe {
                    crate::snapi::snapi_bridge_unofficial_terminate_execution(env);
                }
            }
        }

        let tracker = if reservation.clamped {
            let boxed = Box::into_raw(Box::new(EnvHeapCharge::new(
                Arc::clone(&self.budget),
                env as usize,
                Arc::clone(&self.host_stopped),
                u64::from(reservation.max_old),
            )));
            // SAFETY: `env` is the isolate just created; `boxed` outlives the
            // bridge's hooks (freed only at this env's teardown, below).
            unsafe {
                snapi_bridge_unofficial_set_host_budget(env, boxed as *const c_void);
            }
            boxed as usize
        } else {
            0
        };

        self.env_heap_charges.insert(
            env_id,
            EnvHeapChargeHandle {
                ceiling: reservation.ceiling_bytes,
                tracker,
            },
        );
        Some((env_id, scope_id))
    }

    /// The heap budget tracker of a live budgeted env, or `None` for an env
    /// under an unlimited budget (which has no heap limit to settle).
    pub(crate) fn heap_tracker_for(&mut self, env: SnapiEnv) -> Option<&EnvHeapCharge> {
        let key = env as usize;
        let tracker = if self.heap_settle_cache.0 == key && key != 0 {
            self.heap_settle_cache.1
        } else {
            let tracker = self
                .napi_state_to_guest_env
                .get(&key)
                .and_then(|env_id| self.env_heap_charges.get(env_id))
                .map_or(0, |handle| handle.tracker);
            self.heap_settle_cache = (key, tracker);
            tracker
        };
        if tracker == 0 {
            return None;
        }
        // SAFETY: the box lives until `finish_unregister_napi_env` removes the
        // handle (and clears this cache), which only happens while no JS runs
        // on this env.
        Some(unsafe { &*(tracker as *const EnvHeapCharge) })
    }

    /// Release a reservation whose env creation failed after [`reserve_isolate`].
    ///
    /// [`reserve_isolate`]: NapiEnv::reserve_isolate
    pub(crate) fn abort_isolate(&self, reservation: &HeapReservation) {
        self.budget.release_env(reservation.ceiling_bytes);
    }

    /// Charge a positive `napi_adjust_external_memory` delta against the budget.
    /// Returns `false` (guest sees a failure) when it would exceed the budget.
    pub(crate) fn charge_declared_external(&mut self, bytes: u64) -> bool {
        if self.budget.try_charge(Pool::V8External, bytes).is_err() {
            return false;
        }
        self.external_declared = self.external_declared.saturating_add(bytes);
        true
    }

    /// Release a negative `napi_adjust_external_memory` delta, clamped to what
    /// this env actually declared so it can never underflow the pool or release
    /// the allocator's charges.
    pub(crate) fn uncharge_declared_external(&mut self, bytes: u64) {
        let release = bytes.min(self.external_declared);
        self.external_declared -= release;
        self.budget.uncharge(Pool::V8External, release);
    }

    pub(crate) fn host_stopped(&self) -> bool {
        self.host_stopped.load(Ordering::Acquire)
    }

    pub(crate) fn terminate_all(&self) {
        self.host_stopped.store(true, Ordering::Release);
        let envs = self
            .env_registry
            .lock()
            .expect("poisoned N-API env registry");
        for env in envs.iter().copied() {
            unsafe {
                crate::snapi::snapi_bridge_unofficial_terminate_execution(env as SnapiEnv);
            }
        }
        drop(envs);
        self.pending_messages.close_and_clear();
    }

    /// Serialize guest cancellation with host termination. The host sets its
    /// sticky flag before taking this registry lock and terminates every live
    /// isolate while holding it. If a kill races this call, either cancellation
    /// is refused or the kill's termination happens after cancellation.
    pub(crate) fn cancel_guest_termination(&self, env: SnapiEnv) -> i32 {
        let _registry = self
            .env_registry
            .lock()
            .expect("poisoned N-API env registry");
        if self.host_stopped() {
            if !env.is_null() {
                unsafe { crate::snapi::snapi_bridge_unofficial_terminate_execution(env) };
            }
            return 1;
        }
        if env.is_null() {
            return 1;
        }
        unsafe { crate::snapi::snapi_bridge_unofficial_cancel_terminate_execution(env) }
    }

    /// Claim one level of guest↔host callback reentrancy. Returns `false` (the
    /// callback must be refused) once [`MAX_CALLBACK_DEPTH`] is reached, so a
    /// runaway recursion across the FFI boundary cannot overflow the host native
    /// stack. Pair every `true` with exactly one [`leave_callback`].
    ///
    /// [`MAX_CALLBACK_DEPTH`]: crate::guest::MAX_CALLBACK_DEPTH
    /// [`leave_callback`]: NapiEnv::leave_callback
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
    pub(crate) fn leave_callback(&mut self) {
        self.callback_depth = self.callback_depth.saturating_sub(1);
    }

    pub(crate) fn in_callback(&self) -> bool {
        self.callback_depth != 0
    }

    pub(crate) fn scope_env(&self, scope_id: u32) -> Option<(u32, SnapiEnv)> {
        let env_id = *self.napi_scopes.get(&scope_id)?;
        let env = *self.napi_envs.get(&env_id)? as SnapiEnv;
        Some((env_id, env))
    }

    pub(crate) fn register_napi_env(&mut self, env: SnapiEnv) -> Option<(u32, u32)> {
        if self.next_napi_scope_id > i32::MAX as u32 {
            return None;
        }
        #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
        let env_id = self
            .next_native_env_id
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |id| {
                (id <= i32::MAX as u32).then_some(id + 1)
            })
            .ok()?;
        #[cfg(all(target_arch = "wasm32", feature = "js"))]
        let env_id = next_js_napi_env_id()?;

        let scope_id = self.next_napi_scope_id;
        self.next_napi_scope_id += 1;

        self.napi_envs.insert(env_id, env as usize);
        self.napi_state_to_guest_env.insert(env as usize, env_id);
        self.napi_scopes.insert(scope_id, env_id);
        Some((env_id, scope_id))
    }

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

        #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
        {
            let lease_ids: Vec<(u32, u32)> = self
                .native_buffer_leases
                .keys()
                .filter(|(lease_env_id, _)| *lease_env_id == env_id)
                .copied()
                .collect();
            for lease_id in lease_ids {
                let Some(lease) = self.native_buffer_leases.remove(&lease_id) else {
                    continue;
                };
                if lease.copied
                    && let Some(heap) = self.guest_heap.as_ref()
                {
                    heap.free_offset(lease.guest_ptr);
                }
            }
        }
    }

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

    pub(crate) fn finish_unregister_napi_env(&mut self, env_id: u32, env: SnapiEnv) {
        // Release the heap ceiling + any granted growth and bookkeeping +
        // isolate slot this env reserved.
        if let Some(handle) = self.env_heap_charges.remove(&env_id) {
            if handle.tracker != 0 {
                // SAFETY: reclaim the box created in `commit_isolate`. The
                // native release has already removed the callback and disposed
                // the isolate, so it cannot call through this pointer again.
                let boxed = unsafe { Box::from_raw(handle.tracker as *mut EnvHeapCharge) };
                self.budget
                    .uncharge(Pool::V8HeapReserved, boxed.granted.load(Ordering::Acquire));
                self.budget.uncharge(
                    Pool::HostBookkeeping,
                    boxed.bookkeeping_granted.load(Ordering::Acquire),
                );
                self.budget
                    .release_heap_emergency(boxed.emergency_exposed.load(Ordering::Acquire));
            }
            self.budget.release_env(handle.ceiling);
        }
        self.napi_envs.remove(&env_id);
        #[cfg(all(target_arch = "wasm32", feature = "js"))]
        unsafe {
            snapi_bridge_swap_active_callback_ctx(env, std::ptr::null_mut());
            self.persistent_callback_contexts.remove(&env_id);
        }
        self.napi_state_to_guest_env.remove(&(env as usize));
        if self.heap_settle_cache.0 == env as usize {
            self.heap_settle_cache = (0, 0);
        }
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

    fn release_registered_envs(&mut self, mut release: impl FnMut(SnapiEnv) -> bool) -> bool {
        let mut all_quiesced = true;
        let scope_ids: Vec<u32> = self.napi_scopes.keys().copied().collect();
        for scope_id in scope_ids {
            let Some((env_id, env)) = self.begin_unregister_napi_scope(scope_id) else {
                continue;
            };
            // Native release can wait for V8 background work and finalizers.
            // Keep the isolate's full reservation in the shared accountant
            // until that work is quiescent; another WASIX worker may try to
            // create an environment concurrently.
            if release(env) {
                self.finish_unregister_napi_env(env_id, env);
            } else {
                all_quiesced = false;
                // A failed native release has not proved that the isolate is
                // gone. Retain the charge and heap-limit callback backing
                // rather than making its bytes available to a sibling.
                eprintln!("[wasmer-napi] env {env_id} release failed during drop; retaining quota");
                self.host_stopped.store(true, Ordering::Release);
                self.env_registry
                    .lock()
                    .expect("poisoned N-API env registry")
                    .insert(env as usize);
                #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
                unsafe {
                    crate::snapi::snapi_bridge_unofficial_terminate_execution(env);
                }
                std::mem::forget(Arc::clone(&self.budget));
            }
        }
        all_quiesced
    }
}

impl Drop for NapiEnv {
    fn drop(&mut self) {
        let all_quiesced = self.release_registered_envs(|env| {
            let status = unsafe { snapi_bridge_unofficial_release_env(env) };
            if status == 0 {
                return true;
            }
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            {
                // A finalizer error can be returned after complete native
                // disposal. Only a still-registered bridge requires keeping
                // the reservation charged.
                unsafe { snapi_bridge_unofficial_env_alive(env) == 0 }
            }
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            false
        });
        // Release any external memory the guest declared but did not take back.
        if all_quiesced && self.external_declared > 0 {
            self.budget
                .uncharge(Pool::V8External, self.external_declared);
            self.external_declared = 0;
        }
        if !all_quiesced {
            // A still-registered native env keeps a raw pointer to its V8
            // queue. Preserve that owner and its session admission slot along
            // with the quota retained above; dropping either would let later
            // work touch freed memory or admit another live session.
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            if let Some(lane) = self.managed_lane.take() {
                std::mem::forget(lane);
            }
            if let Some(lease) = self.session_lease.take() {
                std::mem::forget(lease);
            }
        }
        #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
        if let (Some(heap), Some(registration)) =
            (&self.guest_heap, self.guest_heap_registration.take())
        {
            heap.unregister_memory(registration);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(not(target_arch = "wasm32"))]
    use std::{sync::mpsc, thread};

    const MIB: u64 = 1024 * 1024;

    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    #[test]
    fn native_env_ids_are_unique_across_concurrent_instance_sessions() {
        let next_id = Arc::new(AtomicU32::new(1));
        let budget = ResourceBudget::unlimited();
        let pending = PendingMessages::new();
        let registry = Arc::new(std::sync::Mutex::new(HashSet::new()));
        let stopped = Arc::new(AtomicBool::new(false));
        let barrier = Arc::new(std::sync::Barrier::new(24));
        let mut joins = Vec::new();
        for index in 0..24usize {
            let next_id = Arc::clone(&next_id);
            let budget = Arc::clone(&budget);
            let pending = Arc::clone(&pending);
            let registry = Arc::clone(&registry);
            let stopped = Arc::clone(&stopped);
            let barrier = Arc::clone(&barrier);
            joins.push(std::thread::spawn(move || {
                let mut session =
                    NapiEnv::new(budget, pending, None, registry, stopped, next_id, None);
                barrier.wait();
                let (id, _) = session.register_napi_env((index + 1) as SnapiEnv).unwrap();
                // The fake pointer only exercises ID allocation; never pass it
                // through the native release path in NapiEnv::drop.
                session.napi_envs.clear();
                session.napi_state_to_guest_env.clear();
                session.napi_scopes.clear();
                id
            }));
        }
        let mut ids: Vec<u32> = joins.into_iter().map(|join| join.join().unwrap()).collect();
        ids.sort_unstable();
        assert_eq!(ids, (1..=24).collect::<Vec<_>>());
        assert_eq!(next_id.load(Ordering::Relaxed), 25);
    }

    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    #[test]
    fn native_env_ids_reject_exhaustion_without_reuse() {
        let next_id = Arc::new(AtomicU32::new(i32::MAX as u32));
        let mut session = NapiEnv::new(
            ResourceBudget::unlimited(),
            PendingMessages::new(),
            None,
            Arc::new(std::sync::Mutex::new(HashSet::new())),
            Arc::new(AtomicBool::new(false)),
            Arc::clone(&next_id),
            None,
        );
        assert_eq!(
            session.register_napi_env(1usize as SnapiEnv).unwrap().0,
            i32::MAX as u32
        );
        assert!(session.register_napi_env(2usize as SnapiEnv).is_none());
        session.napi_envs.clear();
        session.napi_state_to_guest_env.clear();
        session.napi_scopes.clear();
    }

    #[test]
    fn declared_external_charges_denies_and_clamps() {
        let budget = ResourceBudget::with_memory_limit(10 * MIB);
        let mut env = NapiEnv::new(
            Arc::clone(&budget),
            PendingMessages::new(),
            None,
            Arc::new(std::sync::Mutex::new(HashSet::new())),
            Arc::new(AtomicBool::new(false)),
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            Arc::new(AtomicU32::new(1)),
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            None,
        );

        assert!(env.charge_declared_external(6 * MIB));
        assert_eq!(budget.snapshot().v8_external, 6 * MIB);

        // Over budget: denied, nothing charged.
        assert!(!env.charge_declared_external(6 * MIB));
        assert_eq!(budget.snapshot().v8_external, 6 * MIB);

        // A negative delta larger than declared is clamped to what was declared,
        // so it can never underflow the pool.
        env.uncharge_declared_external(100 * MIB);
        assert_eq!(budget.snapshot().v8_external, 0);
        assert_eq!(env.external_declared, 0);

        // Further release is a no-op.
        env.uncharge_declared_external(MIB);
        assert_eq!(budget.snapshot().v8_external, 0);
    }

    #[test]
    fn declared_external_released_on_drop() {
        let budget = ResourceBudget::with_memory_limit(10 * MIB);
        {
            let mut env = NapiEnv::new(
                Arc::clone(&budget),
                PendingMessages::new(),
                None,
                Arc::new(std::sync::Mutex::new(HashSet::new())),
                Arc::new(AtomicBool::new(false)),
                #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
                Arc::new(AtomicU32::new(1)),
                #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
                None,
            );
            assert!(env.charge_declared_external(4 * MIB));
            assert_eq!(budget.snapshot().v8_external, 4 * MIB);
        }
        assert_eq!(
            budget.snapshot().v8_external,
            0,
            "drop releases declared external"
        );
    }

    #[cfg(not(target_arch = "wasm32"))]
    #[test]
    fn dropping_env_keeps_quota_reserved_until_native_release_finishes() {
        let budget = ResourceBudget::with_memory_limit(110 * MIB);
        let mut env = NapiEnv::new(
            Arc::clone(&budget),
            PendingMessages::new(),
            None,
            Arc::new(std::sync::Mutex::new(HashSet::new())),
            Arc::new(AtomicBool::new(false)),
            Arc::new(AtomicU32::new(1)),
            None,
        );
        let reservation = env.reserve_isolate(RequestedHeap::default()).unwrap();
        // The release callback below is deliberately substituted so the
        // cross-thread check can pause at the native quiescence boundary.
        let fake_env = 1usize as SnapiEnv;
        let (env_id, _) = env.register_napi_env(fake_env).unwrap();
        env.env_heap_charges.insert(
            env_id,
            EnvHeapChargeHandle {
                ceiling: reservation.ceiling_bytes,
                tracker: 0,
            },
        );
        let (entered_tx, entered_rx) = mpsc::channel();
        let (finish_tx, finish_rx) = mpsc::channel();
        let worker = thread::spawn(move || {
            env.release_registered_envs(|_| {
                entered_tx.send(()).unwrap();
                finish_rx.recv().unwrap();
                true
            });
        });
        entered_rx.recv().unwrap();
        assert_eq!(
            budget.snapshot().v8_heap_reserved,
            reservation.ceiling_bytes
        );
        assert!(
            budget
                .try_reserve_env(RequestedHeap::default(), None)
                .is_err()
        );
        finish_tx.send(()).unwrap();
        worker.join().unwrap();
        assert_eq!(budget.snapshot().v8_heap_reserved, 0);
        let later = budget
            .try_reserve_env(RequestedHeap::default(), None)
            .unwrap();
        budget.release_env(later.ceiling_bytes);
    }

    #[test]
    fn callback_reentrancy_is_bounded() {
        let mut env = NapiEnv::new(
            ResourceBudget::unlimited(),
            PendingMessages::new(),
            None,
            Arc::new(std::sync::Mutex::new(HashSet::new())),
            Arc::new(AtomicBool::new(false)),
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            Arc::new(AtomicU32::new(1)),
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            None,
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
