// ============================================================
// WASM import handlers for "napi" module
// ============================================================

// --- Init ---

use std::ffi::{CString, c_void};

use wasmer::{AsStoreMut, Function, FunctionEnv, FunctionEnvMut, Imports, namespace};

#[cfg(all(target_arch = "wasm32", feature = "js"))]
use wasm_bindgen::JsValue;
#[cfg(all(target_arch = "wasm32", feature = "js"))]
use wasmer::AsyncFunctionEnvMut;

use crate::{
    NAPI_EXTENSION_WASMER_MODULE_NAME, NAPI_MODULE_NAME, NapiEnv,
    guest::{
        MAX_NAPI_BIGINT_WORDS, MAX_NAPI_CALLBACK_ARGS,
        callback::{
            flush_host_buffer_copies_for_owner, install_persistent_callback_state,
            publish_pending_host_buffer_copies, refresh_host_buffer_copies, with_callback_state,
            with_callback_state_async,
        },
        typedarray_element_size,
    },
    snapi::*,
};

use super::{abi, util::*};
use crate::snapi_js::{
    host_js_get_named_property, host_js_has_named_property, host_js_put_primitive,
    host_js_set_named_property, host_js_value,
};

const JS_BACKING_TOKEN_MARKER: u64 = 1 << 63;
const JS_BACKING_TOKEN_OFFSET_MASK: u64 = u32::MAX as u64;

fn guest_napi_wasm_init_env(mut env: FunctionEnvMut<NapiEnv>) -> i32 {
    let _ = unsafe { snapi_bridge_init() };
    let _ = unsafe { snapi_bridge_unofficial_configure_runtime(std::ptr::null(), 0) };

    if let Some(env_id) = env.data().default_napi_env_id {
        return env_id as i32;
    }

    let mut snapi_env_state: SnapiEnv = std::ptr::null_mut();
    let status =
        unsafe { snapi_bridge_unofficial_create_env(8, std::ptr::null(), &mut snapi_env_state) };
    if status != 0 || snapi_env_state.is_null() {
        return 0;
    }

    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    let async_env = env.as_async_mut();
    let (env_id, _scope_id) = env.data_mut().register_napi_env(snapi_env_state);
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    if let Some(async_env) = async_env {
        install_persistent_callback_state(&mut env, env_id, snapi_env_state, async_env);
    }
    env.data_mut().default_napi_env_id = Some(env_id);
    env_id as i32
}

fn with_cb_context<R>(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_env: u32,
    f: impl FnOnce() -> R,
) -> R {
    let snapi_env = env.data().resolve_napi_env(guest_env);
    publish_pending_host_buffer_copies(env, snapi_env);
    let result = with_callback_state(env, snapi_env, f);
    refresh_host_buffer_copies(env, snapi_env);
    result
}

fn snapi_env(env: &FunctionEnvMut<NapiEnv>, guest_env: u32) -> SnapiEnv {
    env.data().resolve_napi_env(guest_env)
}

fn drain_ready_guest_buffers(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_env: u32,
    env_handle: SnapiEnv,
    force: bool,
) {
    loop {
        let mut handle_id = 0u32;
        let mut guest_ptr = 0u32;
        let mut recyclable = 0i32;
        let status = unsafe {
            crate::snapi::snapi_bridge_take_ready_guest_buffer_finalizer(
                env_handle,
                i32::from(force),
                &mut handle_id,
                &mut guest_ptr,
                &mut recyclable,
            )
        };
        if status != 0 {
            break;
        }
        let Some(index) = env.data().host_buffer_copies.iter().position(|mapping| {
            mapping.guest_env == guest_env
                && mapping.handle_id == handle_id
                && mapping.guest_ptr == guest_ptr
                && mapping.persistent
        }) else {
            continue;
        };
        let mapping = env.data_mut().host_buffer_copies.remove(index);
        if mapping.host_reference_id != 0 {
            unsafe {
                crate::snapi::snapi_bridge_delete_reference(env_handle, mapping.host_reference_id);
            }
        }
        env.data_mut()
            .guest_data_ptrs
            .remove(&(guest_env, handle_id));
        if mapping.backing_store_token != 0 {
            env.data_mut()
                .guest_data_backing_stores
                .remove(&(guest_env, mapping.backing_store_token));
        }
        if recyclable != 0 {
            recycle_guest_allocation(env, guest_ptr);
        }
    }
}

fn host_backing_store_token(env: &FunctionEnvMut<NapiEnv>, guest_env: u32, handle_id: u32) -> u64 {
    let mut token = 0u64;
    let status = unsafe {
        snapi_bridge_get_backing_store_token(snapi_env(env, guest_env), handle_id, &mut token)
    };
    if status == 0 { token } else { 0 }
}

fn write_guest_pod<T>(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, value: &T) -> bool {
    if guest_ptr == 0 {
        return false;
    }
    let bytes = unsafe {
        std::slice::from_raw_parts((value as *const T).cast::<u8>(), std::mem::size_of::<T>())
    };
    write_guest_bytes(env, guest_ptr, bytes)
}

fn write_guest_pod_slice<T>(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    values: &[T],
) -> bool {
    if values.is_empty() {
        return true;
    }
    if guest_ptr == 0 {
        return false;
    }
    let bytes = unsafe {
        std::slice::from_raw_parts(values.as_ptr().cast::<u8>(), std::mem::size_of_val(values))
    };
    write_guest_bytes(env, guest_ptr, bytes)
}

fn remember_guest_backing_store(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_env: u32,
    handle_id: u32,
    backing_store_token: u64,
    host_addr: u64,
    guest_ptr: u32,
    byte_len: usize,
    covers_full_backing_store: bool,
) {
    let state = env.data_mut();
    state
        .guest_data_ptrs
        .insert((guest_env, handle_id), guest_ptr);
    if backing_store_token != 0 {
        // A Buffer created as a direct view over guest WebAssembly memory has
        // a token whose low 32 bits are its byte offset in that memory.
        // Register the whole backing store as an identity mapping as well as
        // the exact view. Buffer.subarray() then resolves offset aliases back
        // to guest memory instead of allocating a mirrored host copy.
        if host_addr == 0 && backing_store_token & JS_BACKING_TOKEN_MARKER != 0 {
            let byte_offset = backing_store_token & JS_BACKING_TOKEN_OFFSET_MASK;
            if byte_offset == u64::from(guest_ptr) {
                let base_token = backing_store_token & !JS_BACKING_TOKEN_OFFSET_MASK;
                state.guest_data_backing_stores.insert(
                    (guest_env, base_token),
                    crate::GuestBackingStoreMapping {
                        host_addr: 0,
                        guest_ptr: 0,
                        // JavaScript already bounds-checked this view against
                        // the same Wasm memory. The guest memory access applies
                        // the final bounds check after resolving the offset.
                        byte_len: usize::MAX,
                        covers_full_backing_store: true,
                    },
                );
            }
        }
        state.guest_data_backing_stores.insert(
            (guest_env, backing_store_token),
            crate::GuestBackingStoreMapping {
                host_addr,
                guest_ptr,
                byte_len,
                covers_full_backing_store,
            },
        );
    }
}

// TODO: Route Buffer/ArrayBuffer allocation through guest memory from the start
// so host-owned buffers do not need snapshot-and-flush fallback synchronization.
fn remember_host_buffer_copy(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_env: u32,
    handle_id: u32,
    backing_store_token: u64,
    host_addr: u64,
    guest_ptr: u32,
    byte_len: usize,
    guest_allocation_recyclable: bool,
    persistent: bool,
    needs_flush: bool,
    covers_full_backing_store: bool,
) -> bool {
    let mut host_reference_id = 0u32;
    if unsafe {
        snapi_bridge_create_reference(
            snapi_env(env, guest_env),
            handle_id,
            if persistent { 0 } else { 1 },
            &mut host_reference_id,
        )
    } != 0
    {
        return false;
    }
    {
        let state = env.data_mut();
        state
            .guest_data_ptrs
            .insert((guest_env, handle_id), guest_ptr);
        if backing_store_token != 0 {
            state.guest_data_backing_stores.insert(
                (guest_env, backing_store_token),
                crate::GuestBackingStoreMapping {
                    host_addr,
                    guest_ptr,
                    byte_len,
                    covers_full_backing_store,
                },
            );
        }
        state.host_buffer_copies.push(crate::HostBufferCopy {
            owner_id: state.current_host_buffer_owner(),
            guest_env,
            handle_id,
            host_reference_id,
            backing_store_token,
            guest_ptr,
            byte_len,
            guest_allocation_recyclable,
            persistent,
            reference_holds: 0,
            needs_flush,
        });
    }
    if persistent
        && unsafe {
            crate::snapi::snapi_bridge_register_guest_buffer_finalizer(
                snapi_env(env, guest_env),
                handle_id,
                guest_ptr,
                i32::from(guest_allocation_recyclable),
            )
        } != 0
    {
        if let Some(mapping) = env.data_mut().host_buffer_copies.pop() {
            unsafe {
                crate::snapi::snapi_bridge_delete_reference(
                    snapi_env(env, guest_env),
                    mapping.host_reference_id,
                );
            }
        }
        env.data_mut()
            .guest_data_ptrs
            .remove(&(guest_env, handle_id));
        if backing_store_token != 0 {
            env.data_mut()
                .guest_data_backing_stores
                .remove(&(guest_env, backing_store_token));
        }
        return false;
    }
    true
}

fn prepare_cached_guest_data(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_env: u32,
    handle_id: u32,
    guest_ptr: u32,
) -> bool {
    let matching = env
        .data()
        .host_buffer_copies
        .iter()
        .enumerate()
        .filter(|(_, mapping)| {
            let mapping_end = mapping.guest_ptr.saturating_add(mapping.byte_len as u32);
            mapping.guest_env == guest_env
                && guest_ptr >= mapping.guest_ptr
                && guest_ptr < mapping_end
        });
    let Some((index, mapping)) = matching
        .clone()
        .find(|(_, mapping)| mapping.handle_id == handle_id)
        .or_else(|| matching.into_iter().next())
    else {
        // Guest-memory-backed views are already coherent with JavaScript.
        return false;
    };
    let refresh = if mapping.handle_id != handle_id {
        // Distinct Buffer objects may reuse the same pooled ArrayBuffer range.
        // Only the originating handle is coherent with an in-frame guest copy.
        true
    } else {
        mapping.owner_id != env.data().current_host_buffer_owner()
    };
    // Standard N-API pointers are writable. The JS backend cannot observe
    // Wasm stores directly, so the mapping is conservatively dirty until the
    // next guest-to-JavaScript coherence boundary.
    env.data_mut().host_buffer_copies[index].needs_flush = true;
    refresh
}

fn resolve_current_host_data_to_guest(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_env: u32,
    handle_id: u32,
    backing_store_token: u64,
    host_addr: u64,
    byte_len: usize,
    covers_full_backing_store: bool,
) -> Option<u32> {
    if backing_store_token != 0
        && let Some(guest_data_ptr) = resolve_guest_backing_store_token(
            env.data(),
            guest_env,
            handle_id,
            backing_store_token,
            host_addr,
            byte_len,
        )
    {
        // JavaScript may mutate a retained Buffer/TypedArray between native
        // calls. Refresh an existing guest-side cache before exposing its
        // pointer again; the backing-store token identifies the allocation,
        // not a particular version of its bytes.
        if prepare_cached_guest_data(env, guest_env, handle_id, guest_data_ptr) && host_addr != 0 {
            let snapshot = unsafe { std::slice::from_raw_parts(host_addr as *const u8, byte_len) };
            let _ = write_guest_bytes(env, guest_data_ptr, snapshot);
        }
        env.data_mut()
            .guest_data_ptrs
            .insert((guest_env, handle_id), guest_data_ptr);
        return Some(guest_data_ptr);
    }
    if let Some(&guest_data_ptr) = env.data().guest_data_ptrs.get(&(guest_env, handle_id)) {
        if prepare_cached_guest_data(env, guest_env, handle_id, guest_data_ptr) && host_addr != 0 {
            let snapshot = unsafe { std::slice::from_raw_parts(host_addr as *const u8, byte_len) };
            let _ = write_guest_bytes(env, guest_data_ptr, snapshot);
        }
        return Some(guest_data_ptr);
    }
    if host_addr == 0 {
        return Some(0);
    }
    if byte_len == 0 {
        return Some(0);
    }

    resolve_or_copy_host_data_to_guest(
        env,
        guest_env,
        handle_id,
        backing_store_token,
        host_addr,
        byte_len,
        covers_full_backing_store,
    )
}

fn resolve_current_host_value_to_guest(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_env: u32,
    handle_id: u32,
    backing_store_token: u64,
    byte_len: usize,
    covers_full_backing_store: bool,
) -> Option<u32> {
    let cached = if backing_store_token != 0 {
        resolve_guest_backing_store_token(
            env.data(),
            guest_env,
            handle_id,
            backing_store_token,
            0,
            byte_len,
        )
    } else {
        None
    }
    .or_else(|| {
        env.data()
            .guest_data_ptrs
            .get(&(guest_env, handle_id))
            .copied()
    });

    if let Some(guest_ptr) = cached {
        if prepare_cached_guest_data(env, guest_env, handle_id, guest_ptr) {
            let memory = env.data().memory.clone()?;
            let memory_buffer = memory.as_js().js_buffer();
            if crate::snapi_js::copy_value_bytes_to_memory(
                snapi_env(env, guest_env),
                handle_id,
                &memory_buffer,
                guest_ptr,
                byte_len as u32,
            ) != 0
            {
                return None;
            }
        }
        env.data_mut()
            .guest_data_ptrs
            .insert((guest_env, handle_id), guest_ptr);
        return Some(guest_ptr);
    }

    if byte_len == 0 {
        return Some(0);
    }
    let guest_ptr = allocate_guest_buffer(env, byte_len)?;
    let memory = env.data().memory.clone()?;
    let memory_buffer = memory.as_js().js_buffer();
    if crate::snapi_js::copy_value_bytes_to_memory(
        snapi_env(env, guest_env),
        handle_id,
        &memory_buffer,
        guest_ptr,
        byte_len as u32,
    ) != 0
    {
        recycle_guest_allocation(env, guest_ptr);
        return None;
    }
    if !remember_host_buffer_copy(
        env,
        guest_env,
        handle_id,
        backing_store_token,
        0,
        guest_ptr,
        byte_len,
        true,
        false,
        true,
        covers_full_backing_store,
    ) {
        recycle_guest_allocation(env, guest_ptr);
        return None;
    }
    Some(guest_ptr)
}

fn guest_unofficial_napi_configure_runtime(
    mut env: FunctionEnvMut<NapiEnv>,
    options_ptr: u32,
) -> i32 {
    let Some(flags) = abi::read_runtime_options(&mut env, options_ptr) else {
        return 1;
    };
    let Ok(flags) = CString::new(flags) else {
        return 1;
    };
    unsafe {
        snapi_bridge_unofficial_configure_runtime(flags.as_ptr(), flags.as_bytes().len() as u32)
    }
}

fn guest_unofficial_napi_create_env(
    mut env: FunctionEnvMut<NapiEnv>,
    module_api_version: i32,
    options_ptr: u32,
    env_out_ptr: u32,
    scope_out_ptr: u32,
) -> i32 {
    let (
        total_memory,
        constrained_memory,
        max_young_generation_size_in_bytes,
        max_old_generation_size_in_bytes,
        code_range_size_in_bytes,
        stack_limit,
    ) = if options_ptr != 0 {
        let Some(options) = abi::read_env_create(&mut env, options_ptr) else {
            return 1;
        };
        (
            options.total_memory,
            options.constrained_memory,
            options.max_young_generation_size_in_bytes,
            options.max_old_generation_size_in_bytes,
            options.code_range_size_in_bytes,
            options.stack_limit,
        )
    } else {
        (0, 0, 0, 0, 0, 0)
    };

    let mut snapi_env_state: SnapiEnv = std::ptr::null_mut();
    let status = unsafe {
        snapi_bridge_unofficial_create_env_with_options(
            module_api_version,
            total_memory,
            constrained_memory,
            max_young_generation_size_in_bytes,
            max_old_generation_size_in_bytes,
            code_range_size_in_bytes,
            stack_limit,
            std::ptr::null(),
            &mut snapi_env_state,
        )
    };
    if status != 0 {
        return status;
    }
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    let async_env = env.as_async_mut();
    let (env_id, scope_id) = env.data_mut().register_napi_env(snapi_env_state);
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    if let Some(async_env) = async_env {
        install_persistent_callback_state(&mut env, env_id, snapi_env_state, async_env);
    }
    if env_out_ptr != 0 {
        write_guest_u32(&mut env, env_out_ptr, env_id);
    }
    if scope_out_ptr != 0 {
        write_guest_u32(&mut env, scope_out_ptr, scope_id);
    }
    0
}

fn drain_env_guest_finalizers(env: &mut FunctionEnvMut<NapiEnv>, scope_id: u32) -> i32 {
    let guest_env = env.data().napi_scopes.get(&scope_id).copied().unwrap_or(0);
    let snapi_env_state = env
        .data()
        .napi_scopes
        .get(&scope_id)
        .and_then(|guest_env| env.data().napi_envs.get(guest_env))
        .copied()
        .map(|state| state as SnapiEnv)
        .unwrap_or(std::ptr::null_mut());
    if !snapi_env_state.is_null() {
        publish_pending_host_buffer_copies(env, snapi_env_state);
        let status = with_callback_state(env, snapi_env_state, || unsafe {
            crate::snapi::snapi_bridge_drain_all_guest_finalizers(snapi_env_state)
        });
        refresh_host_buffer_copies(env, snapi_env_state);
        drain_ready_guest_buffers(env, guest_env, snapi_env_state, true);
        return status;
    }
    0
}

fn guest_unofficial_napi_release_env(
    mut env: FunctionEnvMut<NapiEnv>,
    scope_ptr: u32,
    loop_ptr: u32,
) -> i32 {
    let scope_id = scope_ptr;
    let status = drain_env_guest_finalizers(&mut env, scope_id);
    if status != 0 {
        return status;
    }
    let Some(snapi_env_state) = env.data_mut().unregister_napi_scope(scope_id) else {
        return 1;
    };
    let loop_id = loop_ptr;
    unsafe { snapi_bridge_unofficial_release_env_with_loop(snapi_env_state, loop_id) }
}

fn guest_unofficial_napi_attach_env(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    hooks_ptr: u32,
    accepted_hooks_ptr: u32,
) -> i32 {
    let Some(hooks) = abi::read_env_hooks(&mut env, hooks_ptr) else {
        return 1;
    };
    if napi_env == 0 || accepted_hooks_ptr == 0 {
        return 1;
    }
    let mut accepted_hooks = 0u64;
    let status = unsafe {
        snapi_bridge_unofficial_attach_env(
            snapi_env(&env, napi_env),
            hooks.fatal,
            hooks.oom,
            &mut accepted_hooks,
        )
    };
    if status == 0 {
        write_guest_u64(&mut env, accepted_hooks_ptr, accepted_hooks);
        debug_assert_eq!(accepted_hooks & !hooks.requested, 0);
    }
    status
}

fn guest_unofficial_napi_collect_garbage(env: FunctionEnvMut<NapiEnv>, napi_env: u32) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    unsafe { snapi_bridge_unofficial_collect_garbage(env_handle) }
}

