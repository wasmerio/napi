use std::ffi::c_void;

use wasmer::{FunctionEnvMut, Table, Value};

use crate::{NapiEnv, snapi::SnapiEnv};

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

/// Like [`call_guest_callback`] but passes two data arguments in addition to
/// the guest env handle. Used to dispatch N-API finalizers into the guest,
/// whose ABI is `void finalize(napi_env env, void* data, void* hint)`.
fn call_guest_callback2(
    env: &mut FunctionEnvMut<NapiEnv>,
    table: &Table,
    guest_env: i32,
    wasm_fn_ptr: u32,
    arg0: u32,
    arg1: u32,
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
        &[
            Value::I32(guest_env),
            Value::I32(arg0 as i32),
            Value::I32(arg1 as i32),
        ],
    ) {
        Ok(ret_vals) => match ret_vals.first() {
            Some(Value::I32(v)) => *v as u32,
            Some(Value::I64(v)) => *v as u32,
            _ => 0,
        },
        Err(err) => {
            eprintln!("[finalizer trampoline] error calling guest finalizer: {err}");
            0
        }
    }
}

/// Install a callback-invocation context for the duration of `f`, so host V8
/// callbacks fired inside the bridge call can be dispatched into the wasm
/// guest (see [`snapi_host_invoke_wasm_callback`]).
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
    let prev = unsafe {
        crate::snapi::snapi_bridge_swap_active_callback_ctx(
            snapi_env,
            (&mut ctx as *mut CallbackInvocationCtx).cast::<c_void>(),
        )
    };
    struct CallbackStateGuard {
        snapi_env: SnapiEnv,
        prev: *mut c_void,
    }
    impl Drop for CallbackStateGuard {
        fn drop(&mut self) {
            unsafe {
                crate::snapi::snapi_bridge_swap_active_callback_ctx(self.snapi_env, self.prev);
            }
        }
    }
    let _guard = CallbackStateGuard { snapi_env, prev };
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
    let result = call_guest_callback(env, &table, guest_env as i32, wasm_fn_ptr, callback_arg);
    env.data_mut().leave_callback();
    result
}

/// Rust trampoline called from C++ when a guest-registered N-API finalizer
/// fires (during the deferred finalizer drain, which runs inside a guest→bridge
/// import call, so an active callback scope is available). Re-enters the guest
/// and dispatches `finalize(env, data, hint)` through the indirect table.
#[unsafe(no_mangle)]
pub extern "C" fn snapi_host_invoke_wasm_finalizer(
    callback_ctx: *mut c_void,
    guest_env: u32,
    wasm_fn_ptr: u32,
    data: u32,
    hint: u32,
) -> u32 {
    if callback_ctx.is_null() {
        // No active guest call to re-enter (e.g. fired during env teardown,
        // when the guest instance is going away regardless). Nothing to do.
        return 0;
    }
    let ctx = unsafe { &mut *(callback_ctx as *mut CallbackInvocationCtx) };
    if ctx.env.is_null() {
        return 0;
    }
    let env = unsafe { &mut *ctx.env.cast::<FunctionEnvMut<'_, NapiEnv>>() };
    let Some(table) = env.data().table.clone() else {
        return 0;
    };
    // Same reentrancy bound as the regular callback trampoline: a finalizer
    // may itself create/free values and trigger more callbacks.
    if !env.data_mut().enter_callback() {
        eprintln!("[finalizer trampoline] reentrancy depth limit exceeded");
        return 0;
    }
    let result = call_guest_callback2(env, &table, guest_env as i32, wasm_fn_ptr, data, hint);
    env.data_mut().leave_callback();
    result
}
