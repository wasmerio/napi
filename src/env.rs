use std::collections::HashMap;
use std::sync::Arc;

use wasmer::{Memory, Table, TypedFunction};

use crate::budget::{EnvRejected, HeapReservation, RequestedHeap, ResourceBudget};
use crate::snapi::{SnapiEnv, snapi_bridge_unofficial_release_env};

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
    /// Heap ceiling (bytes) charged per live V8 env, keyed by guest env id, so
    /// teardown releases exactly what creation charged.
    env_heap_charges: HashMap<u32, u64>,
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

    /// Register a successfully-created env and attach its heap charge, so
    /// teardown releases it.
    pub(crate) fn commit_isolate(
        &mut self,
        env: SnapiEnv,
        reservation: &HeapReservation,
    ) -> (u32, u32) {
        let (env_id, scope_id) = self.register_napi_env(env);
        self.env_heap_charges
            .insert(env_id, reservation.ceiling_bytes);
        (env_id, scope_id)
    }

    /// Release a reservation whose env creation failed after [`reserve_isolate`].
    ///
    /// [`reserve_isolate`]: NapiEnv::reserve_isolate
    pub(crate) fn abort_isolate(&self, reservation: &HeapReservation) {
        self.budget.release_env(reservation.ceiling_bytes);
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
        // Release the heap ceiling + isolate slot this env reserved at creation.
        if let Some(ceiling) = self.env_heap_charges.remove(&env_id) {
            self.budget.release_env(ceiling);
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
    }
}