fn provider_event_loop_checkpoint_sync(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    mode: i32,
    has_runnable_work: i32,
    checkpoint_state_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut checkpoint_state = 0;
    let status = with_cb_context(&mut env, napi_env, || unsafe {
        let status = snapi_bridge_drain_pending_callbacks(env_handle);
        if status != 0 {
            return status;
        }
        snapi_bridge_unofficial_event_loop_checkpoint(
            env_handle,
            mode,
            has_runnable_work,
            &mut checkpoint_state,
        )
    });
    drain_ready_guest_buffers(&mut env, napi_env, env_handle, false);
    if checkpoint_state_ptr != 0 {
        write_guest_u32(&mut env, checkpoint_state_ptr, checkpoint_state);
    }
    status
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
async fn event_loop_checkpoint(
    allow_host_tasks: bool,
    has_runnable_work: bool,
) -> Result<JsValue, JsValue> {
    crate::snapi_js::event_loop_checkpoint(allow_host_tasks, has_runnable_work).await
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
async fn guest_unofficial_napi_event_loop_checkpoint(
    mut env: AsyncFunctionEnvMut<NapiEnv>,
    napi_env: u32,
    mode: i32,
    has_runnable_work: i32,
    checkpoint_state_ptr: u32,
) -> i32 {
    // The host JavaScript realm owns its Promise and task queues. One explicit
    // JSPI suspension is the only async scheduling primitive exposed to Edge.
    let env_handle = {
        let env = env.read().await;
        env.data().resolve_napi_env(napi_env)
    };
    let (status, checkpoint_state) =
        with_callback_state_async(env.as_mut(), env_handle, async move {
            if mode != 0 && mode != 1 {
                return (1, 0);
            }
            if event_loop_checkpoint(mode == 1, has_runnable_work != 0)
                .await
                .is_err()
            {
                return (9, 0);
            }
            let status = unsafe { snapi_bridge_drain_pending_callbacks(env_handle) };
            if status != 0 {
                return (status, 0);
            }
            let mut checkpoint_state = 0;
            let status = unsafe {
                snapi_bridge_unofficial_event_loop_checkpoint(
                    env_handle,
                    mode,
                    has_runnable_work,
                    &mut checkpoint_state,
                )
            };
            if status == 0 && mode == 1 {
                checkpoint_state |= 1 << 1;
            }
            (status, checkpoint_state)
        })
        .await;
    {
        let mut locked = env.write().await;
        let mut sync_env = locked.as_function_env_mut();
        drain_ready_guest_buffers(&mut sync_env, napi_env, env_handle, false);
        if checkpoint_state_ptr != 0 {
            write_guest_u32(&mut sync_env, checkpoint_state_ptr, checkpoint_state);
        }
    }
    status
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
fn event_loop_checkpoint_import(
    store: &mut impl AsStoreMut,
    fe: &FunctionEnv<NapiEnv>,
) -> Function {
    Function::new_typed_with_env_async(store, fe, guest_unofficial_napi_event_loop_checkpoint)
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
fn guest_unofficial_napi_event_loop_checkpoint_sync(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    mode: i32,
    has_runnable_work: i32,
    checkpoint_state_ptr: u32,
) -> i32 {
    provider_event_loop_checkpoint_sync(
        env,
        napi_env,
        mode,
        has_runnable_work,
        checkpoint_state_ptr,
    )
}

fn guest_unofficial_napi_create_uninitialized_arraybuffer(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    length: u32,
    _zero_fill: i32,
    result_ptr: u32,
) -> i32 {
    // JavaScript ArrayBuffers are engine-owned and zero-initialized. Avoid a
    // guest snapshot entirely when Edge does not request a native pointer.
    guest_napi_create_arraybuffer(env, napi_env, length, 0, result_ptr)
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
fn event_loop_checkpoint_import(
    store: &mut impl AsStoreMut,
    fe: &FunctionEnv<NapiEnv>,
) -> Function {
    Function::new_typed_with_env(store, fe, guest_unofficial_napi_event_loop_checkpoint_sync)
}

fn guest_unofficial_napi_get_promise_details(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    promise: u32,
    state_ptr: u32,
    result_ptr: u32,
    has_result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let promise_id = promise;
    let mut state = 0i32;
    let mut result_id = 0u32;
    let mut has_result = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_get_promise_details(
            env_handle,
            promise_id,
            &mut state,
            &mut result_id,
            &mut has_result,
        )
    };
    if status != 0 {
        return status;
    }
    if state_ptr != 0 {
        write_guest_i32(&mut env, state_ptr, state);
    }
    if result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, result_id);
    }
    if has_result_ptr != 0 {
        write_guest_u8(&mut env, has_result_ptr, (has_result != 0) as u8);
    }
    0
}

fn guest_unofficial_napi_get_proxy_details(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    proxy: u32,
    target_ptr: u32,
    handler_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let proxy_id = proxy;
    let mut target_id = 0u32;
    let mut handler_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_get_proxy_details(
            env_handle,
            proxy_id,
            &mut target_id,
            &mut handler_id,
        )
    };
    if status != 0 {
        return status;
    }
    if target_ptr != 0 {
        write_guest_u32(&mut env, target_ptr, target_id);
    }
    if handler_ptr != 0 {
        write_guest_u32(&mut env, handler_ptr, handler_id);
    }
    0
}

fn guest_unofficial_napi_preview_entries(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    value: u32,
    entries_ptr: u32,
    is_key_value_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let value_id = value;
    let mut entries_id = 0u32;
    let mut is_key_value = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_preview_entries(
            env_handle,
            value_id,
            &mut entries_id,
            &mut is_key_value,
        )
    };
    if status != 0 {
        return status;
    }
    if entries_ptr != 0 {
        write_guest_u32(&mut env, entries_ptr, entries_id);
    }
    if is_key_value_ptr != 0 {
        write_guest_u8(&mut env, is_key_value_ptr, (is_key_value != 0) as u8);
    }
    0
}

fn guest_unofficial_napi_get_call_sites(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    frames: u32,
    callsites_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut callsites_id = 0u32;
    let status =
        unsafe { snapi_bridge_unofficial_get_call_sites(env_handle, frames, &mut callsites_id) };
    if status == 0 && callsites_ptr != 0 {
        write_guest_u32(&mut env, callsites_ptr, callsites_id);
    }
    status
}

fn guest_unofficial_napi_arraybuffer_view_has_buffer(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    value: u32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let value_id = value;
    let mut result = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_arraybuffer_view_has_buffer(env_handle, value_id, &mut result)
    };
    if status == 0 && result_ptr != 0 {
        write_guest_u8(&mut env, result_ptr, (result != 0) as u8);
    }
    status
}

fn guest_unofficial_napi_get_constructor_name(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    value: u32,
    name_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let value_id = value;
    let mut name_id = 0u32;
    let status =
        unsafe { snapi_bridge_unofficial_get_constructor_name(env_handle, value_id, &mut name_id) };
    if status == 0 && name_ptr != 0 {
        write_guest_u32(&mut env, name_ptr, name_id);
    }
    status
}

fn guest_unofficial_napi_create_private_symbol(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    desc_ptr: u32,
    length: u32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let wl = length;
    let desc = if desc_ptr != 0 {
        if wl == 0xFFFFFFFFu32 {
            read_guest_c_string(&mut env, desc_ptr)
        } else {
            read_guest_bytes(&mut env, desc_ptr, wl as usize)
        }
    } else {
        Some(Vec::new())
    };
    let Some(desc) = desc else {
        return 1;
    };
    let desc_len = desc.len() as u32;
    let cs = CString::new(desc).unwrap_or_default();
    let mut out = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_create_private_symbol(env_handle, cs.as_ptr(), desc_len, &mut out)
    };
    if status == 0 && result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, out);
    }
    status
}

fn guest_unofficial_napi_get_continuation_preserved_embedder_data(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut out = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_get_continuation_preserved_embedder_data(env_handle, &mut out)
    };
    if status == 0 && result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, out);
    }
    status
}

fn guest_unofficial_napi_set_prepare_stack_trace_callback(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    callback: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let callback_id = callback;
    unsafe { snapi_bridge_unofficial_set_prepare_stack_trace_callback(env_handle, callback_id) }
}

fn guest_unofficial_napi_set_continuation_preserved_embedder_data(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    value: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let value_id = value;
    unsafe {
        snapi_bridge_unofficial_set_continuation_preserved_embedder_data(env_handle, value_id)
    }
}

fn guest_unofficial_napi_notify_datetime_configuration_change(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    unsafe { snapi_bridge_unofficial_notify_datetime_configuration_change(env_handle) }
}

fn guest_unofficial_napi_terminate_execution(env: FunctionEnvMut<NapiEnv>, napi_env: u32) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    unsafe { snapi_bridge_unofficial_terminate_execution(env_handle) }
}

fn guest_unofficial_napi_cancel_terminate_execution(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    unsafe { snapi_bridge_unofficial_cancel_terminate_execution(env_handle) }
}

fn guest_unofficial_napi_request_interrupt(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    callback: u32,
    data: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let callback_id = callback;
    let data_val = data;
    with_cb_context(&mut env, napi_env, || unsafe {
        snapi_bridge_unofficial_request_interrupt(env_handle, napi_env, callback_id, data_val)
    })
}

fn guest_unofficial_napi_structured_clone(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    value: u32,
    transfer_list: u32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let value_id = value;
    let transfer_list_id = transfer_list;
    let mut out = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_structured_clone(env_handle, value_id, transfer_list_id, &mut out)
    };
    if status == 0 && result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, out);
    }
    status
}

fn guest_unofficial_napi_message_create(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    value: u32,
    payload_out_ptr: u32,
) -> i32 {
    if payload_out_ptr == 0 {
        return 1;
    }
    let env_handle = snapi_env(&env, napi_env);
    let mut payload = 0u32;
    let status = unsafe { snapi_bridge_unofficial_message_create(env_handle, value, &mut payload) };
    if status == 0 && payload_out_ptr != 0 {
        write_guest_u32(&mut env, payload_out_ptr, payload);
    }
    status
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
async fn guest_unofficial_napi_message_take_async(
    mut env: AsyncFunctionEnvMut<NapiEnv>,
    napi_env: u32,
    payload: u32,
    result_out_ptr: u32,
) -> i32 {
    if payload == 0 {
        return 1;
    }
    if result_out_ptr == 0 {
        unsafe { snapi_bridge_unofficial_message_drop(payload) };
        return 1;
    }
    let env_handle = {
        let env = env.read().await;
        env.data().resolve_napi_env(napi_env)
    };
    let (status, value) = with_callback_state_async(env.as_mut(), env_handle, async move {
        if crate::snapi_js::wait_for_message(payload).await.is_err() {
            unsafe { snapi_bridge_unofficial_message_drop(payload) };
            return (9, 0);
        }
        let mut value = 0u32;
        let status =
            unsafe { snapi_bridge_unofficial_message_take(env_handle, payload, &mut value) };
        (status, value)
    })
    .await;
    if status == 0 && result_out_ptr != 0 {
        let mut locked = env.write().await;
        let mut sync_env = locked.as_function_env_mut();
        write_guest_u32(&mut sync_env, result_out_ptr, value);
    }
    status
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
fn guest_unofficial_napi_message_take_sync(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    payload: u32,
    result_out_ptr: u32,
) -> i32 {
    if payload == 0 {
        return 1;
    }
    if result_out_ptr == 0 {
        unsafe { snapi_bridge_unofficial_message_drop(payload) };
        return 1;
    }
    let env_handle = snapi_env(&env, napi_env);
    let mut value = 0u32;
    let status = unsafe { snapi_bridge_unofficial_message_take(env_handle, payload, &mut value) };
    if status == 0 && result_out_ptr != 0 {
        write_guest_u32(&mut env, result_out_ptr, value);
    }
    status
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
fn message_take_import(store: &mut impl AsStoreMut, fe: &FunctionEnv<NapiEnv>) -> Function {
    Function::new_typed_with_env_async(store, fe, guest_unofficial_napi_message_take_async)
}

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
fn message_take_import(store: &mut impl AsStoreMut, fe: &FunctionEnv<NapiEnv>) -> Function {
    Function::new_typed_with_env(store, fe, guest_unofficial_napi_message_take_sync)
}

fn guest_unofficial_napi_message_drop(_env: FunctionEnvMut<NapiEnv>, payload: u32) {
    unsafe { snapi_bridge_unofficial_message_drop(payload) };
}

fn guest_unofficial_napi_enqueue_microtask(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    callback: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let callback_id = callback;
    unsafe { snapi_bridge_unofficial_enqueue_microtask(env_handle, callback_id) }
}

fn guest_unofficial_napi_set_promise_reject_callback(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    callback: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let callback_id = callback;
    unsafe { snapi_bridge_unofficial_set_promise_reject_callback(env_handle, callback_id) }
}

fn guest_unofficial_napi_get_own_non_index_properties(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    value: u32,
    filter_bits: u32,
    result_out_ptr: u32,
) -> i32 {
    if value == 0 || result_out_ptr == 0 {
        return 1;
    }
    let env_handle = snapi_env(&env, napi_env);
    let mut out = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_get_own_non_index_properties(
            env_handle,
            value,
            filter_bits,
            &mut out,
        )
    };
    if status == 0 {
        write_guest_u32(&mut env, result_out_ptr, out);
    }
    status
}

fn guest_unofficial_napi_set_promise_hooks(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    init_callback: u32,
    before_callback: u32,
    after_callback: u32,
    resolve_callback: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    unsafe {
        snapi_bridge_unofficial_set_promise_hooks(
            env_handle,
            init_callback,
            before_callback,
            after_callback,
            resolve_callback,
        )
    }
}

fn guest_unofficial_napi_get_hash_seed(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    hash_seed_out: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut hash_seed = 0u64;
    let status = unsafe { snapi_bridge_unofficial_get_hash_seed(env_handle, &mut hash_seed) };
    if status == 0 && hash_seed_out != 0 {
        write_guest_u64(&mut env, hash_seed_out, hash_seed);
    }
    status
}

fn guest_unofficial_napi_get_error_metadata(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    error: u32,
    mode: i32,
    metadata_ptr: u32,
) -> i32 {
    // The whole 28-byte metadata struct must lie below 4 GiB, so the field
    // offsets below cannot wrap.
    if metadata_ptr != 0 && u64::from(metadata_ptr) + 28 > 1u64 << 32 {
        return 1;
    }
    let env_handle = snapi_env(&env, napi_env);
    let error_id = error;
    let mut source_line_id = 0u32;
    let mut script_resource_name_id = 0u32;
    let mut stderr_line_id = 0u32;
    let mut thrown_at_id = 0u32;
    let mut line_number = 0i32;
    let mut start_column = 0i32;
    let mut end_column = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_get_error_metadata(
            env_handle,
            error_id,
            mode,
            &mut source_line_id,
            &mut script_resource_name_id,
            &mut stderr_line_id,
            &mut thrown_at_id,
            &mut line_number,
            &mut start_column,
            &mut end_column,
        )
    };
    if status != 0 {
        return status;
    }
    if metadata_ptr != 0 {
        write_guest_u32(&mut env, metadata_ptr, source_line_id);
        write_guest_u32(&mut env, metadata_ptr + 4, script_resource_name_id);
        write_guest_u32(&mut env, metadata_ptr + 8, stderr_line_id);
        write_guest_u32(&mut env, metadata_ptr + 12, thrown_at_id);
        write_guest_i32(&mut env, metadata_ptr + 16, line_number);
        write_guest_i32(&mut env, metadata_ptr + 20, start_column);
        write_guest_i32(&mut env, metadata_ptr + 24, end_column);
    }
    0
}

fn guest_unofficial_napi_configure_source_maps(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    enabled: i32,
    callback: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let callback_id = callback;
    unsafe { snapi_bridge_unofficial_configure_source_maps(env_handle, enabled, callback_id) }
}

fn guest_unofficial_napi_preserve_error_source_message(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    error: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let error_id = error;
    unsafe { snapi_bridge_unofficial_preserve_error_source_message(env_handle, error_id) }
}

fn guest_unofficial_napi_mark_promise_as_handled(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    promise: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let promise_id = promise;
    unsafe { snapi_bridge_unofficial_mark_promise_as_handled(env_handle, promise_id) }
}

fn guest_unofficial_napi_get_heap_statistics(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    stats_ptr: u32,
) -> i32 {
    let Some(size) = abi::read_output_header(
        &mut env,
        stats_ptr,
        std::mem::size_of::<SnapiUnofficialHeapStatistics>(),
        1,
    ) else {
        return 1;
    };
    let env_handle = snapi_env(&env, napi_env);
    let mut stats = SnapiUnofficialHeapStatistics {
        size,
        version: 1,
        valid_fields: 0,
        total_heap_size: 0,
        total_heap_size_executable: 0,
        total_physical_size: 0,
        total_available_size: 0,
        used_heap_size: 0,
        heap_size_limit: 0,
        does_zap_garbage: 0,
        malloced_memory: 0,
        peak_malloced_memory: 0,
        number_of_native_contexts: 0,
        number_of_detached_contexts: 0,
        total_global_handles_size: 0,
        used_global_handles_size: 0,
        external_memory: 0,
        array_buffer_memory: 0,
    };
    let status = unsafe { snapi_bridge_unofficial_get_heap_statistics(env_handle, &mut stats) };
    if status == 0 && stats_ptr != 0 && !write_guest_pod(&mut env, stats_ptr, &stats) {
        return 1;
    }
    status
}

fn guest_unofficial_napi_get_heap_space_statistics(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    stats_ptr: u32,
    capacity: u32,
    count_ptr: u32,
) -> i32 {
    const MAX_HEAP_SPACE_CAPACITY: usize = 1024;
    if count_ptr == 0 || (capacity != 0 && stats_ptr == 0) {
        return 1;
    }
    let capacity = capacity as usize;
    if capacity > MAX_HEAP_SPACE_CAPACITY {
        return 1;
    }
    let env_handle = snapi_env(&env, napi_env);
    let mut stats: Vec<SnapiUnofficialHeapSpaceStatistics> = (0..capacity)
        .map(|_| SnapiUnofficialHeapSpaceStatistics {
            space_name: [0; 64],
            space_size: 0,
            space_used_size: 0,
            space_available_size: 0,
            physical_space_size: 0,
        })
        .collect();
    let mut count = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_get_heap_space_statistics(
            env_handle,
            if stats.is_empty() {
                std::ptr::null_mut()
            } else {
                stats.as_mut_ptr()
            },
            capacity as u32,
            &mut count,
        )
    };
    if status == 0 {
        if !write_guest_u32(&mut env, count_ptr, count) {
            return 1;
        }
        let written = capacity.min(count as usize);
        if !write_guest_pod_slice(&mut env, stats_ptr, &stats[..written]) {
            return 1;
        }
    }
    status
}

fn guest_unofficial_napi_get_heap_code_statistics(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    stats_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut stats = SnapiUnofficialHeapCodeStatistics {
        code_and_metadata_size: 0,
        bytecode_and_metadata_size: 0,
        external_script_source_size: 0,
        cpu_profiler_metadata_size: 0,
    };
    let status =
        unsafe { snapi_bridge_unofficial_get_heap_code_statistics(env_handle, &mut stats) };
    if status == 0 && stats_ptr != 0 && !write_guest_pod(&mut env, stats_ptr, &stats) {
        return 1;
    }
    status
}

fn guest_unofficial_napi_configure_near_heap_limit_callback(
    _env: FunctionEnvMut<NapiEnv>,
    _napi_env: u32,
    _callback: u32,
    _data: u32,
    _restored_heap_limit: u32,
) -> i32 {
    0
}

fn guest_unofficial_napi_profile_start(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    kind: i32,
    result_ptr: u32,
    profile_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut result = 0i32;
    let mut profile = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_profile_start(env_handle, kind, &mut result, &mut profile)
    };
    if status != 0 {
        return status;
    }
    if result_ptr != 0 {
        write_guest_i32(&mut env, result_ptr, result);
    }
    if profile_ptr != 0 {
        write_guest_u32(&mut env, profile_ptr, profile);
    }
    0
}

fn guest_unofficial_napi_profile_stop(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    profile: u32,
    json_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut json = 0u32;
    let status = unsafe { snapi_bridge_unofficial_profile_stop(env_handle, profile, &mut json) };
    if status != 0 {
        return status;
    }
    if json_ptr != 0 && !write_guest_u32(&mut env, json_ptr, json) {
        return 1;
    }
    0
}

fn guest_unofficial_napi_take_heap_snapshot(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    options_ptr: u32,
    json_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let (expose_internals, expose_numeric_values) = if options_ptr != 0 {
        let Some(bytes) = read_guest_bytes(&mut env, options_ptr, 2) else {
            return 1;
        };
        ((bytes[0] != 0) as i32, (bytes[1] != 0) as i32)
    } else {
        (0, 0)
    };
    let mut json = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_take_heap_snapshot(
            env_handle,
            expose_internals,
            expose_numeric_values,
            &mut json,
        )
    };
    if status != 0 {
        return status;
    }
    if json_ptr != 0 && !write_guest_u32(&mut env, json_ptr, json) {
        return 1;
    }
    0
}

fn guest_unofficial_napi_create_serdes_binding(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut out = 0u32;
    let status = unsafe { snapi_bridge_unofficial_create_serdes_binding(env_handle, &mut out) };
    if status == 0 && result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, out);
    }
    status
}

