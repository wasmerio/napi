use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

use wasmer::{Memory, Table, TypedFunction};

use crate::budget::{
    EnvExternalCharge, EnvHeapCharge, EnvRejected, HeapReservation, Pool, RequestedHeap,
    ResourceBudget,
};
use crate::snapi::{
    SnapiEnv, snapi_bridge_unofficial_release_env,
    snapi_bridge_unofficial_set_host_allocation_budget_callback,
    snapi_bridge_unofficial_set_host_near_heap_limit_callback,
};

/// Bookkeeping for one live V8 env's heap charge: the initial ceiling plus a
/// pointer (as `usize`, so [`NapiEnv`] stays `Send`) to the boxed
/// [`EnvHeapCharge`] the near-heap-limit callback grows. `tracker == 0` means
/// the env is unbudgeted (unlimited).
struct EnvHeapChargeHandle {
    ceiling: u64,
    tracker: usize,
}

pub(crate) struct HostBufferCopy {
    pub(crate) handle_id: u32,
    pub(crate) backing_store_token: u64,
    pub(crate) guest_ptr: u32,
    pub(crate) byte_len: usize,
}

pub(crate) struct GuestBackingStoreMapping {
    pub(crate) host_addr: u64,
    pub(crate) guest_ptr: u32,
    pub(crate) byte_len: usize,
}

pub(crate) struct NapiEnv {
    /// The app's shared accountant. V8 env (isolate) heap ceilings and the
    /// `max_envs` isolate count are charged against it; workers share the same
    /// `Arc` so the app-wide budget stays honest across stores.
    pub(crate) budget: Arc<ResourceBudget>,
    /// Per-app cap on live V8 isolates (`None` = unlimited).
    pub(crate) max_envs: Option<usize>,
    /// Heap charge per live V8 env, keyed by guest env id, so teardown releases
    /// exactly what creation charged plus what the callback later granted.
    env_heap_charges: HashMap<u32, EnvHeapChargeHandle>,
    /// External memory the guest has declared via `napi_adjust_external_memory`,
    /// charged to [`Pool::V8External`]. Tracked so a negative adjustment can only
    /// release what this env declared — never the allocator's charges or more
    /// than it added — and so teardown releases the remainder.
    external_declared: u64,
    pub(crate) memory: Option<Memory>,
    pub(crate) malloc_fn: Option<TypedFunction<i32, i32>>,
    pub(crate) table: Option<Table>,
    /// Maps value handle IDs to their guest-memory data pointers.
    /// Used for buffers/arraybuffers backed by guest linear memory.
    pub(crate) guest_data_ptrs: HashMap<u32, u32>,
    /// Maps stable host backing-store tokens to guest-memory data pointers.
    /// This keeps external Buffer/ArrayBuffer aliases stable even when V8/N-API
    /// surfaces the same backing store through a different value handle.
    pub(crate) guest_data_backing_stores: HashMap<u64, GuestBackingStoreMapping>,
    /// Host-owned buffer/arraybuffer mappings copied into guest memory for the
    /// duration of an active callback. These are written back on callback exit.
    pub(crate) host_buffer_copies: Vec<HostBufferCopy>,
    pub(crate) host_buffer_copy_frames: Vec<usize>,
    /// Host-owned buffer copies created while servicing a single guest-side
    /// native binding invocation (typically bracketed by napi_get_cb_info and a
    /// return-value creation call).
    pub(crate) host_buffer_method_frames: Vec<usize>,
    pub(crate) default_napi_env_id: Option<u32>,
    pub(crate) next_napi_env_id: u32,
    pub(crate) next_napi_scope_id: u32,
    pub(crate) napi_envs: HashMap<u32, usize>,
    pub(crate) napi_state_to_guest_env: HashMap<usize, u32>,
    pub(crate) napi_scopes: HashMap<u32, u32>,
}

