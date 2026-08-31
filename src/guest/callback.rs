use std::{cell::Cell, ffi::c_void};

use wasmer::{FunctionEnvMut, Table, Value};
use wasmer_wasix::WasiError;
use wasmer_wasix::wasmer_wasix_types::wasi::ExitCode;

use crate::{NapiEnv, snapi::SnapiEnv};

type RawFunctionEnvMut = FunctionEnvMut<'static, NapiEnv>;

thread_local! {
    /// A guest instance exit (`WasiError::Exit`) that trapped out of a guest
    /// callback and must be re-raised through the N-API import layers.
    ///
    /// When the guest calls `_exit`/`proc_exit` — e.g. Node's fatal-exception
    /// path taking `std::_Exit` on the main thread after an unhandled exception
    /// — WASI `proc_exit` unwinds the guest and the [`func.call`] that dispatched
    /// the callback returns a trap carrying [`WasiError::Exit`]. The trampoline
    /// runs inside host V8 (a native C++ frame that cannot carry a Rust trap), so
    /// it cannot re-raise directly. Instead it stashes the exit here; the nearest
    /// enclosing N-API import boundary ([`with_cb_context`]) takes it back out and
    /// returns it as an `Err`, so wasmer re-raises the same exit through every
    /// guest→host layer until the runner (`bin_factory`/`thread_spawn`) handles
    /// it — the standard WASIX process-exit propagation. Swallowing it (the old
    /// behavior) left the host event loop spinning forever (observed as
    /// `test-tls-handshake-exception` hanging).
    ///
    /// This terminates only the guest instance whose callback exited, never the
    /// host process.
    ///
    /// [`func.call`]: wasmer::Function::call
    /// [`with_cb_context`]: crate::guest::napi::with_cb_context
    static PENDING_GUEST_EXIT: std::cell::Cell<Option<ExitCode>> =
        const { std::cell::Cell::new(None) };

    /// The invocation currently crossing the native C++ bridge on this thread.
    ///
    /// V8's backing-store allocator has only its opaque heap context, so this
    /// narrow thread-local token lets a synchronous foreground allocation use
    /// the store already held by the enclosing import. Background allocator
    /// threads have their own empty slot and therefore remain arena-only.
    static ACTIVE_INVOCATION: Cell<*mut CallbackInvocationCtx> =
        const { Cell::new(std::ptr::null_mut()) };
}

/// If a guest callback trapped with a WASI process exit, stash the exit code so
/// the enclosing import boundary can re-raise it, and report whether it did.
/// Non-exit traps return `false` and are left for the caller to log.
fn stash_guest_exit(err: &wasmer::RuntimeError) -> bool {
    if let Some(WasiError::Exit(code)) = err.downcast_ref::<WasiError>() {
        PENDING_GUEST_EXIT.with(|slot| slot.set(Some(*code)));
        return true;
    }
    false
}

/// Take a pending guest instance exit stashed by a callback trampoline, if any.
/// Called at each N-API import boundary to re-raise the exit outward.
pub(crate) fn take_pending_guest_exit() -> Option<ExitCode> {
    PENDING_GUEST_EXIT.with(|slot| slot.take())
}

/// The synchronous guest invocation active while Rust is inside the native C++
/// bridge. C++ only carries this as an opaque pointer and may use it before the
/// enclosing bridge call returns.
///
/// The pointer is deliberately scoped instead of publishing the whole Wasmer
/// store through generic runtime TLS. It is valid only on `owner_thread`; V8
/// callbacks execute synchronously there, while allocator calls on background
/// threads see an empty [`ACTIVE_INVOCATION`] slot.
struct CallbackInvocationCtx {
    env: *mut RawFunctionEnvMut,
    owner_thread: std::thread::ThreadId,
}

struct ActiveInvocationGuard {
    prev: *mut CallbackInvocationCtx,
}

impl Drop for ActiveInvocationGuard {
    fn drop(&mut self) {
        ACTIVE_INVOCATION.with(|slot| slot.set(self.prev));
    }
}

fn replace_active_invocation(next: *mut CallbackInvocationCtx) -> ActiveInvocationGuard {
    let prev = ACTIVE_INVOCATION.with(|slot| slot.replace(next));
    ActiveInvocationGuard { prev }
}

fn is_active_invocation(ctx: *mut CallbackInvocationCtx) -> bool {
    ACTIVE_INVOCATION.with(|slot| slot.get() == ctx)
}

struct BridgeCallbackStateGuard {
    snapi_env: SnapiEnv,
    prev: *mut c_void,
}

impl Drop for BridgeCallbackStateGuard {
    fn drop(&mut self) {
        unsafe {
            crate::snapi::snapi_bridge_swap_active_callback_ctx(self.snapi_env, self.prev);
        }
    }
}

fn replace_bridge_callback_state(
    snapi_env: SnapiEnv,
    next: *mut c_void,
) -> Option<BridgeCallbackStateGuard> {
    if snapi_env.is_null() {
        return None;
    }
    let prev = unsafe { crate::snapi::snapi_bridge_swap_active_callback_ctx(snapi_env, next) };
    Some(BridgeCallbackStateGuard { snapi_env, prev })
}