fn guest_napi_add_env_cleanup_hook(
    _env: FunctionEnvMut<NapiEnv>,
    _napi_env: u32,
    _fun: u32,
    _arg: u32,
) -> i32 {
    0
}

fn guest_napi_remove_env_cleanup_hook(
    _env: FunctionEnvMut<NapiEnv>,
    _napi_env: u32,
    _fun: u32,
    _arg: u32,
) -> i32 {
    0
}

fn guest_unofficial_napi_contextify_contains_module_syntax(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    code: u32,
    filename: u32,
    resource_name_or_undefined: u32,
    cjs_var_in_scope: i32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let code_id = code;
    let filename_id = filename;
    let resource_name_id = resource_name_or_undefined;
    let mut result = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_contextify_contains_module_syntax(
            env_handle,
            code_id,
            filename_id,
            resource_name_id,
            cjs_var_in_scope,
            &mut result,
        )
    };
    if status == 0 && result_ptr != 0 {
        write_guest_u8(&mut env, result_ptr, (result != 0) as u8);
    }
    status
}

// A guest realm id is the value id of the realm's global (its sandbox, when it
// has one); 0 means the env's own realm.

fn guest_unofficial_napi_create_realm(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    options_ptr: u32,
    realm_out_ptr: u32,
    global_out_ptr: u32,
) -> i32 {
    let Some(options) = abi::read_realm_options(&mut env, options_ptr) else {
        return 1;
    };
    if realm_out_ptr == 0 {
        return 1;
    }
    let mut realm = 0u32;
    let mut global = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_create_realm(
            snapi_env(&env, napi_env),
            options.sandbox_or_null,
            options.name,
            options.origin,
            options.host_defined_option_id,
            i32::from(options.allow_code_gen_strings),
            i32::from(options.allow_code_gen_wasm),
            i32::from(options.own_microtask_queue),
            &mut realm,
            &mut global,
        )
    };
    if status != 0 {
        return status;
    }
    write_guest_u32(&mut env, realm_out_ptr, realm);
    if global_out_ptr != 0 {
        write_guest_u32(&mut env, global_out_ptr, global);
    }
    0
}

fn guest_unofficial_napi_get_realm(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    value: u32,
    realm_out_ptr: u32,
) -> i32 {
    if realm_out_ptr == 0 {
        return 1;
    }
    let mut realm = 0u32;
    let status =
        unsafe { snapi_bridge_unofficial_get_realm(snapi_env(&env, napi_env), value, &mut realm) };
    if status == 0 {
        write_guest_u32(&mut env, realm_out_ptr, realm);
    }
    status
}

/// `realm` is a guest realm id (0 for the env's own realm). The scope token is
/// the provider's, written as a u64.
fn guest_unofficial_napi_enter_realm(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    realm: u32,
    scope_out_ptr: u32,
) -> i32 {
    if scope_out_ptr == 0 {
        return 1;
    }
    let env_handle = snapi_env(&env, napi_env);
    let mut scope = 0u64;
    let status = unsafe { snapi_bridge_unofficial_enter_realm(env_handle, realm, &mut scope) };
    if status == 0 && !write_guest_u64(&mut env, scope_out_ptr, scope) {
        // The guest never learns the token, so close the scope again.
        unsafe { snapi_bridge_unofficial_leave_realm(env_handle, scope) };
        return 1;
    }
    status
}

fn guest_unofficial_napi_leave_realm(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    scope: i64,
) -> i32 {
    unsafe { snapi_bridge_unofficial_leave_realm(snapi_env(&env, napi_env), scope as u64) }
}

#[allow(clippy::too_many_arguments)]
fn guest_unofficial_napi_run_script(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    realm: u32,
    source_ptr: u32,
    filename: u32,
    line_offset: i32,
    column_offset: i32,
    timeout: i64,
    display_errors: i32,
    break_on_sigint: i32,
    break_on_first_line: i32,
    host_defined_option_id: u32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let Some((source_text, source_bytecode)) = abi::read_js_source(&mut env, source_ptr) else {
        return 1;
    };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_run_script(
            env_handle,
            realm,
            source_text,
            source_bytecode,
            filename,
            line_offset,
            column_offset,
            timeout,
            display_errors,
            break_on_sigint,
            break_on_first_line,
            host_defined_option_id,
            &mut result_id,
        )
    };
    if status == 0 && result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, result_id);
    }
    status
}

#[allow(clippy::too_many_arguments)]
fn guest_unofficial_napi_compile_function(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    realm: u32,
    source_ptr: u32,
    filename: u32,
    line_offset: i32,
    column_offset: i32,
    context_extensions_or_undefined: u32,
    params_or_undefined: u32,
    host_defined_option_id: u32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let Some((source_text, source_bytecode)) = abi::read_js_source(&mut env, source_ptr) else {
        return 1;
    };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_compile_function(
            env_handle,
            realm,
            source_text,
            source_bytecode,
            filename,
            line_offset,
            column_offset,
            context_extensions_or_undefined,
            params_or_undefined,
            host_defined_option_id,
            &mut result_id,
        )
    };
    if status == 0 && result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, result_id);
    }
    status
}

fn guest_unofficial_napi_bytecode_open(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    options_ptr: u32,
    result_ptr: u32,
) -> i32 {
    let Some(options) = abi::read_bytecode_open(&mut env, options_ptr) else {
        return 1;
    };
    // The whole 6-byte result must lie below 4 GiB (checked before
    // anything is created), so the field offsets below cannot wrap.
    if result_ptr == 0 || u64::from(result_ptr) + 6 > 1u64 << 32 {
        return 1;
    }
    let env_handle = snapi_env(&env, napi_env);
    let mut bytecode_id = 0u32;
    let mut cache_rejected = 0u8;
    let mut can_parse_as_module = 0u8;
    let status = unsafe {
        snapi_bridge_unofficial_bytecode_open(
            env_handle,
            options.realm_or_null,
            options.source_text,
            options.filename,
            options.shape,
            options.params_or_undefined,
            options.host_defined_option_id,
            options.line_offset,
            options.column_offset,
            if options.has_cache != 0 {
                options.cache.as_ptr()
            } else {
                std::ptr::null()
            },
            options.cache.len(),
            options.has_cache,
            options.cache_policy,
            &mut bytecode_id,
            &mut cache_rejected,
            &mut can_parse_as_module,
        )
    };
    write_guest_u32(&mut env, result_ptr, bytecode_id);
    write_guest_u8(&mut env, result_ptr + 4, cache_rejected);
    write_guest_u8(&mut env, result_ptr + 5, can_parse_as_module);
    status
}

fn guest_unofficial_napi_bytecode_serialize(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    bytecode: u32,
    buffer_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut buffer_id = 0u32;
    let status =
        unsafe { snapi_bridge_unofficial_bytecode_serialize(env_handle, bytecode, &mut buffer_id) };
    if status == 0 && buffer_ptr != 0 {
        write_guest_u32(&mut env, buffer_ptr, buffer_id);
    }
    status
}

fn guest_unofficial_napi_bytecode_release(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    bytecode: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    unsafe { snapi_bridge_unofficial_bytecode_release(env_handle, bytecode) }
}

fn guest_unofficial_napi_module_wrap_create(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    options_ptr: u32,
    result_ptr: u32,
) -> i32 {
    let Some(options) = abi::read_module_create(&mut env, options_ptr) else {
        return 1;
    };
    // The whole 9-byte result must lie below 4 GiB (checked before
    // anything is created), so the field offsets below cannot wrap.
    if result_ptr == 0 || u64::from(result_ptr) + 9 > 1u64 << 32 {
        return 1;
    }
    let env_handle = snapi_env(&env, napi_env);
    let mut handle_id = 0u32;
    let mut requests_id = 0u32;
    let mut has_top_level_await = 0u8;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_create(
            env_handle,
            options.kind,
            options.wrapper,
            options.url,
            options.realm_or_null,
            options.source_text,
            options.source_bytecode,
            options.line_offset,
            options.column_offset,
            options.host_defined_option_id,
            options.export_names,
            options.synthetic_eval_steps,
            &mut handle_id,
            &mut requests_id,
            &mut has_top_level_await,
        )
    };
    if status == 0 {
        write_guest_u32(&mut env, result_ptr, handle_id);
        write_guest_u32(&mut env, result_ptr + 4, requests_id);
        write_guest_bytes(&mut env, result_ptr + 8, &[has_top_level_await]);
    }
    status
}

fn guest_unofficial_napi_module_wrap_destroy(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    unsafe { snapi_bridge_unofficial_module_wrap_destroy(env_handle, handle) }
}

fn guest_unofficial_napi_module_wrap_link(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
    count: u32,
    linked_handles_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let count_u = count;
    let linked_handles = if count_u > 0 {
        let Some(ids) = read_guest_u32_array(&mut env, linked_handles_ptr, count_u as usize) else {
            return 1;
        };
        ids
    } else {
        Vec::new()
    };
    unsafe {
        snapi_bridge_unofficial_module_wrap_link(
            env_handle,
            handle,
            count_u,
            linked_handles.as_ptr(),
        )
    }
}

fn guest_unofficial_napi_module_wrap_instantiate(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    unsafe { snapi_bridge_unofficial_module_wrap_instantiate(env_handle, handle) }
}

fn guest_unofficial_napi_module_wrap_evaluate(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
    timeout: i64,
    break_on_sigint: i32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut result_id = 0u32;
    let status = with_cb_context(&mut env, napi_env, || unsafe {
        snapi_bridge_unofficial_module_wrap_evaluate(
            env_handle,
            handle,
            timeout,
            break_on_sigint,
            &mut result_id,
        )
    });
    if status == 0 && result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_evaluate_sync(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
    filename: u32,
    parent_filename: u32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut result_id = 0u32;
    let status = with_cb_context(&mut env, napi_env, || unsafe {
        snapi_bridge_unofficial_module_wrap_evaluate_sync(
            env_handle,
            handle,
            filename,
            parent_filename,
            &mut result_id,
        )
    });
    if status == 0 && result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_get_namespace(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_get_namespace(env_handle, handle, &mut result_id)
    };
    if status == 0 && result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_get_state(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
    status_ptr: u32,
    error_ptr: u32,
    has_async_graph_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut status_val = 0i32;
    let mut error_id = 0u32;
    let mut has_async_graph = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_get_state(
            env_handle,
            handle,
            if status_ptr != 0 {
                &mut status_val
            } else {
                std::ptr::null_mut()
            },
            if error_ptr != 0 {
                &mut error_id
            } else {
                std::ptr::null_mut()
            },
            if has_async_graph_ptr != 0 {
                &mut has_async_graph
            } else {
                std::ptr::null_mut()
            },
        )
    };
    if status == 0 {
        if status_ptr != 0 {
            write_guest_i32(&mut env, status_ptr, status_val);
        }
        if error_ptr != 0 {
            write_guest_u32(&mut env, error_ptr, error_id);
        }
        if has_async_graph_ptr != 0 {
            write_guest_u8(&mut env, has_async_graph_ptr, (has_async_graph != 0) as u8);
        }
    }
    status
}

fn guest_unofficial_napi_module_wrap_check_unsettled_top_level_await(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
    warnings: i32,
    settled_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let handle_id = handle;
    let mut settled = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_check_unsettled_top_level_await(
            env_handle,
            handle_id,
            warnings,
            &mut settled,
        )
    };
    if status == 0 && settled_ptr != 0 {
        write_guest_u8(&mut env, settled_ptr, (settled != 0) as u8);
    }
    status
}

fn guest_unofficial_napi_module_wrap_set_export(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
    export_name: u32,
    export_value: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    unsafe {
        snapi_bridge_unofficial_module_wrap_set_export(
            env_handle,
            handle,
            export_name,
            export_value,
        )
    }
}

fn guest_unofficial_napi_module_wrap_set_module_source_object(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
    source_object: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    unsafe {
        snapi_bridge_unofficial_module_wrap_set_module_source_object(
            env_handle,
            handle,
            source_object,
        )
    }
}

fn guest_unofficial_napi_module_wrap_get_module_source_object(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_get_module_source_object(
            env_handle,
            handle,
            &mut result_id,
        )
    };
    if status == 0 && result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_create_cached_data(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_create_cached_data(env_handle, handle, &mut result_id)
    };
    if status == 0 && result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_set_hooks(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    hooks_ptr: u32,
) -> i32 {
    let Some((dynamic_import, initialize_import_meta)) =
        abi::read_module_hooks(&mut env, hooks_ptr)
    else {
        return 1;
    };
    let env_handle = snapi_env(&env, napi_env);
    unsafe {
        snapi_bridge_unofficial_module_wrap_set_hooks(
            env_handle,
            dynamic_import,
            initialize_import_meta,
        )
    }
}

fn guest_unofficial_napi_module_wrap_create_required_module_facade(
    mut env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    handle: u32,
    result_ptr: u32,
) -> i32 {
    let env_handle = snapi_env(&env, napi_env);
    let mut result_id = 0u32;
    let status = with_cb_context(&mut env, napi_env, || unsafe {
        snapi_bridge_unofficial_module_wrap_create_required_module_facade(
            env_handle,
            handle,
            &mut result_id,
        )
    });
    if status == 0 && result_ptr != 0 {
        write_guest_u32(&mut env, result_ptr, result_id);
    }
    status
}

// --- Singleton getters ---

