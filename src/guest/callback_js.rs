use std::ffi::c_void;

use wasm_bindgen::{JsCast, JsValue};
use wasmer::{AsStoreMut, AsStoreRef, FunctionEnv, FunctionEnvMut, Store, Table, Value, js::AsJs};

#[cfg(all(target_arch = "wasm32", feature = "js"))]
use std::future::Future;
#[cfg(all(target_arch = "wasm32", feature = "js"))]
use wasmer::{AsStoreAsync, AsyncFunctionEnvMut, StoreAsync};

use crate::{NapiEnv, snapi::SnapiEnv};

use super::util::{forget_guest_allocation, recycle_guest_allocation};

type RawFunctionEnvMut = FunctionEnvMut<'static, NapiEnv>;

/// A value-handle ID reserved for callbacks which arrived while the guest was
/// suspended through JSPI. The host bridge keeps their callback frame alive
/// and retries them from the next synchronous guest checkpoint.
pub(crate) const CALLBACK_DEFERRED: u32 = u32::MAX;

pub(crate) fn guest_memory_buffer(store: impl AsStoreRef, memory: &wasmer::Memory) -> JsValue {
    memory
        .as_jsvalue(&store)
        .unchecked_into::<js_sys::WebAssembly::Memory>()
        .buffer()
}

#[repr(C)]
pub(crate) struct CallbackInvocationCtx {
    env: CallbackEnv,
}

// The JS backend is single-threaded; this marker only satisfies the generic
// FunctionEnv bound and the context is never transferred between workers.
#[cfg(all(target_arch = "wasm32", feature = "js"))]
unsafe impl Send for CallbackInvocationCtx {}