/// Suspend the outer Rust invocation token while its callback is executing
/// guest code. Nested imports install their own token. An import that was not
/// classified as reentrant therefore sees no stale outer env and safely falls
/// back to the prefunded heap arena instead of aliasing the store currently in
/// `func.call`.
///
/// The C++ callback pointer may remain installed: each Rust trampoline verifies
/// that it matches this TLS token before dereferencing it. Avoiding another
/// pair of atomic C++ swaps keeps actual callback dispatch cheap.
fn without_outer_invocation<R>(f: impl FnOnce() -> R) -> R {
    let _active_guard = replace_active_invocation(std::ptr::null_mut());
    f()
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
            let Some(elem) = table.get(&mut *env, wasm_fn_ptr) else {
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
        &mut *env,
        &[Value::I32(guest_env), Value::I32(callback_arg as i32)],
    ) {
        Ok(ret_vals) => match ret_vals.first() {
            Some(Value::I32(v)) => *v as u32,
            Some(Value::I64(v)) => *v as u32,
            _ => 0,
        },
        Err(err) => {
            if stash_guest_exit(&err) {
                return 0;
            }
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
    let func = match env.data().func_cache.get(&wasm_fn_ptr).cloned() {
        Some(func) => func,
        None => {
            let Some(elem) = table.get(&mut *env, wasm_fn_ptr) else {
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
        &mut *env,
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
            if stash_guest_exit(&err) {
                return 0;
            }
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
    let mut ctx = CallbackInvocationCtx {
        env: (env as *mut FunctionEnvMut<'_, NapiEnv>).cast::<RawFunctionEnvMut>(),
        owner_thread: std::thread::current().id(),
    };
    let ctx_ptr = &mut ctx as *mut CallbackInvocationCtx;

    let _active_guard = replace_active_invocation(ctx_ptr);
    let _bridge_guard = replace_bridge_callback_state(snapi_env, ctx_ptr.cast::<c_void>());

    // Neither this frame nor the guards touch `env` until `f` returns. A
    // synchronous bridge callback or allocator call may temporarily reborrow
    // it through `ctx`; nested guest imports install and restore their own ctx.
    f()
}

/// Give a synchronous foreground allocator call access to the active guest
/// environment. Background V8 threads have distinct TLS and receive `None`.
///
/// This has the same scoped-reborrow invariant as the callback trampoline:
/// [`with_callback_state`] owns `&mut FunctionEnvMut`, does not use it while the
/// bridge is running, and restores this slot before that borrow can be used
/// again.
pub(crate) fn with_active_callback_env<R>(
    f: impl FnOnce(Option<&mut FunctionEnvMut<'_, NapiEnv>>) -> R,
) -> R {
    ACTIVE_INVOCATION.with(|slot| {
        let ctx_ptr = slot.get();
        if ctx_ptr.is_null() {
            return f(None);
        }
        // SAFETY: this thread-local pointer is installed by
        // `with_callback_state` and restored before its stack context drops.
        let ctx = unsafe { &mut *ctx_ptr };
        if ctx.owner_thread != std::thread::current().id() || ctx.env.is_null() {
            return f(None);
        }
        // SAFETY: the installing frame suspends all access to its env for the
        // bridge call. Nested imports replace this token while they execute.
        let env = unsafe { &mut *ctx.env.cast::<FunctionEnvMut<'_, NapiEnv>>() };
        f(Some(env))
    })
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
    let ctx_ptr = callback_ctx.cast::<CallbackInvocationCtx>();
    if !is_active_invocation(ctx_ptr) {
        eprintln!("[callback trampoline] no active callback scope available");
        return 0;
    }
    // SAFETY: `callback_ctx` is the context `with_callback_state` installed,
    // which outlives this synchronous bridge callback.
    let ctx = unsafe { &mut *ctx_ptr };
    if ctx.owner_thread != std::thread::current().id() {
        eprintln!("[callback trampoline] callback invoked on the wrong thread");
        return 0;
    }
    if ctx.env.is_null() {
        eprintln!("[callback trampoline] callback scope env cleared");
        return 0;
    }
    // SAFETY: the installing frame has suspended use of this env until the
    // bridge call returns.
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
    let result = without_outer_invocation(|| {
        call_guest_callback(env, &table, guest_env as i32, wasm_fn_ptr, callback_arg)
    });
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
    let ctx_ptr = callback_ctx.cast::<CallbackInvocationCtx>();
    if !is_active_invocation(ctx_ptr) {
        return 0;
    }
    // SAFETY: as in `snapi_host_invoke_wasm_callback`.
    let ctx = unsafe { &mut *ctx_ptr };
    if ctx.owner_thread != std::thread::current().id() || ctx.env.is_null() {
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
    let result = without_outer_invocation(|| {
        call_guest_callback2(env, &table, guest_env as i32, wasm_fn_ptr, data, hint)
    });
    env.data_mut().leave_callback();
    result
}