fn guest_napi_get_undefined(mut env: FunctionEnvMut<NapiEnv>, e: u32, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_undefined(snapi_env(&env, e), &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_get_null(mut env: FunctionEnvMut<NapiEnv>, e: u32, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_null(snapi_env(&env, e), &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_get_boolean(mut env: FunctionEnvMut<NapiEnv>, e: u32, value: i32, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_boolean(snapi_env(&env, e), value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_get_global(mut env: FunctionEnvMut<NapiEnv>, e: u32, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_global(snapi_env(&env, e), &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

// --- Value creation ---

fn guest_napi_create_string_utf8(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    str_ptr: u32,
    length: u32,
    rp: u32,
) -> i32 {
    let Some(value) = read_guest_string_value(&mut env, str_ptr, length, GuestStringEncoding::Utf8)
    else {
        return 1;
    };
    let mut out: u32 = 0;
    let s = unsafe { host_js_put_primitive(snapi_env(&env, e), value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_string_latin1(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    str_ptr: u32,
    length: u32,
    rp: u32,
) -> i32 {
    let Some(value) =
        read_guest_string_value(&mut env, str_ptr, length, GuestStringEncoding::Latin1)
    else {
        return 1;
    };
    let mut out: u32 = 0;
    let s = unsafe { host_js_put_primitive(snapi_env(&env, e), value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_int32(mut env: FunctionEnvMut<NapiEnv>, e: u32, value: i32, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_int32(snapi_env(&env, e), value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_uint32(mut env: FunctionEnvMut<NapiEnv>, e: u32, value: u32, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_uint32(snapi_env(&env, e), value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_double(mut env: FunctionEnvMut<NapiEnv>, e: u32, value: f64, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_double(snapi_env(&env, e), value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_int64(mut env: FunctionEnvMut<NapiEnv>, e: u32, value: i64, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_int64(snapi_env(&env, e), value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_object(mut env: FunctionEnvMut<NapiEnv>, e: u32, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_object(snapi_env(&env, e), &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_array(mut env: FunctionEnvMut<NapiEnv>, e: u32, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_array(snapi_env(&env, e), &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_array_with_length(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    length: u32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_array_with_length(snapi_env(&env, e), length, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_symbol(mut env: FunctionEnvMut<NapiEnv>, e: u32, desc: u32, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_symbol(snapi_env(&env, e), desc, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_error(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    code: u32,
    msg: u32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_error(snapi_env(&env, e), code, msg, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_type_error(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    code: u32,
    msg: u32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_type_error(snapi_env(&env, e), code, msg, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_range_error(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    code: u32,
    msg: u32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_range_error(snapi_env(&env, e), code, msg, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_bigint_int64(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    value: i64,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_bigint_int64(snapi_env(&env, e), value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_bigint_uint64(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    value: i64,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let s =
        unsafe { snapi_bridge_create_bigint_uint64(snapi_env(&env, e), value as u64, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_date(mut env: FunctionEnvMut<NapiEnv>, e: u32, time: f64, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_date(snapi_env(&env, e), time, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_create_external(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    data: u32,
    _finalize_cb: u32,
    _finalize_hint: u32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_external(snapi_env(&env, e), data as u64, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

// --- Value reading ---

fn guest_napi_get_value_string_utf8(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    bp: u32,
    bs: u32,
    rp: u32,
) -> i32 {
    let value = match unsafe { host_js_value(snapi_env(&env, e), vh) } {
        Ok(value) => value,
        Err(status) => return status,
    };
    write_guest_string_value(&mut env, &value, bp, bs, rp, GuestStringEncoding::Utf8)
}

fn guest_napi_get_value_string_latin1(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    bp: u32,
    bs: u32,
    rp: u32,
) -> i32 {
    let value = match unsafe { host_js_value(snapi_env(&env, e), vh) } {
        Ok(value) => value,
        Err(status) => return status,
    };
    write_guest_string_value(&mut env, &value, bp, bs, rp, GuestStringEncoding::Latin1)
}

fn guest_napi_get_value_int32(mut env: FunctionEnvMut<NapiEnv>, e: u32, vh: u32, rp: u32) -> i32 {
    let value = match unsafe { host_js_value(snapi_env(&env, e), vh) } {
        Ok(value) => value,
        Err(status) => return status,
    };
    write_guest_number_value(&mut env, &value, rp, GuestNumberKind::I32)
}

fn guest_napi_get_value_uint32(mut env: FunctionEnvMut<NapiEnv>, e: u32, vh: u32, rp: u32) -> i32 {
    let value = match unsafe { host_js_value(snapi_env(&env, e), vh) } {
        Ok(value) => value,
        Err(status) => return status,
    };
    write_guest_number_value(&mut env, &value, rp, GuestNumberKind::U32)
}

fn guest_napi_get_value_double(mut env: FunctionEnvMut<NapiEnv>, e: u32, vh: u32, rp: u32) -> i32 {
    let value = match unsafe { host_js_value(snapi_env(&env, e), vh) } {
        Ok(value) => value,
        Err(status) => return status,
    };
    write_guest_number_value(&mut env, &value, rp, GuestNumberKind::F64)
}

fn guest_napi_get_value_int64(mut env: FunctionEnvMut<NapiEnv>, e: u32, vh: u32, rp: u32) -> i32 {
    let value = match unsafe { host_js_value(snapi_env(&env, e), vh) } {
        Ok(value) => value,
        Err(status) => return status,
    };
    write_guest_number_value(&mut env, &value, rp, GuestNumberKind::I64)
}

fn guest_napi_get_value_bool(mut env: FunctionEnvMut<NapiEnv>, e: u32, vh: u32, rp: u32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_get_value_bool(snapi_env(&env, e), vh, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

fn guest_napi_get_value_bigint_int64(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    vp: u32,
    lp: u32,
) -> i32 {
    let mut val: i64 = 0;
    let mut lossless: i32 = 0;
    let s = unsafe {
        snapi_bridge_get_value_bigint_int64(snapi_env(&env, e), vh, &mut val, &mut lossless)
    };
    if s == 0 {
        write_guest_i64(&mut env, vp, val);
        write_guest_u8(&mut env, lp, lossless as u8);
    }
    s
}

fn guest_napi_get_value_bigint_uint64(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    vp: u32,
    lp: u32,
) -> i32 {
    let mut val: u64 = 0;
    let mut lossless: i32 = 0;
    let s = unsafe {
        snapi_bridge_get_value_bigint_uint64(snapi_env(&env, e), vh, &mut val, &mut lossless)
    };
    if s == 0 {
        write_guest_u64(&mut env, vp, val);
        write_guest_u8(&mut env, lp, lossless as u8);
    }
    s
}

fn guest_napi_get_date_value(mut env: FunctionEnvMut<NapiEnv>, e: u32, vh: u32, rp: u32) -> i32 {
    let mut r: f64 = 0.0;
    let s = unsafe { snapi_bridge_get_date_value(snapi_env(&env, e), vh, &mut r) };
    if s == 0 {
        write_guest_f64(&mut env, rp, r);
    }
    s
}

fn guest_napi_get_value_external(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    rp: u32,
) -> i32 {
    let mut data: u64 = 0;
    let s = unsafe { snapi_bridge_get_value_external(snapi_env(&env, e), vh, &mut data) };
    if s == 0 {
        write_guest_u32(&mut env, rp, data as u32);
    }
    s
}

// --- Type checking ---

fn guest_napi_typeof(mut env: FunctionEnvMut<NapiEnv>, e: u32, vh: u32, rp: u32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_typeof(snapi_env(&env, e), vh, &mut r) };
    if s == 0 {
        write_guest_i32(&mut env, rp, r);
    }
    s
}

macro_rules! guest_is_check {
    ($name:ident, $bridge:ident) => {
        fn $name(mut env: FunctionEnvMut<NapiEnv>, e: u32, vh: u32, rp: u32) -> i32 {
            let mut r: i32 = 0;
            let s = unsafe { $bridge(snapi_env(&env, e), vh, &mut r) };
            if s == 0 {
                write_guest_u8(&mut env, rp, r as u8);
            }
            s
        }
    };
}

guest_is_check!(guest_napi_is_array, snapi_bridge_is_array);
guest_is_check!(guest_napi_is_error, snapi_bridge_is_error);
guest_is_check!(guest_napi_is_arraybuffer, snapi_bridge_is_arraybuffer);
guest_is_check!(guest_napi_is_typedarray, snapi_bridge_is_typedarray);
guest_is_check!(guest_napi_is_dataview, snapi_bridge_is_dataview);
guest_is_check!(guest_napi_is_date, snapi_bridge_is_date);
guest_is_check!(guest_napi_is_promise, snapi_bridge_is_promise);

fn guest_napi_instanceof(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    obj: u32,
    ctor: u32,
    rp: u32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_instanceof(snapi_env(&env, e), obj, ctor, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

// --- Coercion ---

macro_rules! guest_coerce {
    ($name:ident, $bridge:ident) => {
        fn $name(mut env: FunctionEnvMut<NapiEnv>, e: u32, vh: u32, rp: u32) -> i32 {
            let mut out: u32 = 0;
            let s = unsafe { $bridge(snapi_env(&env, e), vh, &mut out) };
            if s == 0 {
                write_guest_u32(&mut env, rp, out);
            }
            s
        }
    };
}

guest_coerce!(guest_napi_coerce_to_bool, snapi_bridge_coerce_to_bool);
guest_coerce!(guest_napi_coerce_to_number, snapi_bridge_coerce_to_number);
guest_coerce!(guest_napi_coerce_to_string, snapi_bridge_coerce_to_string);
guest_coerce!(guest_napi_coerce_to_object, snapi_bridge_coerce_to_object);

// --- Object operations ---

fn guest_napi_set_property(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    k: u32,
    v: u32,
) -> i32 {
    let snapi = snapi_env(&env, e);
    with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_set_property(snapi, o, k, v)
    })
}

fn guest_napi_get_property(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    k: u32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let snapi = snapi_env(&env, e);
    let s = with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_get_property(snapi, o, k, &mut out)
    });
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_has_property(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    k: u32,
    rp: u32,
) -> i32 {
    let mut r: i32 = 0;
    let snapi = snapi_env(&env, e);
    let s = with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_has_property(snapi, o, k, &mut r)
    });
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

fn guest_napi_has_own_property(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    k: u32,
    rp: u32,
) -> i32 {
    let mut r: i32 = 0;
    let snapi = snapi_env(&env, e);
    let s = with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_has_own_property(snapi, o, k, &mut r)
    });
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

fn guest_napi_delete_property(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    k: u32,
    rp: u32,
) -> i32 {
    let mut r: i32 = 0;
    let snapi = snapi_env(&env, e);
    let s = with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_delete_property(snapi, o, k, &mut r)
    });
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

fn guest_napi_set_named_property(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    np: u32,
    v: u32,
) -> i32 {
    let Some(key) = read_guest_string_value(&mut env, np, u32::MAX, GuestStringEncoding::Utf8)
    else {
        return 1;
    };
    let snapi = snapi_env(&env, e);
    with_cb_context(&mut env, e, || unsafe {
        host_js_set_named_property(snapi, o, &key, v)
    })
}

fn guest_napi_get_named_property(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    np: u32,
    rp: u32,
) -> i32 {
    let Some(key) = read_guest_string_value(&mut env, np, u32::MAX, GuestStringEncoding::Utf8)
    else {
        return 1;
    };
    let mut out: u32 = 0;
    let snapi = snapi_env(&env, e);
    let s = with_cb_context(&mut env, e, || unsafe {
        host_js_get_named_property(snapi, o, &key, &mut out)
    });
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_has_named_property(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    np: u32,
    rp: u32,
) -> i32 {
    let Some(key) = read_guest_string_value(&mut env, np, u32::MAX, GuestStringEncoding::Utf8)
    else {
        return 1;
    };
    let mut r: i32 = 0;
    let snapi = snapi_env(&env, e);
    let s = with_cb_context(&mut env, e, || unsafe {
        host_js_has_named_property(snapi, o, &key, &mut r)
    });
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

fn guest_napi_set_element(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    idx: u32,
    v: u32,
) -> i32 {
    let snapi = snapi_env(&env, e);
    with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_set_element(snapi, o, idx, v)
    })
}

fn guest_napi_get_element(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    idx: u32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let snapi = snapi_env(&env, e);
    let s = with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_get_element(snapi, o, idx, &mut out)
    });
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_has_element(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    idx: u32,
    rp: u32,
) -> i32 {
    let mut r: i32 = 0;
    let snapi = snapi_env(&env, e);
    let s = with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_has_element(snapi, o, idx, &mut r)
    });
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

fn guest_napi_delete_element(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    idx: u32,
    rp: u32,
) -> i32 {
    let mut r: i32 = 0;
    let snapi = snapi_env(&env, e);
    let s = with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_delete_element(snapi, o, idx, &mut r)
    });
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

fn guest_napi_get_array_length(mut env: FunctionEnvMut<NapiEnv>, e: u32, ah: u32, rp: u32) -> i32 {
    let mut r: u32 = 0;
    let s = unsafe { snapi_bridge_get_array_length(snapi_env(&env, e), ah, &mut r) };
    if s == 0 {
        write_guest_u32(&mut env, rp, r);
    }
    s
}

fn guest_napi_get_property_names(mut env: FunctionEnvMut<NapiEnv>, e: u32, o: u32, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let snapi = snapi_env(&env, e);
    // Property enumeration can synchronously invoke a Proxy's ownKeys trap.
    let s = with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_get_property_names(snapi, o, &mut out)
    });
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_get_all_property_names(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    o: u32,
    mode: i32,
    filter: i32,
    conversion: i32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let snapi = snapi_env(&env, e);
    // Property enumeration can synchronously invoke a Proxy's ownKeys trap.
    let s = with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_get_all_property_names(snapi, o, mode, filter, conversion, &mut out)
    });
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_get_prototype(mut env: FunctionEnvMut<NapiEnv>, e: u32, o: u32, rp: u32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_prototype(snapi_env(&env, e), o, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_object_freeze(env: FunctionEnvMut<NapiEnv>, e: u32, o: u32) -> i32 {
    unsafe { snapi_bridge_object_freeze(snapi_env(&env, e), o) }
}

fn guest_napi_object_seal(env: FunctionEnvMut<NapiEnv>, e: u32, o: u32) -> i32 {
    unsafe { snapi_bridge_object_seal(snapi_env(&env, e), o) }
}

// --- Comparison ---

fn guest_napi_strict_equals(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    lhs: u32,
    rhs: u32,
    rp: u32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_strict_equals(snapi_env(&env, e), lhs, rhs, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

// --- Error handling ---

fn guest_napi_throw(env: FunctionEnvMut<NapiEnv>, e: u32, err: u32) -> i32 {
    unsafe { snapi_bridge_throw(snapi_env(&env, e), err) }
}

fn guest_napi_throw_error(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    code_ptr: u32,
    msg_ptr: u32,
) -> i32 {
    let code_bytes = if code_ptr != 0 {
        read_guest_c_string(&mut env, code_ptr)
    } else {
        None
    };
    let Some(msg_bytes) = read_guest_c_string(&mut env, msg_ptr) else {
        return 1;
    };
    let c_code = code_bytes.map(|b| CString::new(b).unwrap_or_default());
    let c_msg = CString::new(msg_bytes).unwrap_or_default();
    unsafe {
        snapi_bridge_throw_error(
            snapi_env(&env, e),
            c_code.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            c_msg.as_ptr(),
        )
    }
}

fn guest_napi_throw_type_error(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    code_ptr: u32,
    msg_ptr: u32,
) -> i32 {
    let code_bytes = if code_ptr != 0 {
        read_guest_c_string(&mut env, code_ptr)
    } else {
        None
    };
    let Some(msg_bytes) = read_guest_c_string(&mut env, msg_ptr) else {
        return 1;
    };
    let c_code = code_bytes.map(|b| CString::new(b).unwrap_or_default());
    let c_msg = CString::new(msg_bytes).unwrap_or_default();
    unsafe {
        snapi_bridge_throw_type_error(
            snapi_env(&env, e),
            c_code.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            c_msg.as_ptr(),
        )
    }
}

fn guest_napi_throw_range_error(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    code_ptr: u32,
    msg_ptr: u32,
) -> i32 {
    let code_bytes = if code_ptr != 0 {
        read_guest_c_string(&mut env, code_ptr)
    } else {
        None
    };
    let Some(msg_bytes) = read_guest_c_string(&mut env, msg_ptr) else {
        return 1;
    };
    let c_code = code_bytes.map(|b| CString::new(b).unwrap_or_default());
    let c_msg = CString::new(msg_bytes).unwrap_or_default();
    unsafe {
        snapi_bridge_throw_range_error(
            snapi_env(&env, e),
            c_code.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            c_msg.as_ptr(),
        )
    }
}

fn guest_napi_is_exception_pending(mut env: FunctionEnvMut<NapiEnv>, e: u32, rp: u32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_is_exception_pending(snapi_env(&env, e), &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

fn guest_napi_get_and_clear_last_exception(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_and_clear_last_exception(snapi_env(&env, e), &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

// --- Promise ---

fn guest_napi_create_promise(mut env: FunctionEnvMut<NapiEnv>, e: u32, dp: u32, rp: u32) -> i32 {
    let mut deferred_id: u32 = 0;
    let mut promise_id: u32 = 0;
    let s = unsafe {
        snapi_bridge_create_promise(snapi_env(&env, e), &mut deferred_id, &mut promise_id)
    };
    if s == 0 {
        write_guest_u32(&mut env, dp, deferred_id);
        write_guest_u32(&mut env, rp, promise_id);
    }
    s
}

fn guest_napi_resolve_deferred(mut env: FunctionEnvMut<NapiEnv>, e: u32, d: u32, v: u32) -> i32 {
    let snapi = snapi_env(&env, e);
    publish_pending_host_buffer_copies(&mut env, snapi);
    unsafe { snapi_bridge_resolve_deferred(snapi, d, v) }
}

fn guest_napi_reject_deferred(mut env: FunctionEnvMut<NapiEnv>, e: u32, d: u32, v: u32) -> i32 {
    let snapi = snapi_env(&env, e);
    publish_pending_host_buffer_copies(&mut env, snapi);
    unsafe { snapi_bridge_reject_deferred(snapi, d, v) }
}

// --- ArrayBuffer ---

fn guest_napi_create_arraybuffer(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    byte_length: u32,
    data_ptr: u32,
    rp: u32,
) -> i32 {
    #[cfg(feature = "js")]
    {
        // A null data result means native code never observes a pointer into
        // this ArrayBuffer. Allocate it in the host realm so its lifetime is
        // governed by JavaScript GC instead of pinning an unreachable guest
        // malloc allocation.
        if data_ptr == 0 {
            let mut out = 0u32;
            let status = unsafe {
                snapi_bridge_create_arraybuffer(snapi_env(&env, e), byte_length, &mut out)
            };
            if status == 0 {
                write_guest_u32(&mut env, rp, out);
            }
            return status;
        }
        let Some(guest_ptr) = allocate_zeroed_guest_bytes(&mut env, byte_length as usize) else {
            return 1;
        };
        let mut out = 0u32;
        let status =
            unsafe { snapi_bridge_create_arraybuffer(snapi_env(&env, e), byte_length, &mut out) };
        if status != 0 {
            recycle_guest_allocation(&mut env, guest_ptr);
            return status;
        }
        let backing_store_token = host_backing_store_token(&env, e, out);
        if !remember_host_buffer_copy(
            &mut env,
            e,
            out,
            backing_store_token,
            0,
            guest_ptr,
            byte_length as usize,
            true,
            true,
            true,
            true,
        ) {
            recycle_guest_allocation(&mut env, guest_ptr);
            return 1;
        }
        write_guest_u32(&mut env, rp, out);
        if data_ptr != 0 {
            write_guest_u32(&mut env, data_ptr, guest_ptr);
        }
        return 0;
    }

    #[cfg(not(feature = "js"))]
    {
        // Try to create a guest-memory-backed ArrayBuffer (for WASIX)
        let malloc_fn = env.data().malloc_fn.clone();
        let memory = env.data().memory.clone();

        if let (Some(malloc_fn), Some(memory)) = (malloc_fn, memory) {
            // Allocate memory in the guest's linear memory
            let guest_ptr: u32 = {
                let (_, mut store_ref) = env.data_and_store_mut();
                match malloc_fn.call(&mut store_ref, byte_length as i32) {
                    Ok(ptr) if ptr != 0 => ptr as u32,
                    _ => return 1, // allocation failed
                }
            };

            // Get host pointer corresponding to the guest allocation
            let host_addr: u64 = {
                let (_, store_ref) = env.data_and_store_mut();
                let view = memory.view(&store_ref);
                let host_base = view.data_ptr() as u64;
                host_base + guest_ptr as u64
            };

            // Zero-initialize the guest memory region
            {
                let zeros = vec![0u8; byte_length as usize];
                write_guest_bytes(&mut env, guest_ptr, &zeros);
            }

            // Create external arraybuffer backed by guest memory
            let mut out: u32 = 0;
            let mut backing_store_token: u64 = 0;
            let s = unsafe {
                snapi_bridge_create_external_arraybuffer(
                    snapi_env(&env, e),
                    host_addr,
                    byte_length,
                    &mut backing_store_token,
                    &mut out,
                )
            };
            if s == 0 {
                remember_guest_backing_store(
                    &mut env,
                    e,
                    out,
                    backing_store_token,
                    host_addr,
                    guest_ptr,
                    byte_length as usize,
                    true,
                );
                write_guest_u32(&mut env, rp, out);
                if data_ptr != 0 {
                    write_guest_u32(&mut env, data_ptr, guest_ptr);
                }
            }
            s
        } else {
            // Fallback: host-memory-backed arraybuffer (non-WASIX path)
            let mut out: u32 = 0;
            let s = unsafe {
                snapi_bridge_create_arraybuffer(snapi_env(&env, e), byte_length, &mut out)
            };
            if s == 0 {
                write_guest_u32(&mut env, rp, out);
            }
            s
        }
    }
}

fn guest_napi_create_external_arraybuffer(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    external_data: u32,
    byte_length: u32,
    _finalize_cb: u32,
    _finalize_hint: u32,
    rp: u32,
) -> i32 {
    #[cfg(feature = "js")]
    {
        let Some(source) = read_guest_bytes(&mut env, external_data, byte_length as usize) else {
            return 1;
        };
        let mut out = 0u32;
        let status =
            unsafe { snapi_bridge_create_arraybuffer(snapi_env(&env, e), byte_length, &mut out) };
        if status != 0 {
            return status;
        }
        if byte_length != 0 {
            let status = unsafe {
                snapi_bridge_overwrite_value_bytes(
                    snapi_env(&env, e),
                    out,
                    source.as_ptr().cast(),
                    byte_length,
                )
            };
            if status != 0 {
                return status;
            }
        }
        let backing_store_token = host_backing_store_token(&env, e, out);
        if !remember_host_buffer_copy(
            &mut env,
            e,
            out,
            backing_store_token,
            0,
            external_data,
            byte_length as usize,
            false,
            true,
            true,
            true,
        ) {
            return 1;
        }
        write_guest_u32(&mut env, rp, out);
        return 0;
    }

    #[cfg(not(feature = "js"))]
    {
        let memory = env.data().memory.clone();
        let Some(memory) = memory else {
            return 1;
        };

        let host_addr: u64 = {
            let (_, store_ref) = env.data_and_store_mut();
            let view = memory.view(&store_ref);
            let host_base = view.data_ptr() as u64;
            host_base + external_data as u64
        };

        let mut out: u32 = 0;
        let mut backing_store_token: u64 = 0;
        let s = unsafe {
            snapi_bridge_create_external_arraybuffer(
                snapi_env(&env, e),
                host_addr,
                byte_length,
                &mut backing_store_token,
                &mut out,
            )
        };
        if s == 0 {
            remember_guest_backing_store(
                &mut env,
                e,
                out,
                backing_store_token,
                host_addr,
                external_data,
                byte_length as usize,
                true,
            );
            write_guest_u32(&mut env, rp, out);
        }
        s
    }
}

fn guest_napi_create_external_buffer(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    byte_length: u32,
    external_data: u32,
    _finalize_cb: u32,
    _finalize_hint: u32,
    rp: u32,
) -> i32 {
    #[cfg(feature = "js")]
    {
        let Some(source) = read_guest_bytes(&mut env, external_data, byte_length as usize) else {
            return 1;
        };
        let mut host_data = 0u64;
        let mut out = 0u32;
        let status = unsafe {
            snapi_bridge_create_buffer_copy(
                snapi_env(&env, e),
                byte_length,
                source.as_ptr(),
                &mut host_data,
                &mut out,
            )
        };
        if host_data != 0 {
            unsafe { snapi_bridge_free_buffer(host_data as *mut c_void) };
        }
        if status != 0 {
            return status;
        }
        let backing_store_token = host_backing_store_token(&env, e, out);
        if !remember_host_buffer_copy(
            &mut env,
            e,
            out,
            backing_store_token,
            0,
            external_data,
            byte_length as usize,
            false,
            true,
            true,
            false,
        ) {
            return 1;
        }
        write_guest_u32(&mut env, rp, out);
        return 0;
    }

    #[cfg(not(feature = "js"))]
    {
        let memory = env.data().memory.clone();
        let Some(memory) = memory else {
            return 1;
        };

        let host_addr: u64 = {
            let (_, store_ref) = env.data_and_store_mut();
            let view = memory.view(&store_ref);
            let host_base = view.data_ptr() as u64;
            host_base + external_data as u64
        };

        let mut out: u32 = 0;
        let mut backing_store_token: u64 = 0;
        let s = unsafe {
            snapi_bridge_create_external_buffer(
                snapi_env(&env, e),
                host_addr,
                byte_length,
                &mut backing_store_token,
                &mut out,
            )
        };
        if s == 0 {
            remember_guest_backing_store(
                &mut env,
                e,
                out,
                backing_store_token,
                host_addr,
                external_data,
                byte_length as usize,
                false,
            );
            write_guest_u32(&mut env, rp, out);
        }
        s
    }
}

fn guest_napi_get_arraybuffer_info(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    data_ptr: u32,
    len_ptr: u32,
) -> i32 {
    let mut host_data_addr: u64 = 0;
    let mut bl: u32 = 0;
    let mut backing_store_token: u64 = 0;
    let s = unsafe {
        snapi_bridge_get_arraybuffer_info(
            snapi_env(&env, e),
            vh,
            &mut host_data_addr,
            &mut bl,
            &mut backing_store_token,
        )
    };
    if s != 0 {
        return s;
    }

    if len_ptr != 0 {
        write_guest_u32(&mut env, len_ptr, bl);
    }

    let guest_data_ptr = if data_ptr != 0 {
        resolve_current_host_data_to_guest(
            &mut env,
            e,
            vh,
            backing_store_token,
            host_data_addr,
            bl as usize,
            true,
        )
    } else {
        None
    };
    if host_data_addr != 0 {
        unsafe { snapi_bridge_free_buffer(host_data_addr as *mut c_void) };
    }
    if let Some(guest_data_ptr) = guest_data_ptr {
        write_guest_u32(&mut env, data_ptr, guest_data_ptr);
    }
    0
}

fn guest_napi_detach_arraybuffer(env: FunctionEnvMut<NapiEnv>, e: u32, vh: u32) -> i32 {
    unsafe { snapi_bridge_detach_arraybuffer(snapi_env(&env, e), vh) }
}

fn guest_napi_is_detached_arraybuffer(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    rp: u32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_is_detached_arraybuffer(snapi_env(&env, e), vh, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

fn guest_node_api_is_sharedarraybuffer(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    value: u32,
    rp: u32,
) -> i32 {
    let mut r = 0i32;
    let s = unsafe { snapi_bridge_is_sharedarraybuffer(snapi_env(&env, e), value, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

fn guest_node_api_create_sharedarraybuffer(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    byte_length: u32,
    data_ptr: u32,
    rp: u32,
) -> i32 {
    let Some(guest_ptr) = allocate_zeroed_guest_bytes(&mut env, byte_length as usize) else {
        return 1;
    };
    let mut host_data_addr = 0u64;
    let mut out = 0u32;
    let s = unsafe {
        snapi_bridge_create_sharedarraybuffer(
            snapi_env(&env, e),
            byte_length,
            &mut host_data_addr,
            &mut out,
        )
    };
    if s != 0 {
        recycle_guest_allocation(&mut env, guest_ptr);
        return s;
    }

    let backing_store_token = host_backing_store_token(&env, e, out);
    if !remember_host_buffer_copy(
        &mut env,
        e,
        out,
        backing_store_token,
        0,
        guest_ptr,
        byte_length as usize,
        true,
        true,
        true,
        true,
    ) {
        recycle_guest_allocation(&mut env, guest_ptr);
        return 1;
    }

    if data_ptr != 0 {
        write_guest_u32(&mut env, data_ptr, guest_ptr);
    }
    write_guest_u32(&mut env, rp, out);
    s
}

fn guest_node_api_set_prototype(
    env: FunctionEnvMut<NapiEnv>,
    napi_env: u32,
    object: u32,
    prototype: u32,
) -> i32 {
    let object_id = object;
    let prototype_id = prototype;
    unsafe {
        snapi_bridge_node_api_set_prototype(snapi_env(&env, napi_env), object_id, prototype_id)
    }
}

// --- TypedArray ---

fn guest_napi_create_typedarray(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    typ: i32,
    length: u32,
    ab: u32,
    offset: u32,
    rp: u32,
) -> i32 {
    if typedarray_element_size(typ).is_none() {
        return 1;
    }
    let mut out: u32 = 0;
    let s = unsafe {
        snapi_bridge_create_typedarray(snapi_env(&env, e), typ, length, ab, offset, &mut out)
    };
    if s == 0 {
        // A TypedArray view shares its ArrayBuffer's guest-side copy. Record
        // that alias immediately so a reference to the view keeps the backing
        // copy coherent for later native writes. Without this, long-lived
        // native pointers (for example Node's streamBaseState) are dropped at
        // the first JS callback boundary and JavaScript observes stale bytes.
        let byte_len = (length as usize)
            .saturating_mul(typedarray_element_size(typ).expect("validated typed-array kind"));
        let backing_store_token = host_backing_store_token(&env, e, out);
        let guest_ptr = if backing_store_token != 0 {
            resolve_guest_backing_store_token(env.data(), e, out, backing_store_token, 0, byte_len)
        } else {
            env.data()
                .guest_data_ptrs
                .get(&(e, ab))
                .copied()
                .and_then(|base| base.checked_add(offset))
        };
        if let Some(guest_ptr) = guest_ptr {
            env.data_mut().guest_data_ptrs.insert((e, out), guest_ptr);
        }
        write_guest_u32(&mut env, rp, out);
    }
    s
}

#[allow(clippy::too_many_arguments)]
fn guest_napi_get_typedarray_info(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    tp: u32,
    lp: u32,
    dp: u32,
    abp: u32,
    bop: u32,
) -> i32 {
    let mut typ: i32 = 0;
    let mut len: u32 = 0;
    let mut host_data_addr: u64 = 0;
    let mut ab: u32 = 0;
    let mut bo: u32 = 0;
    let mut backing_store_token: u64 = 0;
    let s = unsafe {
        snapi_bridge_get_typedarray_info(
            snapi_env(&env, e),
            vh,
            &mut typ,
            &mut len,
            if dp != 0 {
                &mut host_data_addr
            } else {
                std::ptr::null_mut()
            },
            if abp != 0 {
                &mut ab
            } else {
                std::ptr::null_mut()
            },
            if bop != 0 {
                &mut bo
            } else {
                std::ptr::null_mut()
            },
            if dp != 0 {
                &mut backing_store_token
            } else {
                std::ptr::null_mut()
            },
        )
    };
    if s == 0 {
        if tp != 0 {
            write_guest_i32(&mut env, tp, typ);
        }
        if lp != 0 {
            write_guest_u32(&mut env, lp, len);
        }
        if dp != 0 {
            let Some(elem_size) = typedarray_element_size(typ) else {
                return 1;
            };
            let byte_len = len as usize * elem_size;
            if let Some(guest_data_ptr) = resolve_current_host_data_to_guest(
                &mut env,
                e,
                vh,
                backing_store_token,
                host_data_addr,
                byte_len,
                false,
            ) {
                write_guest_u32(&mut env, dp, guest_data_ptr);
            }
        }
        if host_data_addr != 0 {
            unsafe { snapi_bridge_free_buffer(host_data_addr as *mut c_void) };
        }
        if abp != 0 {
            write_guest_u32(&mut env, abp, ab);
        }
        if bop != 0 {
            write_guest_u32(&mut env, bop, bo);
        }
    }
    s
}

// --- DataView ---

fn guest_napi_create_dataview(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    bl: u32,
    ab: u32,
    bo: u32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_dataview(snapi_env(&env, e), bl, ab, bo, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_get_dataview_info(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    blp: u32,
    dp: u32,
    abp: u32,
    bop: u32,
) -> i32 {
    let mut bl: u32 = 0;
    let mut host_data_addr: u64 = 0;
    let mut ab: u32 = 0;
    let mut bo: u32 = 0;
    let mut backing_store_token: u64 = 0;
    let s = unsafe {
        snapi_bridge_get_dataview_info(
            snapi_env(&env, e),
            vh,
            &mut bl,
            &mut host_data_addr,
            &mut ab,
            &mut bo,
            &mut backing_store_token,
        )
    };
    if s == 0 {
        if blp != 0 {
            write_guest_u32(&mut env, blp, bl);
        }
        let guest_data_ptr = if dp != 0 {
            resolve_current_host_data_to_guest(
                &mut env,
                e,
                vh,
                backing_store_token,
                host_data_addr,
                bl as usize,
                false,
            )
        } else {
            None
        };
        if host_data_addr != 0 {
            unsafe { snapi_bridge_free_buffer(host_data_addr as *mut c_void) };
        }
        if let Some(guest_data_ptr) = guest_data_ptr {
            write_guest_u32(&mut env, dp, guest_data_ptr);
        }
        if abp != 0 {
            write_guest_u32(&mut env, abp, ab);
        }
        if bop != 0 {
            write_guest_u32(&mut env, bop, bo);
        }
    }
    s
}

// --- References ---

fn guest_napi_create_reference(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    irc: u32,
    rp: u32,
) -> i32 {
    let mut ref_id: u32 = 0;
    let s = unsafe { snapi_bridge_create_reference(snapi_env(&env, e), vh, irc, &mut ref_id) };
    if s == 0 {
        let guest_ptr = env.data().guest_data_ptrs.get(&(e, vh)).copied();
        let held_guest_ptr = guest_ptr.and_then(|guest_ptr| {
            env.data_mut()
                .host_buffer_copies
                .iter_mut()
                .find(|mapping| {
                    let mapping_end = mapping.guest_ptr.saturating_add(mapping.byte_len as u32);
                    mapping.guest_env == e
                        && guest_ptr >= mapping.guest_ptr
                        && guest_ptr < mapping_end
                })
                .map(|mapping| {
                    mapping.reference_holds = mapping.reference_holds.saturating_add(1);
                    mapping.guest_ptr
                })
        });
        if let Some(backing_guest_ptr) = held_guest_ptr {
            env.data_mut()
                .host_buffer_reference_holds
                .insert((e, ref_id), backing_guest_ptr);
        }
        write_guest_u32(&mut env, rp, ref_id);
    }
    s
}

fn guest_napi_delete_reference(mut env: FunctionEnvMut<NapiEnv>, e: u32, r: u32) -> i32 {
    if let Some(guest_ptr) = env.data_mut().host_buffer_reference_holds.remove(&(e, r))
        && let Some(mapping) = env
            .data_mut()
            .host_buffer_copies
            .iter_mut()
            .find(|mapping| mapping.guest_env == e && mapping.guest_ptr == guest_ptr)
    {
        mapping.reference_holds = mapping.reference_holds.saturating_sub(1);
    }
    unsafe { snapi_bridge_delete_reference(snapi_env(&env, e), r) }
}

fn guest_napi_reference_ref(mut env: FunctionEnvMut<NapiEnv>, e: u32, r: u32, rp: u32) -> i32 {
    let mut count: u32 = 0;
    let s = unsafe { snapi_bridge_reference_ref(snapi_env(&env, e), r, &mut count) };
    if s == 0 && rp != 0 {
        write_guest_u32(&mut env, rp, count);
    }
    s
}

fn guest_napi_reference_unref(mut env: FunctionEnvMut<NapiEnv>, e: u32, r: u32, rp: u32) -> i32 {
    let mut count: u32 = 0;
    let s = unsafe { snapi_bridge_reference_unref(snapi_env(&env, e), r, &mut count) };
    if s == 0 && rp != 0 {
        write_guest_u32(&mut env, rp, count);
    }
    s
}

fn guest_napi_get_reference_value(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    r: u32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_reference_value(snapi_env(&env, e), r, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

// --- Handle scopes ---

fn guest_napi_open_handle_scope(mut env: FunctionEnvMut<NapiEnv>, e: u32, rp: u32) -> i32 {
    if snapi_env(&env, e).is_null() || rp == 0 {
        return 1;
    }
    let mut scope_id = 0;
    let status =
        unsafe { crate::snapi::snapi_bridge_open_handle_scope(snapi_env(&env, e), &mut scope_id) };
    if status == 0 && !write_guest_u32(&mut env, rp, scope_id) {
        return 1;
    }
    if status == 0 {
        let owner_id = env.data_mut().next_host_buffer_owner();
        env.data_mut()
            .host_buffer_handle_scopes
            .push((e, scope_id, owner_id));
    }
    status
}
fn guest_napi_close_handle_scope(mut env: FunctionEnvMut<NapiEnv>, e: u32, scope: u32) -> i32 {
    let snapi = snapi_env(&env, e);
    if let Some(index) = env
        .data()
        .host_buffer_handle_scopes
        .iter()
        .rposition(|(guest_env, scope_id, _)| *guest_env == e && *scope_id == scope)
    {
        let (_, _, owner_id) = env.data_mut().host_buffer_handle_scopes.remove(index);
        flush_host_buffer_copies_for_owner(&mut env, owner_id);
    }
    unsafe { crate::snapi::snapi_bridge_close_handle_scope(snapi, scope) }
}

fn guest_napi_open_escapable_handle_scope(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    rp: u32,
) -> i32 {
    let mut scope_id: u32 = 0;
    let s = unsafe { snapi_bridge_open_escapable_handle_scope(snapi_env(&env, e), &mut scope_id) };
    if s == 0 {
        write_guest_u32(&mut env, rp, scope_id);
        let owner_id = env.data_mut().next_host_buffer_owner();
        env.data_mut()
            .host_buffer_handle_scopes
            .push((e, scope_id, owner_id));
    }
    s
}

fn guest_napi_close_escapable_handle_scope(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    scope: u32,
) -> i32 {
    let snapi = snapi_env(&env, e);
    if let Some(index) = env
        .data()
        .host_buffer_handle_scopes
        .iter()
        .rposition(|(guest_env, scope_id, _)| *guest_env == e && *scope_id == scope)
    {
        let (_, _, owner_id) = env.data_mut().host_buffer_handle_scopes.remove(index);
        flush_host_buffer_copies_for_owner(&mut env, owner_id);
    }
    unsafe { snapi_bridge_close_escapable_handle_scope(snapi, scope) }
}

fn guest_napi_escape_handle(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    scope: u32,
    escapee: u32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_escape_handle(snapi_env(&env, e), scope, escapee, &mut out) };
    if s == 0 {
        if let Some(&guest_ptr) = env.data().guest_data_ptrs.get(&(e, escapee)) {
            env.data_mut().guest_data_ptrs.insert((e, out), guest_ptr);
        }
        write_guest_u32(&mut env, rp, out);
    }
    s
}

// --- Type tagging ---

fn guest_napi_type_tag_object(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    obj: u32,
    tag_ptr: u32,
) -> i32 {
    // napi_type_tag is { uint64_t lower; uint64_t upper; } = 16 bytes
    let Some(tag_bytes) = read_guest_bytes(&mut env, tag_ptr, 16) else {
        return 1;
    };
    let lower = u64::from_le_bytes(tag_bytes[0..8].try_into().unwrap());
    let upper = u64::from_le_bytes(tag_bytes[8..16].try_into().unwrap());
    unsafe { snapi_bridge_type_tag_object(snapi_env(&env, e), obj, lower, upper) }
}

fn guest_napi_check_object_type_tag(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    obj: u32,
    tag_ptr: u32,
    rp: u32,
) -> i32 {
    let Some(tag_bytes) = read_guest_bytes(&mut env, tag_ptr, 16) else {
        return 1;
    };
    let lower = u64::from_le_bytes(tag_bytes[0..8].try_into().unwrap());
    let upper = u64::from_le_bytes(tag_bytes[8..16].try_into().unwrap());
    let mut r: i32 = 0;
    let s = unsafe {
        snapi_bridge_check_object_type_tag(snapi_env(&env, e), obj, lower, upper, &mut r)
    };
    if s == 0 {
        write_guest_u8(&mut env, rp, r as u8);
    }
    s
}

// --- Function calling ---

fn guest_napi_call_function(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    recv: u32,
    func: u32,
    argc: u32,
    argv_ptr: u32,
    rp: u32,
) -> i32 {
    let argc_u = argc;
    let argv_ids = if argc_u > 0 {
        let Some(ids) = read_guest_u32_array(&mut env, argv_ptr, argc_u as usize) else {
            return 1;
        };
        ids
    } else {
        vec![]
    };

    let snapi = snapi_env(&env, e);
    // Native code may have written through a guest-memory pointer obtained for
    // a host-owned Buffer/ArrayBuffer. Synchronize those bytes at the handoff
    // back to JavaScript, immediately before JavaScript can observe the value.
    publish_pending_host_buffer_copies(&mut env, snapi);
    let mut out: u32 = 0;
    let s = with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_call_function(snapi, recv, func, argc_u, argv_ids.as_ptr(), &mut out)
    });

    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

// --- napi_create_function ---

fn guest_napi_create_function(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    name_ptr: u32,
    name_len: u32,
    cb: u32,
    data: u32,
    rp: u32,
) -> i32 {
    // Read function name
    let wl = name_len;
    let name_bytes: Vec<u8> = if wl == 0xFFFFFFFFu32 {
        // NAPI_AUTO_LENGTH: read null-terminated string
        read_guest_c_string(&mut env, name_ptr).unwrap_or_default()
    } else if wl > 0 && name_ptr != 0 {
        let Some(bytes) = read_guest_bytes(&mut env, name_ptr, wl as usize) else {
            return 1;
        };
        bytes
    } else {
        vec![]
    };

    // Allocate a registration ID in the C++ callback registry
    let snapi = snapi_env(&env, e);
    let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id(snapi) };

    // Register the WASM callback and data pointer in the C++ registry
    unsafe { snapi_bridge_register_callback(snapi, reg_id, e, cb, data as u64) };

    // Create a JS function in V8 with generic_wasm_callback as its native callback.
    // The reg_id is stored as the function's data pointer so generic_wasm_callback
    // can look up which WASM function to invoke.
    let actual_name_len = name_bytes.len() as u32;
    let c_name = CString::new(name_bytes).unwrap_or_default();
    let mut out: u32 = 0;
    let s = unsafe {
        snapi_bridge_create_function(snapi, c_name.as_ptr(), actual_name_len, reg_id, &mut out)
    };
    if s != 0 {
        return s;
    }

    write_guest_u32(&mut env, rp, out);
    0
}

// --- napi_get_cb_info ---

fn guest_napi_get_cb_info(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    _cbinfo: u32,
    argc_ptr: u32,
    argv_ptr: u32,
    this_ptr: u32,
    data_ptr: u32,
) -> i32 {
    // Read the caller's requested argc (size of their argv array)
    let wanted: u32 = if argc_ptr != 0 {
        let Some(bytes) = read_guest_bytes(&mut env, argc_ptr, 4) else {
            return 1;
        };
        u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
    } else {
        0
    };
    if wanted as usize > MAX_NAPI_CALLBACK_ARGS {
        return 1;
    }

    // Query the bridge for callback context
    let mut actual_argc: u32 = wanted;
    let mut argv_ids = vec![0u32; wanted as usize];
    let mut this_id: u32 = 0;
    let mut data_val: u64 = 0;

    let s = unsafe {
        snapi_bridge_get_cb_info(
            snapi_env(&env, e),
            _cbinfo,
            &mut actual_argc,
            if wanted > 0 {
                argv_ids.as_mut_ptr()
            } else {
                std::ptr::null_mut()
            },
            wanted,
            &mut this_id,
            &mut data_val,
        )
    };
    if s != 0 {
        return s;
    }

    // Write actual argc back
    if argc_ptr != 0 {
        write_guest_u32(&mut env, argc_ptr, actual_argc);
    }

    // Write argv (array of handle IDs) - only write up to min(wanted, actual)
    if argv_ptr != 0 {
        let to_write = std::cmp::min(wanted, actual_argc);
        // The argv entries written must lie below 4 GiB.
        if u64::from(argv_ptr) + u64::from(to_write) * 4 > 1u64 << 32 {
            return 1;
        }
        for i in 0..to_write {
            write_guest_u32(&mut env, argv_ptr + i * 4, argv_ids[i as usize]);
        }
    }

    // Write this_arg
    if this_ptr != 0 {
        write_guest_u32(&mut env, this_ptr, this_id);
    }

    // Write data pointer (as a 32-bit guest pointer)
    if data_ptr != 0 {
        write_guest_u32(&mut env, data_ptr, data_val as u32);
    }

    0
}

// --- napi_get_new_target ---

fn guest_napi_get_new_target(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    _cbinfo: u32,
    rp: u32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_new_target(snapi_env(&env, e), _cbinfo, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

// --- napi_define_class ---
// Guest layout of napi_property_descriptor (32-bit WASM, 32 bytes):
//   offset  0: const char* utf8name     (4 bytes, guest pointer)
//   offset  4: napi_value name          (4 bytes, handle ID)
//   offset  8: napi_callback method     (4 bytes, fn pointer)
//   offset 12: napi_callback getter     (4 bytes, fn pointer)
//   offset 16: napi_callback setter     (4 bytes, fn pointer)
//   offset 20: napi_value value         (4 bytes, handle ID)
//   offset 24: napi_property_attributes (4 bytes, enum)
//   offset 28: void* data               (4 bytes, guest pointer)
const PROP_DESC_SIZE: usize = 32;

#[allow(clippy::too_many_arguments)]
fn guest_napi_define_class(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    name_ptr: u32,
    name_len: u32,
    constructor: u32,
    ctor_data: u32,
    prop_count: u32,
    props_ptr: u32,
    rp: u32,
) -> i32 {
    // Read class name
    let wl = name_len;
    let name_bytes: Vec<u8> = if wl == 0xFFFFFFFFu32 {
        read_guest_c_string(&mut env, name_ptr).unwrap_or_default()
    } else if wl > 0 && name_ptr != 0 {
        let Some(bytes) = read_guest_bytes(&mut env, name_ptr, wl as usize) else {
            return 1;
        };
        bytes
    } else {
        vec![]
    };

    // Register the constructor callback
    let snapi = snapi_env(&env, e);
    let ctor_reg_id = unsafe { snapi_bridge_alloc_cb_reg_id(snapi) };
    unsafe { snapi_bridge_register_callback(snapi, ctor_reg_id, e, constructor, ctor_data as u64) };

    let pc = prop_count;
    let actual_name_len = name_bytes.len() as u32;
    let c_name = CString::new(name_bytes).unwrap_or_default();

    if pc == 0 {
        // No properties — simple case
        let mut out: u32 = 0;
        let s = unsafe {
            snapi_bridge_define_class(
                snapi_env(&env, e),
                c_name.as_ptr(),
                actual_name_len,
                ctor_reg_id,
                0,
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                &mut out,
            )
        };
        if s != 0 {
            return s;
        }
        write_guest_u32(&mut env, rp, out);
        return 0;
    }

    // Read property descriptors from guest memory
    // usize is 32 bits on this host, so a guest-supplied count can overflow.
    let Some(total_bytes) = (pc as usize).checked_mul(PROP_DESC_SIZE) else {
        return 1;
    };
    let Some(raw) = read_guest_bytes(&mut env, props_ptr, total_bytes) else {
        return 1;
    };

    let mut prop_names_c: Vec<CString> = Vec::with_capacity(pc as usize);
    let mut prop_names_ptrs: Vec<*const i8> = Vec::with_capacity(pc as usize);
    let mut prop_name_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_types: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_value_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_method_reg_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_getter_reg_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_setter_reg_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_attributes: Vec<i32> = Vec::with_capacity(pc as usize);

    for i in 0..pc as usize {
        let base = i * PROP_DESC_SIZE;
        let utf8name_guest =
            u32::from_le_bytes([raw[base], raw[base + 1], raw[base + 2], raw[base + 3]]);
        let name_id =
            u32::from_le_bytes([raw[base + 4], raw[base + 5], raw[base + 6], raw[base + 7]]);
        let method_ptr =
            u32::from_le_bytes([raw[base + 8], raw[base + 9], raw[base + 10], raw[base + 11]]);
        let getter_ptr = u32::from_le_bytes([
            raw[base + 12],
            raw[base + 13],
            raw[base + 14],
            raw[base + 15],
        ]);
        let setter_ptr = u32::from_le_bytes([
            raw[base + 16],
            raw[base + 17],
            raw[base + 18],
            raw[base + 19],
        ]);
        let value_id = u32::from_le_bytes([
            raw[base + 20],
            raw[base + 21],
            raw[base + 22],
            raw[base + 23],
        ]);
        let attrs = i32::from_le_bytes([
            raw[base + 24],
            raw[base + 25],
            raw[base + 26],
            raw[base + 27],
        ]);
        let data_ptr = u32::from_le_bytes([
            raw[base + 28],
            raw[base + 29],
            raw[base + 30],
            raw[base + 31],
        ]);

        // Read property name
        let pname = if utf8name_guest != 0 {
            read_guest_c_string(&mut env, utf8name_guest).unwrap_or_default()
        } else {
            vec![]
        };
        let c_pname = CString::new(pname).unwrap_or_default();
        prop_name_ids.push(name_id);

        // Determine property type and register callbacks as needed
        if method_ptr != 0 {
            // Method property
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id(snapi) };
            unsafe {
                snapi_bridge_register_callback(snapi, reg_id, e, method_ptr, data_ptr as u64)
            };
            prop_types.push(1);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(reg_id);
            prop_getter_reg_ids.push(0);
            prop_setter_reg_ids.push(0);
        } else if getter_ptr != 0 && setter_ptr != 0 {
            // Getter + Setter
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id(snapi) };
            unsafe {
                snapi_bridge_register_callback_pair(
                    snapi,
                    reg_id,
                    e,
                    getter_ptr,
                    setter_ptr,
                    data_ptr as u64,
                )
            };
            prop_types.push(4);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(reg_id);
            prop_setter_reg_ids.push(0);
        } else if getter_ptr != 0 {
            // Getter only
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id(snapi) };
            unsafe {
                snapi_bridge_register_callback(snapi, reg_id, e, getter_ptr, data_ptr as u64)
            };
            prop_types.push(2);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(reg_id);
            prop_setter_reg_ids.push(0);
        } else if setter_ptr != 0 {
            // Setter only
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id(snapi) };
            unsafe {
                snapi_bridge_register_callback(snapi, reg_id, e, setter_ptr, data_ptr as u64)
            };
            prop_types.push(3);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(0);
            prop_setter_reg_ids.push(reg_id);
        } else {
            // Value property
            prop_types.push(0);
            prop_value_ids.push(value_id);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(0);
            prop_setter_reg_ids.push(0);
        }

        prop_attributes.push(attrs);
        prop_names_c.push(c_pname);
    }

    // Build pointer array (must live as long as the FFI call)
    for cn in &prop_names_c {
        prop_names_ptrs.push(cn.as_ptr());
    }

    let mut out: u32 = 0;
    let s = unsafe {
        snapi_bridge_define_class(
            snapi_env(&env, e),
            c_name.as_ptr(),
            actual_name_len,
            ctor_reg_id,
            pc,
            prop_names_ptrs.as_ptr(),
            prop_name_ids.as_ptr(),
            prop_types.as_ptr(),
            prop_value_ids.as_ptr(),
            prop_method_reg_ids.as_ptr(),
            prop_getter_reg_ids.as_ptr(),
            prop_setter_reg_ids.as_ptr(),
            prop_attributes.as_ptr(),
            &mut out,
        )
    };
    if s != 0 {
        return s;
    }
    write_guest_u32(&mut env, rp, out);
    0
}

fn guest_napi_define_properties(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    obj: u32,
    prop_count: u32,
    props_ptr: u32,
) -> i32 {
    let snapi = snapi_env(&env, e);
    let pc = prop_count;
    if pc == 0 {
        return unsafe {
            snapi_bridge_define_properties(
                snapi,
                obj,
                0,
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
            )
        };
    }

    // usize is 32 bits on this host, so a guest-supplied count can overflow.
    let Some(total_bytes) = (pc as usize).checked_mul(PROP_DESC_SIZE) else {
        return 1;
    };
    let Some(raw) = read_guest_bytes(&mut env, props_ptr, total_bytes) else {
        return 1;
    };

    let mut prop_names_c: Vec<CString> = Vec::with_capacity(pc as usize);
    let mut prop_names_ptrs: Vec<*const i8> = Vec::with_capacity(pc as usize);
    let mut prop_name_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_types: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_value_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_method_reg_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_getter_reg_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_setter_reg_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_attributes: Vec<i32> = Vec::with_capacity(pc as usize);

    for i in 0..pc as usize {
        let base = i * PROP_DESC_SIZE;
        let utf8name_guest =
            u32::from_le_bytes([raw[base], raw[base + 1], raw[base + 2], raw[base + 3]]);
        let name_id =
            u32::from_le_bytes([raw[base + 4], raw[base + 5], raw[base + 6], raw[base + 7]]);
        let method_ptr =
            u32::from_le_bytes([raw[base + 8], raw[base + 9], raw[base + 10], raw[base + 11]]);
        let getter_ptr = u32::from_le_bytes([
            raw[base + 12],
            raw[base + 13],
            raw[base + 14],
            raw[base + 15],
        ]);
        let setter_ptr = u32::from_le_bytes([
            raw[base + 16],
            raw[base + 17],
            raw[base + 18],
            raw[base + 19],
        ]);
        let value_id = u32::from_le_bytes([
            raw[base + 20],
            raw[base + 21],
            raw[base + 22],
            raw[base + 23],
        ]);
        let attrs = i32::from_le_bytes([
            raw[base + 24],
            raw[base + 25],
            raw[base + 26],
            raw[base + 27],
        ]);
        let data_ptr = u32::from_le_bytes([
            raw[base + 28],
            raw[base + 29],
            raw[base + 30],
            raw[base + 31],
        ]);

        let pname = if utf8name_guest != 0 {
            read_guest_c_string(&mut env, utf8name_guest).unwrap_or_default()
        } else {
            vec![]
        };
        let c_pname = CString::new(pname).unwrap_or_default();
        prop_name_ids.push(name_id);

        if method_ptr != 0 {
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id(snapi) };
            unsafe {
                snapi_bridge_register_callback(snapi, reg_id, e, method_ptr, data_ptr as u64)
            };
            prop_types.push(1);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(reg_id);
            prop_getter_reg_ids.push(0);
            prop_setter_reg_ids.push(0);
        } else if getter_ptr != 0 && setter_ptr != 0 {
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id(snapi) };
            unsafe {
                snapi_bridge_register_callback_pair(
                    snapi,
                    reg_id,
                    e,
                    getter_ptr,
                    setter_ptr,
                    data_ptr as u64,
                )
            };
            prop_types.push(4);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(reg_id);
            prop_setter_reg_ids.push(0);
        } else if getter_ptr != 0 {
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id(snapi) };
            unsafe {
                snapi_bridge_register_callback(snapi, reg_id, e, getter_ptr, data_ptr as u64)
            };
            prop_types.push(2);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(reg_id);
            prop_setter_reg_ids.push(0);
        } else if setter_ptr != 0 {
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id(snapi) };
            unsafe {
                snapi_bridge_register_callback(snapi, reg_id, e, setter_ptr, data_ptr as u64)
            };
            prop_types.push(3);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(0);
            prop_setter_reg_ids.push(reg_id);
        } else {
            prop_types.push(0);
            prop_value_ids.push(value_id);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(0);
            prop_setter_reg_ids.push(0);
        }

        prop_attributes.push(attrs);
        prop_names_c.push(c_pname);
    }

    for cn in &prop_names_c {
        prop_names_ptrs.push(cn.as_ptr());
    }

    unsafe {
        snapi_bridge_define_properties(
            snapi,
            obj,
            pc,
            prop_names_ptrs.as_ptr(),
            prop_name_ids.as_ptr(),
            prop_types.as_ptr(),
            prop_value_ids.as_ptr(),
            prop_method_reg_ids.as_ptr(),
            prop_getter_reg_ids.as_ptr(),
            prop_setter_reg_ids.as_ptr(),
            prop_attributes.as_ptr(),
        )
    }
}

// --- Script execution ---

fn guest_napi_run_script(mut env: FunctionEnvMut<NapiEnv>, e: u32, sh: u32, rp: u32) -> i32 {
    let snapi = snapi_env(&env, e);
    let mut out: u32 = 0;
    let s = with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_run_script(snapi, sh, &mut out)
    });

    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

// --- UTF-16 strings ---

fn guest_napi_create_string_utf16(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    str_ptr: u32,
    length: u32,
    rp: u32,
) -> i32 {
    let Some(value) =
        read_guest_string_value(&mut env, str_ptr, length, GuestStringEncoding::Utf16)
    else {
        return 1;
    };
    let mut out: u32 = 0;
    let s = unsafe { host_js_put_primitive(snapi_env(&env, e), value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_get_value_string_utf16(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    bp: u32,
    bs: u32,
    rp: u32,
) -> i32 {
    let value = match unsafe { host_js_value(snapi_env(&env, e), vh) } {
        Ok(value) => value,
        Err(status) => return status,
    };
    write_guest_string_value(&mut env, &value, bp, bs, rp, GuestStringEncoding::Utf16)
}

// --- BigInt words ---

fn guest_napi_create_bigint_words(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    sign_bit: i32,
    word_count: u32,
    words_ptr: u32,
    rp: u32,
) -> i32 {
    if word_count as usize > MAX_NAPI_BIGINT_WORDS {
        return 1;
    }
    let wc = word_count;
    // Read u64 words from guest memory (each is 8 bytes)
    let Some(byte_len) = (wc as usize).checked_mul(8) else {
        return 1;
    };
    let Some(words_bytes) = read_guest_bytes(&mut env, words_ptr, byte_len) else {
        return 1;
    };
    let words: Vec<u64> = words_bytes
        .chunks_exact(8)
        .map(|c| u64::from_le_bytes(c.try_into().unwrap()))
        .collect();
    let mut out: u32 = 0;
    let s = unsafe {
        snapi_bridge_create_bigint_words(snapi_env(&env, e), sign_bit, wc, words.as_ptr(), &mut out)
    };
    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

fn guest_napi_get_value_bigint_words(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    sign_ptr: u32,
    wc_ptr: u32,
    words_ptr: u32,
) -> i32 {
    // First, read the word_count from guest to know how many to allocate
    let Some(wc_bytes) = read_guest_bytes(&mut env, wc_ptr, 4) else {
        return 1;
    };
    let mut word_count = u32::from_le_bytes(wc_bytes.try_into().unwrap()) as usize;
    if word_count > MAX_NAPI_BIGINT_WORDS {
        return 1;
    }

    if words_ptr == 0 {
        // Query mode: just get the word count
        let mut sign: i32 = 0;
        let s = unsafe {
            snapi_bridge_get_value_bigint_words(
                snapi_env(&env, e),
                vh,
                &mut sign,
                &mut word_count,
                std::ptr::null_mut(),
            )
        };
        if s == 0 {
            write_guest_i32(&mut env, sign_ptr, sign);
            write_guest_u32(&mut env, wc_ptr, word_count as u32);
        }
        return s;
    }

    let mut sign: i32 = 0;
    let mut words = vec![0u64; word_count];
    let s = unsafe {
        snapi_bridge_get_value_bigint_words(
            snapi_env(&env, e),
            vh,
            &mut sign,
            &mut word_count,
            words.as_mut_ptr(),
        )
    };
    if s == 0 {
        write_guest_i32(&mut env, sign_ptr, sign);
        write_guest_u32(&mut env, wc_ptr, word_count as u32);
        // Write u64 words as LE bytes to guest
        let bytes: Vec<u8> = words[..word_count]
            .iter()
            .flat_map(|&v| v.to_le_bytes())
            .collect();
        write_guest_bytes(&mut env, words_ptr, &bytes);
    }
    s
}

// --- Instance data ---

fn guest_napi_set_instance_data(
    env: FunctionEnvMut<NapiEnv>,
    e: u32,
    data: u32,
    _finalize_cb: u32,
    _finalize_hint: u32,
) -> i32 {
    unsafe { snapi_bridge_set_instance_data(snapi_env(&env, e), data as u64) }
}

fn guest_napi_get_instance_data(mut env: FunctionEnvMut<NapiEnv>, e: u32, rp: u32) -> i32 {
    let mut data: u64 = 0;
    let s = unsafe { snapi_bridge_get_instance_data(snapi_env(&env, e), &mut data) };
    if s == 0 {
        write_guest_u32(&mut env, rp, data as u32);
    }
    s
}

fn guest_napi_adjust_external_memory(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    change: i64,
    rp: u32,
) -> i32 {
    let mut adjusted: i64 = 0;
    let s =
        unsafe { snapi_bridge_adjust_external_memory(snapi_env(&env, e), change, &mut adjusted) };
    if s == 0 {
        write_guest_i64(&mut env, rp, adjusted);
    }
    s
}

// --- Node Buffers ---

fn guest_napi_create_buffer(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    length: u32,
    data_ptr: u32,
    rp: u32,
) -> i32 {
    #[cfg(feature = "js")]
    {
        let Some(guest_ptr) = allocate_zeroed_guest_bytes(&mut env, length as usize) else {
            return 1;
        };
        let Some(memory) = env.data().memory.clone() else {
            recycle_guest_allocation(&mut env, guest_ptr);
            return 1;
        };
        let memory_buffer = memory.as_js().js_buffer();
        let mut out = 0u32;
        let status = crate::snapi_js::create_guest_buffer_view(
            snapi_env(&env, e),
            &memory_buffer,
            guest_ptr,
            length,
            &mut out,
        );
        if status != 0 {
            recycle_guest_allocation(&mut env, guest_ptr);
            return status;
        }
        let backing_store_token = host_backing_store_token(&env, e, out);
        remember_guest_backing_store(
            &mut env,
            e,
            out,
            backing_store_token,
            0,
            guest_ptr,
            length as usize,
            false,
        );
        write_guest_u32(&mut env, rp, out);
        if data_ptr != 0 {
            write_guest_u32(&mut env, data_ptr, guest_ptr);
        }
        return 0;
    }

    #[cfg(not(feature = "js"))]
    {
        // Buffers must be backed by guest linear memory (same pattern as create_arraybuffer)
        let malloc_fn = env.data().malloc_fn.clone();
        let memory = env.data().memory.clone();

        if let (Some(malloc_fn), Some(memory)) = (malloc_fn, memory) {
            // Allocate memory in the guest's linear memory
            let guest_ptr: u32 = {
                let (_, mut store_ref) = env.data_and_store_mut();
                match malloc_fn.call(&mut store_ref, length as i32) {
                    Ok(ptr) if ptr != 0 => ptr as u32,
                    _ => return 1,
                }
            };

            // Get host pointer corresponding to the guest allocation
            let host_addr: u64 = {
                let (_, store_ref) = env.data_and_store_mut();
                let view = memory.view(&store_ref);
                let host_base = view.data_ptr() as u64;
                host_base + guest_ptr as u64
            };

            // Zero-initialize the guest memory region
            if length != 0 {
                let zeros = vec![0u8; length as usize];
                write_guest_bytes(&mut env, guest_ptr, &zeros);
            }

            let mut buf_id: u32 = 0;
            let mut backing_store_token: u64 = 0;
            let s = unsafe {
                snapi_bridge_create_external_buffer(
                    snapi_env(&env, e),
                    host_addr,
                    length,
                    &mut backing_store_token,
                    &mut buf_id,
                )
            };
            if s != 0 {
                return s;
            }

            remember_guest_backing_store(
                &mut env,
                e,
                buf_id,
                backing_store_token,
                host_addr,
                guest_ptr,
                length as usize,
                false,
            );

            write_guest_u32(&mut env, rp, buf_id);
            if data_ptr != 0 {
                write_guest_u32(&mut env, data_ptr, guest_ptr);
            }
            0
        } else {
            // Fallback for non-WASIX: use bridge directly
            let mut host_data: u64 = 0;
            let mut out: u32 = 0;
            let s = unsafe {
                snapi_bridge_create_buffer(snapi_env(&env, e), length, &mut host_data, &mut out)
            };
            if s != 0 {
                return s;
            }
            write_guest_u32(&mut env, rp, out);
            0
        }
    }
}

fn guest_napi_create_buffer_copy(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    length: u32,
    data_ptr: u32,
    result_data_ptr: u32,
    rp: u32,
) -> i32 {
    // Read source data from guest memory first
    let Some(src_data) = read_guest_bytes(&mut env, data_ptr, length as usize) else {
        return 1;
    };

    #[cfg(feature = "js")]
    {
        // napi_create_buffer_copy() only exposes native memory when the caller
        // asks for result_data. Without that pointer, let JavaScript own the
        // copied Buffer directly; retaining a guest allocation would pin one
        // allocation per network chunk with no observable native benefit.
        if result_data_ptr == 0 {
            let mut out = 0u32;
            let status = unsafe {
                snapi_bridge_create_buffer_copy(
                    snapi_env(&env, e),
                    length,
                    src_data.as_ptr(),
                    std::ptr::null_mut(),
                    &mut out,
                )
            };
            if status == 0 {
                write_guest_u32(&mut env, rp, out);
            }
            return status;
        }

        // result_data must remain a real pointer into guest linear memory.
        let Some(guest_ptr) = allocate_guest_bytes(&mut env, &src_data) else {
            return 1;
        };
        let Some(memory) = env.data().memory.clone() else {
            return 1;
        };
        let memory_buffer = memory.as_js().js_buffer();
        let mut out = 0u32;
        let status = crate::snapi_js::create_guest_buffer_view(
            snapi_env(&env, e),
            &memory_buffer,
            guest_ptr,
            length,
            &mut out,
        );
        if status != 0 {
            return status;
        }
        let backing_store_token = host_backing_store_token(&env, e, out);
        remember_guest_backing_store(
            &mut env,
            e,
            out,
            backing_store_token,
            0,
            guest_ptr,
            length as usize,
            false,
        );
        write_guest_u32(&mut env, rp, out);
        if result_data_ptr != 0 {
            write_guest_u32(&mut env, result_data_ptr, guest_ptr);
        }
        return 0;
    }

    #[cfg(not(feature = "js"))]
    {
        let malloc_fn = env.data().malloc_fn.clone();
        let memory = env.data().memory.clone();

        if let (Some(malloc_fn), Some(memory)) = (malloc_fn, memory) {
            // Allocate memory in the guest's linear memory
            let guest_ptr: u32 = {
                let (_, mut store_ref) = env.data_and_store_mut();
                match malloc_fn.call(&mut store_ref, length as i32) {
                    Ok(ptr) if ptr != 0 => ptr as u32,
                    _ => return 1,
                }
            };

            // Copy source data to guest memory
            write_guest_bytes(&mut env, guest_ptr, &src_data);

            // Get host pointer corresponding to the guest allocation
            let host_addr: u64 = {
                let (_, store_ref) = env.data_and_store_mut();
                let view = memory.view(&store_ref);
                let host_base = view.data_ptr() as u64;
                host_base + guest_ptr as u64
            };

            let mut buf_id: u32 = 0;
            let mut backing_store_token: u64 = 0;
            let s = unsafe {
                snapi_bridge_create_external_buffer(
                    snapi_env(&env, e),
                    host_addr,
                    length,
                    &mut backing_store_token,
                    &mut buf_id,
                )
            };
            if s != 0 {
                return s;
            }

            remember_guest_backing_store(
                &mut env,
                e,
                buf_id,
                backing_store_token,
                host_addr,
                guest_ptr,
                length as usize,
                false,
            );

            write_guest_u32(&mut env, rp, buf_id);
            if result_data_ptr != 0 {
                write_guest_u32(&mut env, result_data_ptr, guest_ptr);
            }
            0
        } else {
            // Fallback for non-WASIX
            let mut result_host_data: u64 = 0;
            let mut out: u32 = 0;
            let s = unsafe {
                snapi_bridge_create_buffer_copy(
                    snapi_env(&env, e),
                    length,
                    src_data.as_ptr(),
                    &mut result_host_data,
                    &mut out,
                )
            };
            if s == 0 {
                write_guest_u32(&mut env, rp, out);
            }
            s
        }
    }
}

guest_is_check!(guest_napi_is_buffer, snapi_bridge_is_buffer);

fn guest_napi_get_buffer_info(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    data_ptr: u32,
    len_ptr: u32,
) -> i32 {
    let (bl, backing_store_token) =
        match crate::snapi_js::get_value_byte_length_and_token(snapi_env(&env, e), vh) {
            Ok(info) => info,
            Err(status) => return status,
        };
    if len_ptr != 0 {
        write_guest_u32(&mut env, len_ptr, bl);
    }
    let guest_data_ptr = if data_ptr != 0 {
        resolve_current_host_value_to_guest(
            &mut env,
            e,
            vh,
            backing_store_token,
            bl as usize,
            false,
        )
    } else {
        None
    };
    if let Some(guest_data_ptr) = guest_data_ptr {
        write_guest_u32(&mut env, data_ptr, guest_data_ptr);
    }
    0
}

fn guest_unofficial_napi_acquire_buffer_lease(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    vh: u32,
    byte_offset: u32,
    byte_length: u32,
    mode: i32,
    lease_out: u32,
    data_out: u32,
) -> i32 {
    if lease_out == 0 || data_out == 0 || mode & !3 != 0 || mode == 0 {
        return 1;
    }
    let snapi = snapi_env(&env, e);
    let (value_len, _) = match crate::snapi_js::get_value_byte_length_and_token(snapi, vh) {
        Ok(info) => info,
        Err(status) => return status,
    };
    let offset = byte_offset;
    let len = byte_length;
    if offset > value_len || len > value_len - offset {
        return 1;
    }

    let Some(guest_ptr) = allocate_guest_buffer(&mut env, len as usize) else {
        return 9;
    };
    if mode & 1 != 0 && len > 0 {
        let Some(memory) = env.data().memory.clone() else {
            recycle_guest_allocation(&mut env, guest_ptr);
            return 9;
        };
        let status = crate::snapi_js::copy_value_range_to_memory(
            snapi,
            vh,
            &memory.as_js().js_buffer(),
            guest_ptr,
            offset,
            len,
        );
        if status != 0 {
            recycle_guest_allocation(&mut env, guest_ptr);
            return status;
        }
    }

    let mut host_reference_id = 0u32;
    let status = unsafe { snapi_bridge_create_reference(snapi, vh, 1, &mut host_reference_id) };
    if status != 0 {
        recycle_guest_allocation(&mut env, guest_ptr);
        return status;
    }

    let lease_id = env.data().next_buffer_lease_id.max(1);
    env.data_mut().next_buffer_lease_id = lease_id.wrapping_add(1).max(1);
    if env
        .data_mut()
        .host_buffer_leases
        .insert(
            (e, lease_id),
            crate::env::HostBufferLease {
                guest_ptr,
                byte_offset: offset,
                byte_len: len,
                writable: mode & 2 != 0,
                host_reference_id,
            },
        )
        .is_some()
    {
        let _ = unsafe { snapi_bridge_delete_reference(snapi, host_reference_id) };
        recycle_guest_allocation(&mut env, guest_ptr);
        return 9;
    }
    write_guest_u32(&mut env, lease_out, lease_id);
    write_guest_u32(&mut env, data_out, guest_ptr);
    0
}

fn guest_unofficial_napi_release_buffer_lease(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    lease_id: u32,
    modified: i32,
) -> i32 {
    if lease_id == 0 {
        return 1;
    }
    let Some(lease) = env.data_mut().host_buffer_leases.remove(&(e, lease_id)) else {
        return 1;
    };
    let snapi = snapi_env(&env, e);
    let mut status = 0;
    if lease.writable && modified != 0 && lease.byte_len > 0 {
        let Some(memory) = env.data().memory.clone() else {
            recycle_guest_allocation(&mut env, lease.guest_ptr);
            let _ = unsafe { snapi_bridge_delete_reference(snapi, lease.host_reference_id) };
            return 9;
        };
        status = crate::snapi_js::copy_memory_range_to_reference(
            snapi,
            lease.host_reference_id,
            &memory.as_js().js_buffer(),
            lease.guest_ptr,
            lease.byte_offset,
            lease.byte_len,
        );
    }
    let delete_status = unsafe { snapi_bridge_delete_reference(snapi, lease.host_reference_id) };
    recycle_guest_allocation(&mut env, lease.guest_ptr);
    if status != 0 { status } else { delete_status }
}

fn guest_unofficial_napi_create_guest_backed_typedarray(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    typ: i32,
    length: u32,
    data_out: u32,
    result_out: u32,
) -> i32 {
    if data_out == 0 || result_out == 0 {
        return 1;
    }
    let Some(element_size) = typedarray_element_size(typ) else {
        return 1;
    };
    let Some(byte_len) = (length as usize).checked_mul(element_size) else {
        return 1;
    };
    let Some(guest_ptr) = allocate_guest_buffer(&mut env, byte_len) else {
        return 9;
    };
    if byte_len > 0 && !write_guest_bytes(&mut env, guest_ptr, &vec![0; byte_len]) {
        recycle_guest_allocation(&mut env, guest_ptr);
        return 9;
    }
    let Some(memory) = env.data().memory.clone() else {
        recycle_guest_allocation(&mut env, guest_ptr);
        return 9;
    };
    let memory_buffer = memory.as_js().js_buffer();
    let mut out = 0u32;
    let status = crate::snapi_js::create_guest_typedarray_view(
        snapi_env(&env, e),
        &memory_buffer,
        typ,
        guest_ptr,
        length,
        &mut out,
    );
    if status != 0 {
        recycle_guest_allocation(&mut env, guest_ptr);
        return status;
    }
    let backing_store_token = host_backing_store_token(&env, e, out);
    remember_guest_backing_store(
        &mut env,
        e,
        out,
        backing_store_token,
        0,
        guest_ptr,
        byte_len,
        false,
    );
    write_guest_u32(&mut env, data_out, guest_ptr);
    write_guest_u32(&mut env, result_out, out);
    0
}

// --- Node version ---

fn guest_napi_get_node_version(mut env: FunctionEnvMut<NapiEnv>, e: u32, rp: u32) -> i32 {
    let mut major: u32 = 0;
    let mut minor: u32 = 0;
    let mut patch: u32 = 0;
    let s = unsafe {
        snapi_bridge_get_node_version(snapi_env(&env, e), &mut major, &mut minor, &mut patch)
    };
    if s != 0 {
        return s;
    }
    // Write a napi_node_version struct to guest memory:
    // { uint32_t major, uint32_t minor, uint32_t patch, const char* release }
    // For WASM: we write the struct (16 bytes) into a static-ish location
    // But the N-API spec says we return a *pointer* to the version struct.
    // Actually, the API signature is: napi_get_node_version(env, const napi_node_version** version)
    // So we need to allocate memory in guest for the struct and write the pointer.
    // For simplicity, use malloc if available, otherwise just write the pointer to a fixed value.
    let malloc_fn = env.data().malloc_fn.clone();
    if let Some(malloc_fn) = malloc_fn {
        // Allocate 16 bytes for the struct (major, minor, patch, release_ptr)
        // Then allocate a small buffer for the release string
        let release_str = b"napi-external\0";
        let struct_size = 16u32; // 4 * u32 = 16 (release is a pointer)
        let total = struct_size + release_str.len() as u32;
        let guest_ptr: u32 = {
            let (_, mut store_ref) = env.data_and_store_mut();
            match malloc_fn.call(&mut store_ref, total as i32) {
                Ok(ptr) if ptr != 0 => ptr as u32,
                _ => return 1,
            }
        };
        // Write release string
        let release_offset = guest_ptr + struct_size;
        write_guest_bytes(&mut env, release_offset, release_str);
        // Write struct fields
        write_guest_u32(&mut env, guest_ptr, major);
        write_guest_u32(&mut env, guest_ptr + 4, minor);
        write_guest_u32(&mut env, guest_ptr + 8, patch);
        write_guest_u32(&mut env, guest_ptr + 12, release_offset);
        // Write pointer to struct
        write_guest_u32(&mut env, rp, guest_ptr);
    } else {
        // Fallback: just write major version as a simple value
        write_guest_u32(&mut env, rp, major);
    }
    0
}

// --- Object wrapping ---

fn guest_napi_wrap(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    obj: u32,
    native_data: u32,
    finalize_cb: u32,
    finalize_hint: u32,
    ref_ptr: u32,
) -> i32 {
    if finalize_cb != 0 {
        let mut ref_out = 0u32;
        let status = unsafe {
            snapi_bridge_wrap_finalized(
                snapi_env(&env, e),
                obj,
                native_data as u64,
                e,
                finalize_cb,
                native_data,
                finalize_hint,
                if ref_ptr != 0 {
                    &mut ref_out
                } else {
                    std::ptr::null_mut()
                },
            )
        };
        if status == 0 && ref_ptr != 0 {
            write_guest_u32(&mut env, ref_ptr, ref_out);
        }
        return status;
    }
    let mut ref_out: u32 = 0;
    let s = if ref_ptr != 0 {
        unsafe { snapi_bridge_wrap(snapi_env(&env, e), obj, native_data as u64, &mut ref_out) }
    } else {
        unsafe {
            snapi_bridge_wrap(
                snapi_env(&env, e),
                obj,
                native_data as u64,
                std::ptr::null_mut(),
            )
        }
    };
    if s == 0 && ref_ptr != 0 {
        write_guest_u32(&mut env, ref_ptr, ref_out);
    }
    s
}

fn guest_napi_unwrap(mut env: FunctionEnvMut<NapiEnv>, e: u32, obj: u32, rp: u32) -> i32 {
    let mut data: u64 = 0;
    let s = unsafe { snapi_bridge_unwrap(snapi_env(&env, e), obj, &mut data) };
    if s == 0 {
        write_guest_u32(&mut env, rp, data as u32);
    }
    s
}

fn guest_napi_remove_wrap(mut env: FunctionEnvMut<NapiEnv>, e: u32, obj: u32, rp: u32) -> i32 {
    let mut data: u64 = 0;
    let s = unsafe { snapi_bridge_remove_wrap(snapi_env(&env, e), obj, &mut data) };
    if s == 0 {
        write_guest_u32(&mut env, rp, data as u32);
    }
    s
}

fn guest_napi_add_finalizer(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    obj: u32,
    data: u32,
    finalize_cb: u32,
    finalize_hint: u32,
    ref_ptr: u32,
) -> i32 {
    let mut ref_out: u32 = 0;
    let s = if finalize_cb != 0 {
        unsafe {
            snapi_bridge_add_finalizer_cb(
                snapi_env(&env, e),
                obj,
                data as u64,
                e,
                finalize_cb,
                data,
                finalize_hint,
                if ref_ptr != 0 {
                    &mut ref_out
                } else {
                    std::ptr::null_mut()
                },
            )
        }
    } else if ref_ptr != 0 {
        unsafe { snapi_bridge_add_finalizer(snapi_env(&env, e), obj, data as u64, &mut ref_out) }
    } else {
        unsafe {
            snapi_bridge_add_finalizer(snapi_env(&env, e), obj, data as u64, std::ptr::null_mut())
        }
    };
    if s == 0 && ref_ptr != 0 {
        write_guest_u32(&mut env, ref_ptr, ref_out);
    }
    s
}

// --- Misc stubs ---

fn guest_napi_get_last_error_info(_env: FunctionEnvMut<NapiEnv>, _e: u32, _rp: u32) -> i32 {
    0
}

fn guest_napi_get_version(mut env: FunctionEnvMut<NapiEnv>, _e: u32, rp: u32) -> i32 {
    write_guest_u32(&mut env, rp, 8);
    0
}

fn guest_napi_fatal_error(
    mut env: FunctionEnvMut<NapiEnv>,
    loc_ptr: u32,
    loc_len: u32,
    msg_ptr: u32,
    msg_len: u32,
) -> i32 {
    // Read location and message from guest memory
    let loc = if loc_ptr != 0 {
        let len = if loc_len == 0xFFFFFFFFu32 {
            read_guest_c_string(&mut env, loc_ptr)
                .map(|v| v.len())
                .unwrap_or(0)
        } else {
            loc_len as usize
        };
        read_guest_bytes(&mut env, loc_ptr, len).map(|b| String::from_utf8_lossy(&b).to_string())
    } else {
        None
    };
    let msg = if msg_ptr != 0 {
        let len = if msg_len == 0xFFFFFFFFu32 {
            read_guest_c_string(&mut env, msg_ptr)
                .map(|v| v.len())
                .unwrap_or(0)
        } else {
            msg_len as usize
        };
        read_guest_bytes(&mut env, msg_ptr, len).map(|b| String::from_utf8_lossy(&b).to_string())
    } else {
        None
    };
    eprintln!(
        "FATAL ERROR: location={}, message={}",
        loc.as_deref().unwrap_or("(null)"),
        msg.as_deref().unwrap_or("(null)")
    );
    std::process::abort();
}

// --- Constructor ---

fn guest_napi_new_instance(
    mut env: FunctionEnvMut<NapiEnv>,
    e: u32,
    ctor: u32,
    argc: u32,
    argv_ptr: u32,
    rp: u32,
) -> i32 {
    let argc_u = argc;
    let argv_ids = if argc_u > 0 {
        let Some(ids) = read_guest_u32_array(&mut env, argv_ptr, argc_u as usize) else {
            return 1;
        };
        ids
    } else {
        vec![]
    };

    let snapi = snapi_env(&env, e);
    let mut out: u32 = 0;
    let s = with_cb_context(&mut env, e, || unsafe {
        snapi_bridge_new_instance(snapi, ctor, argc_u, argv_ids.as_ptr(), &mut out)
    });

    if s == 0 {
        write_guest_u32(&mut env, rp, out);
    }
    s
}

// ============================================================
// Register WASM imports for both the core "napi" module and the
// Wasmer-specific "napi_extension_wasmer_v0" extension module.
// ============================================================

pub(crate) fn is_known_napi_import(name: &str) -> bool {
    matches!(
        name,
        "napi_wasm_init_env"
            | "napi_get_undefined"
            | "napi_get_null"
            | "napi_get_boolean"
            | "napi_get_global"
            | "napi_create_string_utf8"
            | "napi_create_string_latin1"
            | "napi_create_int32"
            | "napi_create_uint32"
            | "napi_create_double"
            | "napi_create_int64"
            | "napi_create_object"
            | "napi_create_array"
            | "napi_create_array_with_length"
            | "napi_create_symbol"
            | "napi_create_error"
            | "napi_create_type_error"
            | "napi_create_range_error"
            | "napi_create_bigint_int64"
            | "napi_create_bigint_uint64"
            | "napi_create_date"
            | "napi_create_external"
            | "napi_create_arraybuffer"
            | "napi_create_external_arraybuffer"
            | "napi_create_external_buffer"
            | "napi_create_typedarray"
            | "napi_create_dataview"
            | "napi_create_promise"
            | "napi_get_value_string_utf8"
            | "napi_get_value_string_latin1"
            | "napi_get_value_int32"
            | "napi_get_value_uint32"
            | "napi_get_value_double"
            | "napi_get_value_int64"
            | "napi_get_value_bool"
            | "napi_get_value_bigint_int64"
            | "napi_get_value_bigint_uint64"
            | "napi_get_date_value"
            | "napi_get_value_external"
            | "napi_typeof"
            | "napi_is_array"
            | "napi_is_error"
            | "napi_is_arraybuffer"
            | "napi_is_typedarray"
            | "napi_is_dataview"
            | "napi_is_date"
            | "napi_is_promise"
            | "napi_instanceof"
            | "napi_coerce_to_bool"
            | "napi_coerce_to_number"
            | "napi_coerce_to_string"
            | "napi_coerce_to_object"
            | "napi_set_property"
            | "napi_get_property"
            | "napi_has_property"
            | "napi_has_own_property"
            | "napi_delete_property"
            | "napi_set_named_property"
            | "napi_get_named_property"
            | "napi_has_named_property"
            | "napi_set_element"
            | "napi_get_element"
            | "napi_has_element"
            | "napi_delete_element"
            | "napi_get_array_length"
            | "napi_get_property_names"
            | "napi_get_all_property_names"
            | "napi_get_prototype"
            | "napi_object_freeze"
            | "napi_object_seal"
            | "napi_strict_equals"
            | "napi_throw"
            | "napi_throw_error"
            | "napi_throw_type_error"
            | "napi_throw_range_error"
            | "napi_is_exception_pending"
            | "napi_get_and_clear_last_exception"
            | "napi_resolve_deferred"
            | "napi_reject_deferred"
            | "napi_get_arraybuffer_info"
            | "napi_detach_arraybuffer"
            | "napi_is_detached_arraybuffer"
            | "node_api_is_sharedarraybuffer"
            | "node_api_create_sharedarraybuffer"
            | "node_api_set_prototype"
            | "napi_get_typedarray_info"
            | "napi_get_dataview_info"
            | "napi_create_reference"
            | "napi_delete_reference"
            | "napi_reference_ref"
            | "napi_reference_unref"
            | "napi_get_reference_value"
            | "napi_open_handle_scope"
            | "napi_close_handle_scope"
            | "napi_open_escapable_handle_scope"
            | "napi_close_escapable_handle_scope"
            | "napi_escape_handle"
            | "napi_type_tag_object"
            | "napi_check_object_type_tag"
            | "napi_call_function"
            | "napi_create_function"
            | "napi_get_cb_info"
            | "napi_get_new_target"
            | "napi_run_script"
            | "napi_create_string_utf16"
            | "napi_get_value_string_utf16"
            | "napi_create_bigint_words"
            | "napi_get_value_bigint_words"
            | "napi_set_instance_data"
            | "napi_get_instance_data"
            | "napi_adjust_external_memory"
            | "napi_create_buffer"
            | "napi_create_buffer_copy"
            | "napi_is_buffer"
            | "napi_get_buffer_info"
            | "napi_get_node_version"
            | "napi_wrap"
            | "napi_unwrap"
            | "napi_remove_wrap"
            | "napi_add_finalizer"
            | "napi_new_instance"
            | "napi_define_properties"
            | "napi_define_class"
            | "napi_fatal_error"
            | "napi_get_last_error_info"
            | "napi_get_version"
            | "napi_add_env_cleanup_hook"
            | "napi_remove_env_cleanup_hook"
    )
}

pub fn register_napi_imports(
    store: &mut impl AsStoreMut,
    fe: &FunctionEnv<NapiEnv>,
    io: &mut Imports,
) {
    let napi_namespace = namespace! {
        "napi_wasm_init_env" => Function::new_typed_with_env(store, fe, guest_napi_wasm_init_env),
        "napi_get_undefined" => Function::new_typed_with_env(store, fe, guest_napi_get_undefined),
        "napi_get_null" => Function::new_typed_with_env(store, fe, guest_napi_get_null),
        "napi_get_boolean" => Function::new_typed_with_env(store, fe, guest_napi_get_boolean),
        "napi_get_global" => Function::new_typed_with_env(store, fe, guest_napi_get_global),
        "napi_create_string_utf8" => Function::new_typed_with_env(store, fe, guest_napi_create_string_utf8),
        "napi_create_string_latin1" => Function::new_typed_with_env(store, fe, guest_napi_create_string_latin1),
        "napi_create_int32" => Function::new_typed_with_env(store, fe, guest_napi_create_int32),
        "napi_create_uint32" => Function::new_typed_with_env(store, fe, guest_napi_create_uint32),
        "napi_create_double" => Function::new_typed_with_env(store, fe, guest_napi_create_double),
        "napi_create_int64" => Function::new_typed_with_env(store, fe, guest_napi_create_int64),
        "napi_create_object" => Function::new_typed_with_env(store, fe, guest_napi_create_object),
        "napi_create_array" => Function::new_typed_with_env(store, fe, guest_napi_create_array),
        "napi_create_array_with_length" => Function::new_typed_with_env(store, fe, guest_napi_create_array_with_length),
        "napi_create_symbol" => Function::new_typed_with_env(store, fe, guest_napi_create_symbol),
        "napi_create_error" => Function::new_typed_with_env(store, fe, guest_napi_create_error),
        "napi_create_type_error" => Function::new_typed_with_env(store, fe, guest_napi_create_type_error),
        "napi_create_range_error" => Function::new_typed_with_env(store, fe, guest_napi_create_range_error),
        "napi_create_bigint_int64" => Function::new_typed_with_env(store, fe, guest_napi_create_bigint_int64),
        "napi_create_bigint_uint64" => Function::new_typed_with_env(store, fe, guest_napi_create_bigint_uint64),
        "napi_create_date" => Function::new_typed_with_env(store, fe, guest_napi_create_date),
        "napi_create_external" => Function::new_typed_with_env(store, fe, guest_napi_create_external),
        "napi_create_arraybuffer" => Function::new_typed_with_env(store, fe, guest_napi_create_arraybuffer),
        "napi_create_external_arraybuffer" => Function::new_typed_with_env(store, fe, guest_napi_create_external_arraybuffer),
        "napi_create_external_buffer" => Function::new_typed_with_env(store, fe, guest_napi_create_external_buffer),
        "napi_create_typedarray" => Function::new_typed_with_env(store, fe, guest_napi_create_typedarray),
        "napi_create_dataview" => Function::new_typed_with_env(store, fe, guest_napi_create_dataview),
        "napi_create_promise" => Function::new_typed_with_env(store, fe, guest_napi_create_promise),
        "napi_get_value_string_utf8" => Function::new_typed_with_env(store, fe, guest_napi_get_value_string_utf8),
        "napi_get_value_string_latin1" => Function::new_typed_with_env(store, fe, guest_napi_get_value_string_latin1),
        "napi_get_value_int32" => Function::new_typed_with_env(store, fe, guest_napi_get_value_int32),
        "napi_get_value_uint32" => Function::new_typed_with_env(store, fe, guest_napi_get_value_uint32),
        "napi_get_value_double" => Function::new_typed_with_env(store, fe, guest_napi_get_value_double),
        "napi_get_value_int64" => Function::new_typed_with_env(store, fe, guest_napi_get_value_int64),
        "napi_get_value_bool" => Function::new_typed_with_env(store, fe, guest_napi_get_value_bool),
        "napi_get_value_bigint_int64" => Function::new_typed_with_env(store, fe, guest_napi_get_value_bigint_int64),
        "napi_get_value_bigint_uint64" => Function::new_typed_with_env(store, fe, guest_napi_get_value_bigint_uint64),
        "napi_get_date_value" => Function::new_typed_with_env(store, fe, guest_napi_get_date_value),
        "napi_get_value_external" => Function::new_typed_with_env(store, fe, guest_napi_get_value_external),
        "napi_typeof" => Function::new_typed_with_env(store, fe, guest_napi_typeof),
        "napi_is_array" => Function::new_typed_with_env(store, fe, guest_napi_is_array),
        "napi_is_error" => Function::new_typed_with_env(store, fe, guest_napi_is_error),
        "napi_is_arraybuffer" => Function::new_typed_with_env(store, fe, guest_napi_is_arraybuffer),
        "napi_is_typedarray" => Function::new_typed_with_env(store, fe, guest_napi_is_typedarray),
        "napi_is_dataview" => Function::new_typed_with_env(store, fe, guest_napi_is_dataview),
        "napi_is_date" => Function::new_typed_with_env(store, fe, guest_napi_is_date),
        "napi_is_promise" => Function::new_typed_with_env(store, fe, guest_napi_is_promise),
        "napi_instanceof" => Function::new_typed_with_env(store, fe, guest_napi_instanceof),
        "napi_coerce_to_bool" => Function::new_typed_with_env(store, fe, guest_napi_coerce_to_bool),
        "napi_coerce_to_number" => Function::new_typed_with_env(store, fe, guest_napi_coerce_to_number),
        "napi_coerce_to_string" => Function::new_typed_with_env(store, fe, guest_napi_coerce_to_string),
        "napi_coerce_to_object" => Function::new_typed_with_env(store, fe, guest_napi_coerce_to_object),
        "napi_set_property" => Function::new_typed_with_env(store, fe, guest_napi_set_property),
        "napi_get_property" => Function::new_typed_with_env(store, fe, guest_napi_get_property),
        "napi_has_property" => Function::new_typed_with_env(store, fe, guest_napi_has_property),
        "napi_has_own_property" => Function::new_typed_with_env(store, fe, guest_napi_has_own_property),
        "napi_delete_property" => Function::new_typed_with_env(store, fe, guest_napi_delete_property),
        "napi_set_named_property" => Function::new_typed_with_env(store, fe, guest_napi_set_named_property),
        "napi_get_named_property" => Function::new_typed_with_env(store, fe, guest_napi_get_named_property),
        "napi_has_named_property" => Function::new_typed_with_env(store, fe, guest_napi_has_named_property),
        "napi_set_element" => Function::new_typed_with_env(store, fe, guest_napi_set_element),
        "napi_get_element" => Function::new_typed_with_env(store, fe, guest_napi_get_element),
        "napi_has_element" => Function::new_typed_with_env(store, fe, guest_napi_has_element),
        "napi_delete_element" => Function::new_typed_with_env(store, fe, guest_napi_delete_element),
        "napi_get_array_length" => Function::new_typed_with_env(store, fe, guest_napi_get_array_length),
        "napi_get_property_names" => Function::new_typed_with_env(store, fe, guest_napi_get_property_names),
        "napi_get_all_property_names" => Function::new_typed_with_env(store, fe, guest_napi_get_all_property_names),
        "napi_get_prototype" => Function::new_typed_with_env(store, fe, guest_napi_get_prototype),
        "napi_object_freeze" => Function::new_typed_with_env(store, fe, guest_napi_object_freeze),
        "napi_object_seal" => Function::new_typed_with_env(store, fe, guest_napi_object_seal),
        "napi_strict_equals" => Function::new_typed_with_env(store, fe, guest_napi_strict_equals),
        "napi_throw" => Function::new_typed_with_env(store, fe, guest_napi_throw),
        "napi_throw_error" => Function::new_typed_with_env(store, fe, guest_napi_throw_error),
        "napi_throw_type_error" => Function::new_typed_with_env(store, fe, guest_napi_throw_type_error),
        "napi_throw_range_error" => Function::new_typed_with_env(store, fe, guest_napi_throw_range_error),
        "napi_is_exception_pending" => Function::new_typed_with_env(store, fe, guest_napi_is_exception_pending),
        "napi_get_and_clear_last_exception" => Function::new_typed_with_env(store, fe, guest_napi_get_and_clear_last_exception),
        "napi_resolve_deferred" => Function::new_typed_with_env(store, fe, guest_napi_resolve_deferred),
        "napi_reject_deferred" => Function::new_typed_with_env(store, fe, guest_napi_reject_deferred),
        "napi_get_arraybuffer_info" => Function::new_typed_with_env(store, fe, guest_napi_get_arraybuffer_info),
        "napi_detach_arraybuffer" => Function::new_typed_with_env(store, fe, guest_napi_detach_arraybuffer),
        "napi_is_detached_arraybuffer" => Function::new_typed_with_env(store, fe, guest_napi_is_detached_arraybuffer),
        "node_api_is_sharedarraybuffer" => Function::new_typed_with_env(store, fe, guest_node_api_is_sharedarraybuffer),
        "node_api_create_sharedarraybuffer" => Function::new_typed_with_env(store, fe, guest_node_api_create_sharedarraybuffer),
        "node_api_set_prototype" => Function::new_typed_with_env(store, fe, guest_node_api_set_prototype),
        "napi_get_typedarray_info" => Function::new_typed_with_env(store, fe, guest_napi_get_typedarray_info),
        "napi_get_dataview_info" => Function::new_typed_with_env(store, fe, guest_napi_get_dataview_info),
        "napi_create_reference" => Function::new_typed_with_env(store, fe, guest_napi_create_reference),
        "napi_delete_reference" => Function::new_typed_with_env(store, fe, guest_napi_delete_reference),
        "napi_reference_ref" => Function::new_typed_with_env(store, fe, guest_napi_reference_ref),
        "napi_reference_unref" => Function::new_typed_with_env(store, fe, guest_napi_reference_unref),
        "napi_get_reference_value" => Function::new_typed_with_env(store, fe, guest_napi_get_reference_value),
        "napi_open_handle_scope" => Function::new_typed_with_env(store, fe, guest_napi_open_handle_scope),
        "napi_close_handle_scope" => Function::new_typed_with_env(store, fe, guest_napi_close_handle_scope),
        "napi_open_escapable_handle_scope" => Function::new_typed_with_env(store, fe, guest_napi_open_escapable_handle_scope),
        "napi_close_escapable_handle_scope" => Function::new_typed_with_env(store, fe, guest_napi_close_escapable_handle_scope),
        "napi_escape_handle" => Function::new_typed_with_env(store, fe, guest_napi_escape_handle),
        "napi_type_tag_object" => Function::new_typed_with_env(store, fe, guest_napi_type_tag_object),
        "napi_check_object_type_tag" => Function::new_typed_with_env(store, fe, guest_napi_check_object_type_tag),
        "napi_call_function" => Function::new_typed_with_env(store, fe, guest_napi_call_function),
        "napi_create_function" => Function::new_typed_with_env(store, fe, guest_napi_create_function),
        "napi_get_cb_info" => Function::new_typed_with_env(store, fe, guest_napi_get_cb_info),
        "napi_get_new_target" => Function::new_typed_with_env(store, fe, guest_napi_get_new_target),
        "napi_run_script" => Function::new_typed_with_env(store, fe, guest_napi_run_script),
        "napi_create_string_utf16" => Function::new_typed_with_env(store, fe, guest_napi_create_string_utf16),
        "napi_get_value_string_utf16" => Function::new_typed_with_env(store, fe, guest_napi_get_value_string_utf16),
        "napi_create_bigint_words" => Function::new_typed_with_env(store, fe, guest_napi_create_bigint_words),
        "napi_get_value_bigint_words" => Function::new_typed_with_env(store, fe, guest_napi_get_value_bigint_words),
        "napi_set_instance_data" => Function::new_typed_with_env(store, fe, guest_napi_set_instance_data),
        "napi_get_instance_data" => Function::new_typed_with_env(store, fe, guest_napi_get_instance_data),
        "napi_adjust_external_memory" => Function::new_typed_with_env(store, fe, guest_napi_adjust_external_memory),
        "napi_create_buffer" => Function::new_typed_with_env(store, fe, guest_napi_create_buffer),
        "napi_create_buffer_copy" => Function::new_typed_with_env(store, fe, guest_napi_create_buffer_copy),
        "napi_is_buffer" => Function::new_typed_with_env(store, fe, guest_napi_is_buffer),
        "napi_get_buffer_info" => Function::new_typed_with_env(store, fe, guest_napi_get_buffer_info),
        "napi_get_node_version" => Function::new_typed_with_env(store, fe, guest_napi_get_node_version),
        "napi_wrap" => Function::new_typed_with_env(store, fe, guest_napi_wrap),
        "napi_unwrap" => Function::new_typed_with_env(store, fe, guest_napi_unwrap),
        "napi_remove_wrap" => Function::new_typed_with_env(store, fe, guest_napi_remove_wrap),
        "napi_add_finalizer" => Function::new_typed_with_env(store, fe, guest_napi_add_finalizer),
        "napi_new_instance" => Function::new_typed_with_env(store, fe, guest_napi_new_instance),
        "napi_define_properties" => Function::new_typed_with_env(store, fe, guest_napi_define_properties),
        "napi_define_class" => Function::new_typed_with_env(store, fe, guest_napi_define_class),
        "napi_fatal_error" => Function::new_typed_with_env(store, fe, guest_napi_fatal_error),
        "napi_get_last_error_info" => Function::new_typed_with_env(store, fe, guest_napi_get_last_error_info),
        "napi_get_version" => Function::new_typed_with_env(store, fe, guest_napi_get_version),
        "napi_add_env_cleanup_hook" => Function::new_typed_with_env(store, fe, guest_napi_add_env_cleanup_hook),
        "napi_remove_env_cleanup_hook" => Function::new_typed_with_env(store, fe, guest_napi_remove_env_cleanup_hook),
    };

    let napi_extension_wasmer_namespace = namespace! {
        "unofficial_napi_configure_runtime" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_configure_runtime),
        "unofficial_napi_create_env" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_create_env),
        "unofficial_napi_attach_env" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_attach_env),
        "unofficial_napi_release_env" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_release_env),
        "unofficial_napi_collect_garbage" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_collect_garbage),
        "unofficial_napi_event_loop_checkpoint" => event_loop_checkpoint_import(store, fe),
        "unofficial_napi_create_uninitialized_arraybuffer" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_create_uninitialized_arraybuffer),
        "unofficial_napi_acquire_buffer_lease" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_acquire_buffer_lease),
        "unofficial_napi_release_buffer_lease" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_release_buffer_lease),
        "unofficial_napi_create_guest_backed_typedarray" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_create_guest_backed_typedarray),
        "unofficial_napi_set_prepare_stack_trace_callback" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_set_prepare_stack_trace_callback),
        "unofficial_napi_get_promise_details" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_get_promise_details),
        "unofficial_napi_get_proxy_details" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_get_proxy_details),
        "unofficial_napi_preview_entries" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_preview_entries),
        "unofficial_napi_get_call_sites" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_get_call_sites),
        "unofficial_napi_arraybuffer_view_has_buffer" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_arraybuffer_view_has_buffer),
        "unofficial_napi_get_constructor_name" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_get_constructor_name),
        "unofficial_napi_create_private_symbol" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_create_private_symbol),
        "unofficial_napi_get_continuation_preserved_embedder_data" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_get_continuation_preserved_embedder_data),
        "unofficial_napi_set_continuation_preserved_embedder_data" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_set_continuation_preserved_embedder_data),
        "unofficial_napi_notify_datetime_configuration_change" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_notify_datetime_configuration_change),
        "unofficial_napi_terminate_execution" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_terminate_execution),
        "unofficial_napi_cancel_terminate_execution" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_cancel_terminate_execution),
        "unofficial_napi_request_interrupt" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_request_interrupt),
        "unofficial_napi_structured_clone" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_structured_clone),
        "unofficial_napi_message_create" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_message_create),
        "unofficial_napi_message_take" => message_take_import(store, fe),
        "unofficial_napi_message_drop" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_message_drop),
        "unofficial_napi_enqueue_microtask" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_enqueue_microtask),
        "unofficial_napi_set_promise_reject_callback" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_set_promise_reject_callback),
        "unofficial_napi_get_own_non_index_properties" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_get_own_non_index_properties),
        "unofficial_napi_set_promise_hooks" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_set_promise_hooks),
        "unofficial_napi_get_hash_seed" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_get_hash_seed),
        "unofficial_napi_get_error_metadata" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_get_error_metadata),
        "unofficial_napi_configure_source_maps" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_configure_source_maps),
        "unofficial_napi_preserve_error_source_message" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_preserve_error_source_message),
        "unofficial_napi_mark_promise_as_handled" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_mark_promise_as_handled),
        "unofficial_napi_get_heap_statistics" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_get_heap_statistics),
        "unofficial_napi_get_heap_space_statistics" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_get_heap_space_statistics),
        "unofficial_napi_get_heap_code_statistics" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_get_heap_code_statistics),
        "unofficial_napi_configure_near_heap_limit_callback" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_configure_near_heap_limit_callback),
        "unofficial_napi_profile_start" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_profile_start),
        "unofficial_napi_profile_stop" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_profile_stop),
        "unofficial_napi_take_heap_snapshot" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_take_heap_snapshot),
        "unofficial_napi_create_serdes_binding" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_create_serdes_binding),
        "unofficial_napi_contextify_contains_module_syntax" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_contextify_contains_module_syntax),
        "unofficial_napi_create_realm" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_create_realm),
        "unofficial_napi_get_realm" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_get_realm),
        "unofficial_napi_enter_realm" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_enter_realm),
        "unofficial_napi_leave_realm" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_leave_realm),
        "unofficial_napi_run_script" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_run_script),
        "unofficial_napi_compile_function" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_compile_function),
        "unofficial_napi_bytecode_open" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_bytecode_open),
        "unofficial_napi_bytecode_serialize" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_bytecode_serialize),
        "unofficial_napi_bytecode_release" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_bytecode_release),
        "unofficial_napi_module_wrap_create" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_create),
        "unofficial_napi_module_wrap_destroy" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_destroy),
        "unofficial_napi_module_wrap_link" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_link),
        "unofficial_napi_module_wrap_instantiate" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_instantiate),
        "unofficial_napi_module_wrap_evaluate" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_evaluate),
        "unofficial_napi_module_wrap_evaluate_sync" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_evaluate_sync),
        "unofficial_napi_module_wrap_get_namespace" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_get_namespace),
        "unofficial_napi_module_wrap_get_state" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_get_state),
        "unofficial_napi_module_wrap_check_unsettled_top_level_await" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_check_unsettled_top_level_await),
        "unofficial_napi_module_wrap_set_export" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_set_export),
        "unofficial_napi_module_wrap_set_module_source_object" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_set_module_source_object),
        "unofficial_napi_module_wrap_get_module_source_object" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_get_module_source_object),
        "unofficial_napi_module_wrap_create_cached_data" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_create_cached_data),
        "unofficial_napi_module_wrap_set_hooks" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_set_hooks),
        "unofficial_napi_module_wrap_create_required_module_facade" => Function::new_typed_with_env(store, fe, guest_unofficial_napi_module_wrap_create_required_module_facade),
    };

    io.register_namespace(NAPI_MODULE_NAME, napi_namespace);
    io.register_namespace(
        NAPI_EXTENSION_WASMER_MODULE_NAME,
        napi_extension_wasmer_namespace,
    );
}

