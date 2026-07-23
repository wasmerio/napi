use std::ffi::c_void;

use wasmer::{FunctionEnvMut, Table, Value};

use crate::{NapiEnv, snapi::SnapiEnv};

use super::util::read_guest_bytes;

type RawFunctionEnvMut = FunctionEnvMut<'static, NapiEnv>;

#[repr(C)]
struct CallbackInvocationCtx {
    env: *mut RawFunctionEnvMut,
}

fn call_guest_callback(
    env: &mut FunctionEnvMut<NapiEnv>,
    table: &Table,
    guest_env: i32,
    wasm_fn_ptr: u32,
    callback_arg: u32,
) -> u32 {
    let Some(elem) = table.get(env, wasm_fn_ptr) else {
        return 0;
    };
    let func = match elem {
        Value::FuncRef(Some(func)) => func,
        Value::FuncRef(None) => return 0,
        _ => return 0,
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

fn flush_host_buffer_copies(
    env: &mut FunctionEnvMut<NapiEnv>,
    snapi_env: SnapiEnv,
    frame_start: usize,
) {
    flush_host_buffer_copies_since(env, snapi_env, frame_start);
    env.data_mut().host_buffer_copy_frames.pop();
}

/// Mid-execution coherency flush: write back guest-modified copies so the host
/// sees current bytes, but KEEP the entries (re-based on the flushed bytes).
/// Frames still on the stack hold live pointers into these copies and may
/// mutate them after this flush; dropping the entries here would orphan those
/// later mutations so they never reach the host.
pub fn flush_pending_host_buffer_copies(env: &mut FunctionEnvMut<NapiEnv>, snapi_env: SnapiEnv) {
    if snapi_env.is_null() || env.data().host_buffer_copies.is_empty() {
        return;
    }

    for idx in 0..env.data().host_buffer_copies.len() {
        let (handle_id, guest_ptr, byte_len) = {
            let c = &env.data().host_buffer_copies[idx];
            (c.handle_id, c.guest_ptr, c.byte_len)
        };
        if byte_len == 0 {
            continue;
        }
        let Some(bytes) = read_guest_bytes(env, guest_ptr as i32, byte_len) else {
            continue;
        };
        // Only write back copies the guest actually modified: several frames
        // can hold copies of the same host buffer, and flushing an untouched
        // (possibly stale) copy would clobber newer host-side writes.
        if bytes == env.data().host_buffer_copies[idx].pristine {
            continue;
        }
        unsafe {
            crate::snapi::snapi_bridge_overwrite_value_bytes(
                snapi_env,
                handle_id,
                bytes.as_ptr().cast(),
                byte_len as u32,
            );
        }
        env.data_mut().host_buffer_copies[idx].pristine = bytes;
    }
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

    for mapping in drained {
        if mapping.byte_len > 0
            && let Some(bytes) = read_guest_bytes(env, mapping.guest_ptr as i32, mapping.byte_len)
            // Only write back copies the guest actually modified: several
            // frames can hold copies of the same host buffer, and flushing an
            // untouched (possibly stale) copy would clobber newer host-side
            // writes with old bytes.
            && bytes != mapping.pristine
        {
            unsafe {
                crate::snapi::snapi_bridge_overwrite_value_bytes(
                    snapi_env,
                    mapping.handle_id,
                    bytes.as_ptr().cast(),
                    mapping.byte_len as u32,
                );
            }
        }

        let state = env.data_mut();
        state.guest_data_ptrs.remove(&mapping.handle_id);
        if mapping.backing_store_token != 0 {
            state
                .guest_data_backing_stores
                .remove(&mapping.backing_store_token);
        }
    }
}

pub fn with_callback_state<R>(
    env: &mut FunctionEnvMut<NapiEnv>,
    snapi_env: SnapiEnv,
    f: impl FnOnce() -> R,
) -> R {
    if snapi_env.is_null() {
        return f();
    }

    let mut ctx = CallbackInvocationCtx {
        env: (env as *mut FunctionEnvMut<'_, NapiEnv>).cast::<RawFunctionEnvMut>(),
    };
    let frame_start = env.data().host_buffer_copies.len();
    let method_frame_depth = env.data().host_buffer_method_frames.len();
    env.data_mut().host_buffer_copy_frames.push(frame_start);
    let prev = unsafe {
        crate::snapi::snapi_bridge_swap_active_callback_ctx(
            snapi_env,
            (&mut ctx as *mut CallbackInvocationCtx).cast::<c_void>(),
        )
    };
    struct CallbackStateGuard {
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
    let _guard = CallbackStateGuard {
        snapi_env,
        prev,
        env: ctx.env,
        frame_start,
        method_frame_depth,
    };
    f()
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
    if ctx.env.is_null() {
        eprintln!("[callback trampoline] callback scope env cleared");
        return 0;
    }
    let env = unsafe { &mut *ctx.env.cast::<FunctionEnvMut<'_, NapiEnv>>() };
    let Some(table) = env.data().table.clone() else {
        return 0;
    };

    // Bound guest↔host callback reentrancy so a runaway recursion cannot
    // overflow the host native stack (an uncatchable SIGSEGV that would take
    // down every co-tenant). Refuse the callback past the limit rather than
    // crash. The increment below always pairs with the decrement, since
    // `call_guest_callback` returns normally on both success and trap.
    if !env.data_mut().enter_callback() {
        eprintln!("[callback trampoline] reentrancy depth limit exceeded");
        return 0;
    }
    let copies_start = env.data().host_buffer_copies.len();
    let result = call_guest_callback(env, &table, guest_env as i32, wasm_fn_ptr, callback_arg);
    // Write back host-buffer copies created during this callback before the
    // host pops the callback's handle scope. Without this, guest mutations of
    // host-allocated buffers (e.g. state structs the guest updates in place)
    // only flush when the outermost import call unwinds — far too late for JS
    // code that reads the buffer right after the callback returns.
    let snapi_env = env.data().resolve_napi_env(guest_env as i32);
    if !snapi_env.is_null() {
        flush_host_buffer_copies_since(env, snapi_env, copies_start);
    }
    env.data_mut().leave_callback();
    result
}