impl NapiEnv {
    pub(crate) fn new(budget: Arc<ResourceBudget>, max_envs: Option<usize>) -> Self {
        Self {
            budget,
            max_envs,
            env_heap_charges: HashMap::new(),
            external_declared: 0,
            memory: None,
            malloc_fn: None,
            table: None,
            guest_data_ptrs: HashMap::new(),
            guest_data_backing_stores: HashMap::new(),
            host_buffer_copies: Vec::new(),
            host_buffer_copy_frames: Vec::new(),
            host_buffer_method_frames: Vec::new(),
            default_napi_env_id: None,
            next_napi_env_id: 0,
            next_napi_scope_id: 0,
            napi_envs: HashMap::new(),
            napi_state_to_guest_env: HashMap::new(),
            napi_scopes: HashMap::new(),
        }
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
    /// a limited budget — install the host-owned near-heap-limit callback so V8
    /// heap growth for this isolate is charged against the budget. Teardown
    /// releases both the initial ceiling and any granted growth.
    pub(crate) fn commit_isolate(
        &mut self,
        env: SnapiEnv,
        reservation: &HeapReservation,
    ) -> (u32, u32) {
        let (env_id, scope_id) = self.register_napi_env(env);

        let tracker = if reservation.clamped {
            let boxed = Box::into_raw(Box::new(EnvHeapCharge {
                budget: Arc::clone(&self.budget),
                granted: AtomicU64::new(0),
            }));
            // SAFETY: `env` is the isolate just created; `boxed` outlives the
            // callback (freed only at this env's teardown, below).
            unsafe {
                snapi_bridge_unofficial_set_host_near_heap_limit_callback(
                    env,
                    boxed as *const c_void,
                );
            }

            // Install the enforcing array-buffer allocator hook. Ownership of
            // this box transfers to the allocator, which releases it from its
            // destructor at env teardown; reclaim it only if registration fails.
            let external = Box::into_raw(Box::new(EnvExternalCharge {
                budget: Arc::clone(&self.budget),
            }));
            // SAFETY: `external` is a fresh box handed to the allocator.
            let status = unsafe {
                snapi_bridge_unofficial_set_host_allocation_budget_callback(
                    env,
                    external as *const c_void,
                )
            };
            if status != 0 {
                // SAFETY: registration failed, so the allocator did not take
                // ownership; reclaim the box we just leaked.
                drop(unsafe { Box::from_raw(external) });
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
        (env_id, scope_id)
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

    pub(crate) fn register_napi_env(&mut self, env: SnapiEnv) -> (u32, u32) {
        let env_id = self.next_napi_env_id.max(1);
        self.next_napi_env_id = env_id.saturating_add(1);

        let scope_id = self.next_napi_scope_id.max(1);
        self.next_napi_scope_id = scope_id.saturating_add(1);

        self.napi_envs.insert(env_id, env as usize);
        self.napi_state_to_guest_env.insert(env as usize, env_id);
        self.napi_scopes.insert(scope_id, env_id);
        (env_id, scope_id)
    }

    pub(crate) fn unregister_napi_scope(&mut self, scope_id: u32) -> Option<SnapiEnv> {
        let env_id = self.napi_scopes.remove(&scope_id)?;
        if self.default_napi_env_id == Some(env_id) {
            self.default_napi_env_id = None;
        }
        // Release the heap ceiling + any callback-granted growth + isolate slot
        // this env reserved.
        if let Some(handle) = self.env_heap_charges.remove(&env_id) {
            let granted = if handle.tracker != 0 {
                // SAFETY: reclaim the box created in `commit_isolate`. The
                // near-heap-limit callback holding this pointer only runs during
                // JS execution, and no JS runs between here and the paired env
                // release that removes the callback and disposes the isolate, so
                // freeing it now is safe.
                let boxed = unsafe { Box::from_raw(handle.tracker as *mut EnvHeapCharge) };
                boxed.granted.load(Ordering::Acquire)
            } else {
                0
            };
            self.budget.uncharge(Pool::V8HeapReserved, granted);
            self.budget.release_env(handle.ceiling);
        }
        let env = self.napi_envs.remove(&env_id)?;
        self.napi_state_to_guest_env.remove(&env);
        Some(env as SnapiEnv)
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
}

impl Drop for NapiEnv {
    fn drop(&mut self) {
        let scope_ids: Vec<u32> = self.napi_scopes.keys().copied().collect();
        for scope_id in scope_ids {
            if let Some(env) = self.unregister_napi_scope(scope_id) {
                unsafe {
                    let _ = snapi_bridge_unofficial_release_env(env);
                }
            }
        }
        // Release any external memory the guest declared but did not take back.
        if self.external_declared > 0 {
            self.budget
                .uncharge(Pool::V8External, self.external_declared);
            self.external_declared = 0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MIB: u64 = 1024 * 1024;

    #[test]
    fn declared_external_charges_denies_and_clamps() {
        let budget = ResourceBudget::with_memory_limit(10 * MIB);
        let mut env = NapiEnv::new(Arc::clone(&budget), None);

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
            let mut env = NapiEnv::new(Arc::clone(&budget), None);
            assert!(env.charge_declared_external(4 * MIB));
            assert_eq!(budget.snapshot().v8_external, 4 * MIB);
        }
        assert_eq!(
            budget.snapshot().v8_external,
            0,
            "drop releases declared external"
        );
    }
}