fn guest_env_uv_cpu_info(_cpu_infos_out: i32, _count_out: i32) -> i32 {
    -1
}

fn guest_env_uv_interface_addresses(_addresses_out: i32, _count_out: i32) -> i32 {
    -1
}

fn guest_env_uv_free_interface_addresses(_addresses: i32, _count: i32) {}

fn guest_env_uv_resident_set_memory(_rss_out: i32) -> i32 {
    -1
}

fn guest_env_uv_get_free_memory() -> i64 {
    0
}

fn guest_env_uv_get_total_memory() -> i64 {
    0
}

fn guest_env_ossl_set_max_threads(_ctx: i32, _max_threads: i64) -> i32 {
    // The Wasm-hosted runtime executes on a single host thread, so there is no
    // native OpenSSL worker-pool sizing to apply here.
    1
}

pub fn register_env_imports(store: &mut impl AsStoreMut, io: &mut Imports) {
    macro_rules! reg_env {
        ($name:expr, $func:expr) => {
            io.define("env", $name, Function::new_typed(store, $func));
        };
    }

    reg_env!("uv_cpu_info", guest_env_uv_cpu_info);
    reg_env!("uv_interface_addresses", guest_env_uv_interface_addresses);
    reg_env!(
        "uv_free_interface_addresses",
        guest_env_uv_free_interface_addresses
    );
    reg_env!("uv_resident_set_memory", guest_env_uv_resident_set_memory);
    reg_env!("uv_get_free_memory", guest_env_uv_get_free_memory);
    reg_env!("uv_get_total_memory", guest_env_uv_get_total_memory);
    reg_env!(
        "_Z20OSSL_set_max_threadsP15ossl_lib_ctx_sty",
        guest_env_ossl_set_max_threads
    );
}