#[derive(Clone)]
enum CallbackEnv {
    Sync {
        env: FunctionEnv<NapiEnv>,
        store_id: usize,
    },
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    Async(AsyncCallbackOwner),
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
pub(crate) struct AsyncCallbackOwner {
    env: FunctionEnv<NapiEnv>,
    store: StoreAsync,
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
impl Clone for AsyncCallbackOwner {
    fn clone(&self) -> Self {
        Self {
            env: self.env.clone(),
            store: self.store.store(),
        }
    }
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
fn async_callback_owner(env: &FunctionEnvMut<NapiEnv>) -> Option<AsyncCallbackOwner> {
    Some(AsyncCallbackOwner {
        env: env.as_ref(),
        store: env.as_store_async()?.store(),
    })
}

impl CallbackEnv {
    fn with_env<R>(
        &self,
        mut callback: impl FnMut(&mut FunctionEnvMut<NapiEnv>) -> R,
    ) -> Option<R> {
        let (env, store_id) = match self {
            Self::Sync { env, store_id } => (env, *store_id),
            #[cfg(all(target_arch = "wasm32", feature = "js"))]
            Self::Async(owner) => (&owner.env, owner.store.store_id().as_raw().get()),
        };
        // A synchronous bridge call explicitly lends its store. Never recover
        // a different store merely because it happens to be current.
        let current = Store::with_current(|store| {
            if store.objects_mut().id().as_raw().get() != store_id {
                return None;
            }
            Some(callback(&mut env.clone().into_mut(store)))
        })
        .flatten();
        if current.is_some() {
            return current;
        }
        #[cfg(all(target_arch = "wasm32", feature = "js"))]
        if let Self::Async(owner) = self {
            // A busy JSPI store defers the callback to a guest checkpoint.
            if let Some(mut store) = owner.store.try_write_lock() {
                return Some(callback(&mut owner.env.clone().into_mut(&mut store)));
            }
        }
        None
    }
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
pub(crate) fn install_persistent_callback_state(
    env: &mut FunctionEnvMut<NapiEnv>,
    env_id: u32,
    snapi_env: SnapiEnv,
    async_env: AsyncCallbackOwner,
) {
    let mut ctx = Box::new(CallbackInvocationCtx {
        env: CallbackEnv::Async(async_env),
    });
    let ctx_ptr = (&mut *ctx as *mut CallbackInvocationCtx).cast::<c_void>();
    let data = env.data_mut();
    data.envs_awaiting_async_owner.retain(|&id| id != env_id);
    data.persistent_callback_contexts.insert(env_id, ctx);
    unsafe {
        crate::snapi::snapi_bridge_swap_active_callback_ctx(snapi_env, ctx_ptr);
    }
}

/// Gives a new env its persistent callback context, owned by the store's JSPI
/// shared `StoreAsync` handle. A synchronous store has no such owner yet; some
/// embedders promote it to an async store later. Until then the env waits:
/// the next callback state installed once the store is async, synchronous or
/// async (a checkpoint, a message wait), installs the context.
#[cfg(all(target_arch = "wasm32", feature = "js"))]
pub(crate) fn install_async_owner(
    env: &mut FunctionEnvMut<NapiEnv>,
    env_id: u32,
    snapi_env: SnapiEnv,
) {
    match async_callback_owner(env) {
        Some(async_env) => install_persistent_callback_state(env, env_id, snapi_env, async_env),
        None => env.data_mut().envs_awaiting_async_owner.push(env_id),
    }
}

/// Installs the persistent callback context of an env created from a
/// synchronous store, once that store has become async. An async import
/// passes the owner it holds; otherwise it is looked up.
#[cfg(all(target_arch = "wasm32", feature = "js"))]
fn install_awaited_async_owner(
    env: &mut FunctionEnvMut<NapiEnv>,
    snapi_env: SnapiEnv,
    owner: Option<AsyncCallbackOwner>,
) {
    let Some(&env_id) = env
        .data()
        .napi_state_to_guest_env
        .get(&(snapi_env as usize))
    else {
        return;
    };
    if !env.data().envs_awaiting_async_owner.contains(&env_id) {
        return;
    }
    if let Some(async_env) = owner.or_else(|| async_callback_owner(env)) {
        install_persistent_callback_state(env, env_id, snapi_env, async_env);
    }
}

struct CallbackStateGuard {
    _ctx: Box<CallbackInvocationCtx>,
    snapi_env: SnapiEnv,
    prev: *mut c_void,
    env: *mut RawFunctionEnvMut,
    owner_id: u64,
}

impl Drop for CallbackStateGuard {
    fn drop(&mut self) {
        if !self.env.is_null() {
            let env = unsafe { &mut *self.env.cast::<FunctionEnvMut<'_, NapiEnv>>() };
            flush_host_buffer_copies(env, self.owner_id);
            if env.data().host_buffer_copy_frames.is_empty() {
                flush_pending_host_buffer_copies(env, self.snapi_env);
            } else {
                publish_pending_host_buffer_copies(env, self.snapi_env);
            }
        }
        unsafe {
            crate::snapi::snapi_bridge_swap_active_callback_ctx(self.snapi_env, self.prev);
        }
    }
}

fn install_callback_state(
    env: &mut FunctionEnvMut<NapiEnv>,
    snapi_env: SnapiEnv,
) -> Option<CallbackStateGuard> {
    if snapi_env.is_null() {
        return None;
    }
    // Before saving the active context below, so this guard and any outer
    // one created later restore the persistent context, never a stack guard.
    // Empty unless an env was created from a synchronous store.
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    if !env.data().envs_awaiting_async_owner.is_empty() {
        install_awaited_async_owner(env, snapi_env, None);
    }

    let env_ptr = (env as *mut FunctionEnvMut<'_, NapiEnv>).cast::<RawFunctionEnvMut>();
    let mut ctx = Box::new(CallbackInvocationCtx {
        env: CallbackEnv::Sync {
            env: env.as_ref(),
            store_id: env.objects_mut().id().as_raw().get(),
        },
    });
    let owner_id = env.data_mut().next_host_buffer_owner();
    env.data_mut().host_buffer_copy_frames.push(owner_id);
    let prev = unsafe {
        crate::snapi::snapi_bridge_swap_active_callback_ctx(
            snapi_env,
            (&mut *ctx as *mut CallbackInvocationCtx).cast::<c_void>(),
        )
    };
    Some(CallbackStateGuard {
        env: env_ptr,
        _ctx: ctx,
        snapi_env,
        prev,
        owner_id,
    })
}

fn call_guest_callback(
    env: &mut FunctionEnvMut<NapiEnv>,
    table: &Table,
    guest_env: u32,
    wasm_fn_ptr: u32,
    callback_arg: u32,
) -> u32 {
    let func = match env.data().func_cache.get(&wasm_fn_ptr).cloned() {
        Some(func) => func,
        None => {
            let Some(elem) = table.get(env, wasm_fn_ptr) else {
                eprintln!("[callback trampoline] table entry missing for function {wasm_fn_ptr}");
                return 0;
            };
            let func = match elem {
                Value::FuncRef(Some(func)) => func,
                Value::FuncRef(None) => {
                    eprintln!("[callback trampoline] null function reference at {wasm_fn_ptr}");
                    return 0;
                }
                other => {
                    eprintln!(
                        "[callback trampoline] non-function table value at {wasm_fn_ptr}: {other:?}"
                    );
                    return 0;
                }
            };
            env.data_mut().func_cache.insert(wasm_fn_ptr, func.clone());
            func
        }
    };
    let result = func.call(
        env,
        &[
            Value::I32(guest_env as i32),
            Value::I32(callback_arg as i32),
        ],
    );
    match result {
        Ok(ret_vals) => match ret_vals.first() {
            Some(Value::I32(v)) => *v as u32,
            Some(Value::I64(v)) => *v as u32,
            _ => 0,
        },
        Err(err) => {
            eprintln!("[callback trampoline] error calling function: {err}");
            0
        }
    }
}

fn call_guest_finalizer(
    env: &mut FunctionEnvMut<NapiEnv>,
    table: &Table,
    guest_env: u32,
    wasm_fn_ptr: u32,
    data: u32,
    hint: u32,
) -> u32 {
    let func = match env.data().func_cache.get(&wasm_fn_ptr).cloned() {
        Some(func) => func,
        None => {
            let Some(Value::FuncRef(Some(func))) = table.get(env, wasm_fn_ptr) else {
                eprintln!("[finalizer trampoline] table entry missing for function {wasm_fn_ptr}");
                return 0;
            };
            env.data_mut().func_cache.insert(wasm_fn_ptr, func.clone());
            func
        }
    };
    match func.call(
        env,
        &[
            Value::I32(guest_env as i32),
            Value::I32(data as i32),
            Value::I32(hint as i32),
        ],
    ) {
        Ok(values) => match values.first() {
            Some(Value::I32(value)) => *value as u32,
            Some(Value::I64(value)) => *value as u32,
            _ => 0,
        },
        Err(error) => {
            eprintln!("[finalizer trampoline] error calling function: {error}");
            0
        }
    }
}

fn call_guest_callback_and_flush(
    env: &mut FunctionEnvMut<NapiEnv>,
    table: &Table,
    guest_env: u32,
    wasm_fn_ptr: u32,
    callback_arg: u32,
) -> u32 {
    let snapi_env = env.data().resolve_napi_env(guest_env);
    refresh_host_buffer_copies(env, snapi_env);
    let owner_id = env.data_mut().next_host_buffer_owner();
    env.data_mut().host_buffer_copy_frames.push(owner_id);
    let result = call_guest_callback(env, table, guest_env, wasm_fn_ptr, callback_arg);
    flush_host_buffer_copies(env, owner_id);
    // Nested JavaScript callbacks must not end pointer lifetimes owned by their
    // caller. Only the outermost callback frame may release pending mappings.
    if env.data().host_buffer_copy_frames.is_empty() {
        flush_pending_host_buffer_copies(env, snapi_env);
    } else {
        publish_pending_host_buffer_copies(env, snapi_env);
    }
    result
}

fn flush_host_buffer_copies(env: &mut FunctionEnvMut<NapiEnv>, owner_id: u64) {
    if let Some(index) = env
        .data()
        .host_buffer_copy_frames
        .iter()
        .position(|candidate| *candidate == owner_id)
    {
        env.data_mut().host_buffer_copy_frames.remove(index);
    }
    flush_host_buffer_copies_for_owner(env, owner_id);
}

pub fn flush_pending_host_buffer_copies(env: &mut FunctionEnvMut<NapiEnv>, snapi_env: SnapiEnv) {
    if snapi_env.is_null() || env.data().host_buffer_copies.is_empty() {
        return;
    }

    let drained = {
        let state = env.data_mut();
        std::mem::take(&mut state.host_buffer_copies)
    };

    let mut held = Vec::new();
    for mut mapping in drained {
        let mapping_snapi = env.data().resolve_napi_env(mapping.guest_env);
        if mapping.needs_flush && mapping.byte_len > 0 && !mapping_snapi.is_null() {
            if let Some(memory) = env.data().memory.clone() {
                let memory_buffer = guest_memory_buffer(env.as_store_ref(), &memory);
                let _ = crate::snapi_js::copy_memory_bytes_to_reference(
                    mapping_snapi,
                    mapping.host_reference_id,
                    &memory_buffer,
                    mapping.guest_ptr,
                    mapping.byte_len as u32,
                );
            }
            // The guest snapshot is now coherent with JavaScript. Retained
            // mappings stay allocated and are refreshed before guest re-entry.
            mapping.needs_flush = false;
        }
        if mapping.persistent || mapping.reference_holds > 0 {
            held.push(mapping);
            continue;
        }
        if mapping.host_reference_id != 0 && !mapping_snapi.is_null() {
            unsafe {
                crate::snapi::snapi_bridge_delete_reference(
                    mapping_snapi,
                    mapping.host_reference_id,
                );
            }
        }

        let state = env.data_mut();
        let mapping_end = mapping.guest_ptr.saturating_add(mapping.byte_len as u32);
        state
            .guest_data_ptrs
            .retain(|(guest_env, handle_id), guest_ptr| {
                *guest_env != mapping.guest_env
                    || *handle_id != mapping.handle_id
                        && (*guest_ptr < mapping.guest_ptr || *guest_ptr >= mapping_end)
            });
        if mapping.backing_store_token != 0 {
            state
                .guest_data_backing_stores
                .retain(|(guest_env, _), backing_mapping| {
                    *guest_env != mapping.guest_env
                        || backing_mapping.guest_ptr < mapping.guest_ptr
                        || backing_mapping.guest_ptr >= mapping_end
                });
        }
        if mapping.guest_allocation_recyclable {
            recycle_guest_allocation(env, mapping.guest_ptr);
        } else {
            forget_guest_allocation(env, mapping.guest_ptr);
        }
    }
    env.data_mut().host_buffer_copies.extend(held);
}

/// Publish native writes before entering JavaScript without ending any native
/// pointer lifetime. A guest callback may call JavaScript and then continue to
/// use a pointer returned earlier by N-API (llhttp does this after its headers
/// callback). Recycling that mapping at the nested handoff turns the live
/// pointer into a use-after-release. Frame teardown remains the sole owner of
/// releasing unreferenced mappings.
pub fn publish_pending_host_buffer_copies(env: &mut FunctionEnvMut<NapiEnv>, snapi_env: SnapiEnv) {
    if snapi_env.is_null() || env.data().host_buffer_copies.is_empty() {
        return;
    }
    let Some(memory) = env.data().memory.clone() else {
        return;
    };
    let memory_buffer = guest_memory_buffer(env.as_store_ref(), &memory);
    for index in 0..env.data().host_buffer_copies.len() {
        let (mapping_guest_env, host_reference_id, guest_ptr, byte_len, needs_flush) = {
            let mapping = &env.data().host_buffer_copies[index];
            (
                mapping.guest_env,
                mapping.host_reference_id,
                mapping.guest_ptr,
                mapping.byte_len,
                mapping.needs_flush,
            )
        };
        if !needs_flush || byte_len == 0 {
            continue;
        }
        let mapping_snapi = env.data().resolve_napi_env(mapping_guest_env);
        if mapping_snapi.is_null() {
            continue;
        }
        if crate::snapi_js::copy_memory_bytes_to_reference(
            mapping_snapi,
            host_reference_id,
            &memory_buffer,
            guest_ptr,
            byte_len as u32,
        ) == 0
        {
            env.data_mut().host_buffer_copies[index].needs_flush = false;
        }
    }
}

/// Restore host-side mutations into stable guest allocations before native
/// code resumes. This is the inverse half of `publish_pending_host_buffer_copies`:
/// mappings stay allocated across the JavaScript phase, so retained and
/// interior native pointers remain valid across nested callbacks.
pub fn refresh_host_buffer_copies(env: &mut FunctionEnvMut<NapiEnv>, snapi_env: SnapiEnv) {
    if snapi_env.is_null() || env.data().host_buffer_copies.is_empty() {
        return;
    }
    let Some(memory) = env.data().memory.clone() else {
        return;
    };
    let memory_buffer = guest_memory_buffer(env.as_store_ref(), &memory);
    for index in 0..env.data().host_buffer_copies.len() {
        let (mapping_guest_env, host_reference_id, guest_ptr, byte_len, needs_flush) = {
            let mapping = &env.data().host_buffer_copies[index];
            (
                mapping.guest_env,
                mapping.host_reference_id,
                mapping.guest_ptr,
                mapping.byte_len,
                mapping.needs_flush,
            )
        };
        if needs_flush || byte_len == 0 {
            continue;
        }
        let mapping_snapi = env.data().resolve_napi_env(mapping_guest_env);
        if mapping_snapi.is_null() {
            continue;
        }
        if crate::snapi_js::copy_reference_bytes_to_memory(
            mapping_snapi,
            host_reference_id,
            &memory_buffer,
            guest_ptr,
            byte_len as u32,
        ) == 0
        {
            env.data_mut().host_buffer_copies[index].needs_flush = true;
        }
    }
}

pub fn flush_host_buffer_copies_for_owner(env: &mut FunctionEnvMut<NapiEnv>, owner_id: u64) {
    let (drained, mut retained): (Vec<_>, Vec<_>) =
        std::mem::take(&mut env.data_mut().host_buffer_copies)
            .into_iter()
            .partition(|mapping| mapping.owner_id == owner_id);

    let mut held = Vec::new();
    for mut mapping in drained {
        let mapping_snapi = env.data().resolve_napi_env(mapping.guest_env);
        if mapping.needs_flush && mapping.byte_len > 0 && !mapping_snapi.is_null() {
            if let Some(memory) = env.data().memory.clone() {
                let memory_buffer = guest_memory_buffer(env.as_store_ref(), &memory);
                let _ = crate::snapi_js::copy_memory_bytes_to_reference(
                    mapping_snapi,
                    mapping.host_reference_id,
                    &memory_buffer,
                    mapping.guest_ptr,
                    mapping.byte_len as u32,
                );
            }
            mapping.needs_flush = false;
        }
        if mapping.persistent || mapping.reference_holds > 0 {
            held.push(mapping);
            continue;
        }
        if mapping.host_reference_id != 0 && !mapping_snapi.is_null() {
            unsafe {
                crate::snapi::snapi_bridge_delete_reference(
                    mapping_snapi,
                    mapping.host_reference_id,
                );
            }
        }

        let state = env.data_mut();
        let mapping_end = mapping.guest_ptr.saturating_add(mapping.byte_len as u32);
        state
            .guest_data_ptrs
            .retain(|(guest_env, handle_id), guest_ptr| {
                *guest_env != mapping.guest_env
                    || *handle_id != mapping.handle_id
                        && (*guest_ptr < mapping.guest_ptr || *guest_ptr >= mapping_end)
            });
        if mapping.backing_store_token != 0 {
            state
                .guest_data_backing_stores
                .retain(|(guest_env, _), backing_mapping| {
                    *guest_env != mapping.guest_env
                        || backing_mapping.guest_ptr < mapping.guest_ptr
                        || backing_mapping.guest_ptr >= mapping_end
                });
        }
        if mapping.guest_allocation_recyclable {
            recycle_guest_allocation(env, mapping.guest_ptr);
        } else {
            forget_guest_allocation(env, mapping.guest_ptr);
        }
    }
    retained.extend(held);
    env.data_mut().host_buffer_copies = retained;
}

pub fn with_callback_state<R>(
    env: &mut FunctionEnvMut<NapiEnv>,
    snapi_env: SnapiEnv,
    f: impl FnOnce() -> R,
) -> R {
    let guard = install_callback_state(env, snapi_env);
    let result = if guard.is_some() {
        env.as_store_mut().parked(f)
    } else {
        f()
    };
    drop(guard);
    result
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
pub async fn with_callback_state_async<R, F>(
    env: AsyncFunctionEnvMut<NapiEnv>,
    snapi_env: SnapiEnv,
    future: F,
) -> R
where
    F: Future<Output = R>,
{
    if snapi_env.is_null() {
        return future.await;
    }

    // An env created from a synchronous store gets its persistent context
    // here at the latest, before the import suspends and host jobs (Promise
    // jobs, message deliveries) call into the guest.
    let owner_id = {
        let mut locked = env.write().await;
        let mut sync_env = locked.as_function_env_mut();
        if !sync_env.data().envs_awaiting_async_owner.is_empty() {
            let owner = AsyncCallbackOwner {
                env: env.as_ref(),
                store: env.as_store_async().store(),
            };
            install_awaited_async_owner(&mut sync_env, snapi_env, Some(owner));
        }
        let owner_id = sync_env.data_mut().next_host_buffer_owner();
        sync_env.data_mut().host_buffer_copy_frames.push(owner_id);
        owner_id
    };
    // The environment's persistent Async callback context is the owner of
    // host-to-guest re-entry while a JSPI import is suspended. Do not replace
    // it with a future-local context: multiple suspended imports can overlap
    // and complete out of order, so stack-style save/restore would eventually
    // restore a pointer to a context owned by an already-completed future.
    let result = future.await;
    {
        // Keep this guard in an explicit scope. A completed Rust future may
        // remain owned by its JavaScript Promise until a later GC cycle; if the
        // guard stays in the generator state, it also keeps the async Store
        // shared after the guest entrypoint has returned.
        let mut locked = env.write().await;
        let mut sync_env = locked.as_function_env_mut();
        flush_host_buffer_copies(&mut sync_env, owner_id);
        if sync_env.data().host_buffer_copy_frames.is_empty() {
            flush_pending_host_buffer_copies(&mut sync_env, snapi_env);
        } else {
            publish_pending_host_buffer_copies(&mut sync_env, snapi_env);
        }
    }
    result
}

/// Rust trampoline called from C++ when a V8 callback fires.
/// Re-enters the active guest callback scope and dispatches into the WASM guest.
#[unsafe(no_mangle)]
pub extern "C" fn snapi_host_invoke_wasm_callback(
    callback_ctx: *mut c_void,
    guest_env: u32,
    wasm_fn_ptr: u32,
    callback_arg: u32,
) -> u32 {
    if callback_ctx.is_null() {
        eprintln!("[callback trampoline] no active callback scope available");
        return 0;
    }
    let ctx = unsafe { &*(callback_ctx as *const CallbackInvocationCtx) };
    // Guest reentry can release the env and its persistent callback context.
    // Keep independent handles until dispatch finishes.
    let callback = ctx.env.clone();
    callback
        .with_env(|env| {
            let Some(table) = env.data().table.clone() else {
                eprintln!("[callback trampoline] guest function table is not installed");
                return 0;
            };
            call_guest_callback_and_flush(env, &table, guest_env, wasm_fn_ptr, callback_arg)
        })
        .unwrap_or(CALLBACK_DEFERRED)
}

/// Dispatch a deferred host-JS finalizer while the provider is at an explicit
/// guest checkpoint. JavaScript GC only makes the record ready; guest re-entry
/// happens here, under the same callback ownership rules as ordinary N-API
/// callbacks.
#[unsafe(no_mangle)]
pub extern "C" fn snapi_host_invoke_wasm_finalizer(
    callback_ctx: *mut c_void,
    guest_env: u32,
    wasm_fn_ptr: u32,
    data: u32,
    hint: u32,
) -> u32 {
    if callback_ctx.is_null() {
        return CALLBACK_DEFERRED;
    }
    let ctx = unsafe { &*(callback_ctx as *const CallbackInvocationCtx) };
    let callback = ctx.env.clone();
    let invoke = |env: &mut FunctionEnvMut<NapiEnv>| {
        let Some(table) = env.data().table.clone() else {
            return 0;
        };
        let snapi_env = env.data().resolve_napi_env(guest_env);
        refresh_host_buffer_copies(env, snapi_env);
        let owner_id = env.data_mut().next_host_buffer_owner();
        env.data_mut().host_buffer_copy_frames.push(owner_id);
        let result = call_guest_finalizer(env, &table, guest_env, wasm_fn_ptr, data, hint);
        flush_host_buffer_copies(env, owner_id);
        if env.data().host_buffer_copy_frames.is_empty() {
            flush_pending_host_buffer_copies(env, snapi_env);
        } else {
            publish_pending_host_buffer_copies(env, snapi_env);
        }
        result
    };
    callback.with_env(invoke).unwrap_or(CALLBACK_DEFERRED)
}

#[cfg(test)]
mod tests {
    use super::*;
    use wasm_bindgen_test::wasm_bindgen_test;

    fn callback_state() -> NapiEnv {
        NapiEnv::new(
            crate::budget::ResourceBudget::unlimited(),
            None,
            std::sync::Arc::default(),
            std::sync::Arc::default(),
        )
    }

    #[wasm_bindgen_test]
    fn guest_buffer_views_follow_memory_growth() {
        for shared in [false, true] {
            let mut store = Store::default();
            let memory =
                wasmer::Memory::new(&mut store, wasmer::MemoryType::new(1, Some(2), shared))
                    .unwrap();
            let initial = guest_memory_buffer(store.as_store_ref(), &memory);
            js_sys::Uint8Array::new(&initial).set_index(0, 42);
            memory.grow(&mut store, 1).unwrap();
            let current =
                js_sys::Uint8Array::new(&guest_memory_buffer(store.as_store_ref(), &memory));
            assert_eq!(current.length(), 2 * wasmer::WASM_PAGE_SIZE as u32);
            assert_eq!(current.get_index(0), 42);
        }
    }

    #[wasm_bindgen_test]
    fn synchronous_callbacks_only_borrow_their_lent_store() {
        let mut store = Store::default();
        let function_env = FunctionEnv::new(&mut store, callback_state());
        let callback = CallbackEnv::Sync {
            env: function_env.clone(),
            store_id: store.id().as_raw().get(),
        };
        assert!(callback.with_env(|_| ()).is_none());
        let mut other_store = Store::default();
        other_store.as_store_mut().parked(|| {
            assert!(callback.with_env(|_| ()).is_none());
        });

        let mut env = function_env.clone().into_mut(&mut store);
        env.as_store_mut().parked(|| {
            assert_eq!(
                callback.with_env(|env| {
                    env.data_mut().default_napi_env_id = Some(42);
                    // A nested callback needs the current borrow parked too.
                    assert!(callback.with_env(|_| ()).is_none());
                    env.as_store_mut().parked(|| callback.with_env(|_| 73))
                }),
                Some(Some(73))
            );
        });
        assert_eq!(function_env.as_ref(&store).default_napi_env_id, Some(42));
        assert!(callback.with_env(|_| ()).is_none());
    }

    #[wasm_bindgen_test(async)]
    async fn asynchronous_callbacks_defer_busy_stores_without_retaining_waiters() {
        let mut store = Store::default();
        let function_env = FunctionEnv::new(&mut store, callback_state());
        let store = store.into_async();
        let callback = CallbackEnv::Async(AsyncCallbackOwner {
            env: function_env.clone(),
            store: store.store(),
        });
        assert_eq!(
            callback.with_env(|env| {
                env.data_mut().default_napi_env_id = Some(17);
                42
            }),
            Some(42)
        );
        let locked = store.write_lock().await;
        assert!(callback.with_env(|_| ()).is_none());
        drop(locked);
        assert_eq!(
            callback.with_env(|env| env.data().default_napi_env_id),
            Some(Some(17))
        );
        drop(callback);
        assert!(store.into_store().is_ok());
    }
}
