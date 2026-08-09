use std::ffi::c_void;

use wasmer::{FunctionEnvMut, Table, Value};

#[cfg(all(target_arch = "wasm32", feature = "js"))]
use std::future::Future;
#[cfg(all(target_arch = "wasm32", feature = "js"))]
use wasmer::AsyncFunctionEnvMut;

use crate::{NapiEnv, snapi::SnapiEnv};

use super::util::{forget_guest_allocation, read_guest_bytes, recycle_guest_allocation};

type RawFunctionEnvMut = FunctionEnvMut<'static, NapiEnv>;

/// A value-handle ID reserved for callbacks which arrived while the guest was
/// suspended through JSPI. The host bridge keeps their callback frame alive
/// and retries them from the next synchronous guest checkpoint.
pub(crate) const CALLBACK_DEFERRED: u32 = u32::MAX;

#[repr(C)]
pub(crate) struct CallbackInvocationCtx {
    env: CallbackEnv,
}

// The JS backend is single-threaded; this marker only satisfies the generic
// FunctionEnv bound and the context is never transferred between workers.
#[cfg(all(target_arch = "wasm32", feature = "js"))]
unsafe impl Send for CallbackInvocationCtx {}

enum CallbackEnv {
    Sync(*mut RawFunctionEnvMut),
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    Async(AsyncFunctionEnvMut<NapiEnv>),
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
pub(crate) fn install_persistent_callback_state(
    env: &mut FunctionEnvMut<NapiEnv>,
    env_id: u32,
    snapi_env: SnapiEnv,
    async_env: AsyncFunctionEnvMut<NapiEnv>,
) {
    let mut ctx = Box::new(CallbackInvocationCtx {
        env: CallbackEnv::Async(async_env),
    });
    let ctx_ptr = (&mut *ctx as *mut CallbackInvocationCtx).cast::<c_void>();
    env.data_mut()
        .persistent_callback_contexts
        .insert(env_id, ctx);
    unsafe {
        crate::snapi::snapi_bridge_swap_active_callback_ctx(snapi_env, ctx_ptr);
    }
}

struct CallbackStateGuard {
    _ctx: Box<CallbackInvocationCtx>,
    snapi_env: SnapiEnv,
    prev: *mut c_void,
    env: *mut RawFunctionEnvMut,
    frame_start: usize,
    method_frame_depth: usize,
}

impl Drop for CallbackStateGuard {
    fn drop(&mut self) {
        if !self.env.is_null() {
            let env = unsafe { &mut *self.env.cast::<FunctionEnvMut<'_, NapiEnv>>() };
            flush_host_buffer_copies(env, self.snapi_env, self.frame_start);
            env.data_mut()
                .host_buffer_method_frames
                .truncate(self.method_frame_depth);
            if self.frame_start > 0 {
                flush_pending_host_buffer_copies(env, self.snapi_env);
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

    let env_ptr = (env as *mut FunctionEnvMut<'_, NapiEnv>).cast::<RawFunctionEnvMut>();
    let mut ctx = Box::new(CallbackInvocationCtx {
        env: CallbackEnv::Sync(env_ptr),
    });
    let frame_start = env.data().host_buffer_copies.len();
    let method_frame_depth = env.data().host_buffer_method_frames.len();
    env.data_mut().host_buffer_copy_frames.push(frame_start);
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
        frame_start,
        method_frame_depth,
    })
}

fn call_guest_callback(
    env: &mut FunctionEnvMut<NapiEnv>,
    table: &Table,
    guest_env: i32,
    wasm_fn_ptr: u32,
    callback_arg: u32,
) -> u32 {
    let func = match env.data().func_cache.get(&wasm_fn_ptr).cloned() {
        Some(func) => func,
        None => {
            let Some(elem) = table.get(env, wasm_fn_ptr) else {
                return 0;
            };
            let func = match elem {
                Value::FuncRef(Some(func)) => func,
                Value::FuncRef(None) => return 0,
                _ => return 0,
            };
            env.data_mut().func_cache.insert(wasm_fn_ptr, func.clone());
            func
        }
    };
    match func.call(
        env,
        &[Value::I32(guest_env), Value::I32(callback_arg as i32)],
    ) {
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

fn call_guest_callback_and_flush(
    env: &mut FunctionEnvMut<NapiEnv>,
    table: &Table,
    guest_env: i32,
    wasm_fn_ptr: u32,
    callback_arg: u32,
) -> u32 {
    let frame_start = env.data().host_buffer_copies.len();
    let method_frame_depth = env.data().host_buffer_method_frames.len();
    let result = call_guest_callback(env, table, guest_env, wasm_fn_ptr, callback_arg);
    let snapi_env = env.data().resolve_napi_env(guest_env);
    let flush_start = env.data().host_buffer_method_frames[method_frame_depth..]
        .iter()
        .copied()
        .min()
        .unwrap_or(frame_start)
        .min(frame_start);
    flush_host_buffer_copies_since(env, snapi_env, flush_start);
    env.data_mut()
        .host_buffer_method_frames
        .truncate(method_frame_depth);
    result
}

fn flush_host_buffer_copies(
    env: &mut FunctionEnvMut<NapiEnv>,
    snapi_env: SnapiEnv,
    frame_start: usize,
) {
    flush_host_buffer_copies_since(env, snapi_env, frame_start);
    env.data_mut().host_buffer_copy_frames.pop();
}

pub fn flush_pending_host_buffer_copies(env: &mut FunctionEnvMut<NapiEnv>, snapi_env: SnapiEnv) {
    if snapi_env.is_null() || env.data().host_buffer_copies.is_empty() {
        return;
    }

    let drained = {
        let state = env.data_mut();
        state
            .host_buffer_copy_frames
            .iter_mut()
            .for_each(|start| *start = 0);
        std::mem::take(&mut state.host_buffer_copies)
    };

    let mut held = Vec::new();
    for mapping in drained {
        let mapping_snapi = env.data().resolve_napi_env(mapping.guest_env as i32);
        if mapping.byte_len > 0
            && !mapping_snapi.is_null()
            && let Some(bytes) = read_guest_bytes(env, mapping.guest_ptr as i32, mapping.byte_len)
        {
            unsafe {
                crate::snapi::snapi_bridge_overwrite_value_bytes(
                    mapping_snapi,
                    mapping.handle_id,
                    bytes.as_ptr().cast(),
                    mapping.byte_len as u32,
                );
            }
        }
        if mapping.reference_holds > 0 {
            held.push(mapping);
            continue;
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

pub fn flush_host_buffer_copies_since(
    env: &mut FunctionEnvMut<NapiEnv>,
    snapi_env: SnapiEnv,
    frame_start: usize,
) {
    let start = frame_start.min(env.data().host_buffer_copies.len());
    let drained = {
        let state = env.data_mut();
        state.host_buffer_copies.split_off(start)
    };

    let mut held = Vec::new();
    for mapping in drained {
        let mapping_snapi = env.data().resolve_napi_env(mapping.guest_env as i32);
        if mapping.byte_len > 0
            && !mapping_snapi.is_null()
            && let Some(bytes) = read_guest_bytes(env, mapping.guest_ptr as i32, mapping.byte_len)
        {
            unsafe {
                crate::snapi::snapi_bridge_overwrite_value_bytes(
                    mapping_snapi,
                    mapping.handle_id,
                    bytes.as_ptr().cast(),
                    mapping.byte_len as u32,
                );
            }
        }
        if mapping.reference_holds > 0 {
            held.push(mapping);
            continue;
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

pub fn with_callback_state<R>(
    env: &mut FunctionEnvMut<NapiEnv>,
    snapi_env: SnapiEnv,
    f: impl FnOnce() -> R,
) -> R {
    let guard = install_callback_state(env, snapi_env);
    let result = f();
    drop(guard);
    result
}

#[cfg(all(target_arch = "wasm32", feature = "js"))]
pub async fn with_callback_state_async<R, F>(
    mut env: AsyncFunctionEnvMut<NapiEnv>,
    snapi_env: SnapiEnv,
    future: F,
) -> R
where
    F: Future<Output = R>,
{
    if snapi_env.is_null() {
        return future.await;
    }

    let (frame_start, method_frame_depth) = {
        let mut locked = env.write().await;
        let frame_start = locked.data_mut().host_buffer_copies.len();
        let method_frame_depth = locked.data_mut().host_buffer_method_frames.len();
        locked.data_mut().host_buffer_copy_frames.push(frame_start);
        (frame_start, method_frame_depth)
    };
    let mut ctx = Box::new(CallbackInvocationCtx {
        env: CallbackEnv::Async(env.as_mut()),
    });
    let prev = unsafe {
        crate::snapi::snapi_bridge_swap_active_callback_ctx(
            snapi_env,
            (&mut *ctx as *mut CallbackInvocationCtx).cast::<c_void>(),
        )
    };
    let result = future.await;
    unsafe {
        crate::snapi::snapi_bridge_swap_active_callback_ctx(snapi_env, prev);
    }
    {
        // Keep this guard in an explicit scope. A completed Rust future may
        // remain owned by its JavaScript Promise until a later GC cycle; if the
        // guard stays in the generator state, it also keeps the async Store
        // shared after the guest entrypoint has returned.
        let mut locked = env.write().await;
        let mut sync_env = locked.as_function_env_mut();
        flush_host_buffer_copies(&mut sync_env, snapi_env, frame_start);
        sync_env
            .data_mut()
            .host_buffer_method_frames
            .truncate(method_frame_depth);
        if frame_start > 0 {
            flush_pending_host_buffer_copies(&mut sync_env, snapi_env);
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
    let ctx = unsafe { &mut *(callback_ctx as *mut CallbackInvocationCtx) };
    match &mut ctx.env {
        CallbackEnv::Sync(env) => {
            if env.is_null() {
                eprintln!("[callback trampoline] callback scope env cleared");
                return 0;
            }
            let env = unsafe { &mut *env.cast::<FunctionEnvMut<'_, NapiEnv>>() };
            let Some(table) = env.data().table.clone() else {
                return 0;
            };
            call_guest_callback_and_flush(env, &table, guest_env as i32, wasm_fn_ptr, callback_arg)
        }
        #[cfg(all(target_arch = "wasm32", feature = "js"))]
        CallbackEnv::Async(env) => {
            if let Some(result) = env.with_current_mut(|mut sync_env| {
                let Some(table) = sync_env.data().table.clone() else {
                    return 0;
                };
                call_guest_callback_and_flush(
                    &mut sync_env,
                    &table,
                    guest_env as i32,
                    wasm_fn_ptr,
                    callback_arg,
                )
            }) {
                return result;
            }
            if let Some(mut locked) = env.try_write() {
                let mut sync_env = locked.as_function_env_mut();
                if let Some(table) = sync_env.data().table.clone() {
                    return call_guest_callback_and_flush(
                        &mut sync_env,
                        &table,
                        guest_env as i32,
                        wasm_fn_ptr,
                        callback_arg,
                    );
                }
            }
            CALLBACK_DEFERRED
        }
    }
}
