use std::{ffi::c_void, sync::Arc};

use wasmer::{AsStoreMut, FunctionEnv, FunctionEnvMut, Store, StoreMut, Table, Value};
use wasmer_wasix::WasiError;
use wasmer_wasix::wasmer_wasix_types::wasi::ExitCode;

use super::callback_cache::CachedGuestCallback;
use crate::{NapiEnv, snapi::SnapiEnv};

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

/// What a V8 callback needs to reach the guest: a handle to the guest
/// instance's N-API env and the id of its store. Neither changes for the
/// life of a bridge env, so [`install_callback_ctx`] gives each env one when
/// it is created and imports never swap it. The handle is store-free; the
/// store itself is picked back up from the thread, which an import lends by
/// parking its own borrow (see [`with_callback_state`]). C++ only ever passes
/// this back as an opaque pointer.
pub(crate) struct CallbackInvocationCtx {
    func_env: FunctionEnv<NapiEnv>,
    /// The store `func_env` lives in. A callback only runs with that store
    /// lent: no store may be lent (JavaScript outside an import that lends
    /// it), or another store's, when stores share the thread.
    store_id: usize,
}

impl CallbackInvocationCtx {
    /// The lent store, if it is the one this context's env lives in.
    fn with_store<R>(&self, f: impl FnOnce(&mut StoreMut<'_>) -> R) -> Option<R> {
        Store::with_current(|store| {
            (store.objects_mut().id().as_raw().get() == self.store_id).then(|| f(store))
        })
        .flatten()
    }
}

/// Gives the env `env_id` just created its callback context. The env's agent
/// keeps a pointer to it until its release, and `NapiEnv` frees it after that.
/// The context is shared, never borrowed mutably, so the agent's pointer stays
/// valid however `NapiEnv` moves its handle around.
pub(crate) fn install_callback_ctx(
    env: &mut FunctionEnvMut<NapiEnv>,
    env_id: u32,
    snapi_env: SnapiEnv,
) {
    let ctx = Arc::new(CallbackInvocationCtx {
        func_env: env.as_ref(),
        store_id: env.objects_mut().id().as_raw().get(),
    });
    let ctx_ptr = Arc::as_ptr(&ctx).cast_mut().cast::<c_void>();
    env.data_mut().native_callback_contexts.insert(env_id, ctx);
    unsafe {
        crate::snapi::snapi_bridge_swap_active_callback_ctx(snapi_env, ctx_ptr);
    }
}

fn call_guest_callback(
    store: &mut StoreMut<'_>,
    func_env: &FunctionEnv<NapiEnv>,
    table: &Table,
    guest_env: u32,
    wasm_fn_ptr: u32,
    callback_arg: u32,
) -> u32 {
    let func = match func_env.as_ref(store).func_cache.get(&wasm_fn_ptr).cloned() {
        Some(func) => func,
        None => {
            let Some(elem) = table.get(store, wasm_fn_ptr) else {
                return 0;
            };
            let func = match elem {
                Value::FuncRef(Some(func)) => func,
                Value::FuncRef(None) => return 0,
                _ => return 0,
            };
            let callback = CachedGuestCallback::new(func, store);
            func_env
                .as_mut(store)
                .func_cache
                .insert(wasm_fn_ptr, callback.clone());
            callback
        }
    };
    match func.call(store, guest_env, callback_arg) {
        Ok(result) => result,
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
    store: &mut StoreMut<'_>,
    func_env: &FunctionEnv<NapiEnv>,
    table: &Table,
    guest_env: u32,
    wasm_fn_ptr: u32,
    arg0: u32,
    arg1: u32,
) -> u32 {
    let func = match func_env.as_ref(store).func_cache.get(&wasm_fn_ptr).cloned() {
        Some(func) => func,
        None => {
            let Some(elem) = table.get(store, wasm_fn_ptr) else {
                return 0;
            };
            let func = match elem {
                Value::FuncRef(Some(func)) => func,
                Value::FuncRef(None) => return 0,
                _ => return 0,
            };
            let callback = CachedGuestCallback::new(func, store);
            func_env
                .as_mut(store)
                .func_cache
                .insert(wasm_fn_ptr, callback.clone());
            callback
        }
    };
    match func.call_finalizer(store, guest_env, arg0, arg1) {
        Ok(result) => result,
        Err(err) => {
            if stash_guest_exit(&err) {
                return 0;
            }
            eprintln!("[finalizer trampoline] error calling guest finalizer: {err}");
            0
        }
    }
}

/// Lend the store for the duration of `f`, so host V8 callbacks fired inside
/// the bridge call can be dispatched into the wasm guest through the env's
/// callback context (see [`snapi_host_invoke_wasm_callback`]).
pub fn with_callback_state<R>(
    env: &mut FunctionEnvMut<NapiEnv>,
    snapi_env: SnapiEnv,
    f: impl FnOnce() -> R,
) -> R {
    if snapi_env.is_null() {
        // No env, so no guest callback can fire. Keep this path out of
        // Wasmer's store-lending TLS; callers that allocate guest memory pass
        // their store explicitly.
        return f();
    }

    // Wrap bridge calls that may invoke guest code or allocate through V8's
    // array-buffer allocator, so foreground allocations can grow guest memory.
    // Ordinary N-API imports keep their store local and pass it directly to
    // guest-heap allocation instead of publishing it through runtime TLS.
    env.as_store_mut().parked(f)
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
    // SAFETY: `callback_ctx` is the context `install_callback_ctx` gave the
    // env, which lives at least as long as the env's agent points to it.
    let ctx = unsafe { &*(callback_ctx as *const CallbackInvocationCtx) };
    let func_env = ctx.func_env.clone();

    let dispatched = ctx.with_store(|store| {
        let Some(table) = func_env.as_ref(store).table.clone() else {
            return 0;
        };

        // Bound guest↔host callback reentrancy so a runaway recursion cannot
        // overflow the host native stack (an uncatchable SIGSEGV that would take
        // down every co-tenant). Refuse the callback past the limit rather than
        // crash. The increment below always pairs with the decrement, since
        // `call_guest_callback` returns normally on both success and trap.
        if !func_env.as_mut(store).enter_callback() {
            eprintln!("[callback trampoline] reentrancy depth limit exceeded");
            return 0;
        }
        let result = call_guest_callback(
            store,
            &func_env,
            &table,
            guest_env,
            wasm_fn_ptr,
            callback_arg,
        );
        func_env.as_mut(store).leave_callback();
        result
    });

    match dispatched {
        Some(result) => result,
        None => {
            // No import of this guest lent its store to the code that entered
            // V8, so there is no way back into the guest from here.
            eprintln!("[callback trampoline] the guest's store is not lent to this thread");
            0
        }
    }
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
    // SAFETY: as in `snapi_host_invoke_wasm_callback`.
    let ctx = unsafe { &*(callback_ctx as *const CallbackInvocationCtx) };
    let func_env = ctx.func_env.clone();

    ctx.with_store(|store| {
        let Some(table) = func_env.as_ref(store).table.clone() else {
            return 0;
        };
        // Same reentrancy bound as the regular callback trampoline: a finalizer
        // may itself create/free values and trigger more callbacks.
        if !func_env.as_mut(store).enter_callback() {
            eprintln!("[finalizer trampoline] reentrancy depth limit exceeded");
            return 0;
        }
        let result =
            call_guest_callback2(store, &func_env, &table, guest_env, wasm_fn_ptr, data, hint);
        func_env.as_mut(store).leave_callback();
        result
    })
    .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use wasmer::{Function, TableType, Type};

    #[test]
    fn a_lent_store_respects_the_callback_depth_limit() {
        let mut store = Store::default();
        let mut env = NapiEnv::new(
            crate::budget::ResourceBudget::unlimited(),
            None,
            Default::default(),
            Default::default(),
        );
        let getter = Function::new_typed(&mut store, |_: u32, _: u32| 17u32);
        env.table = Some(
            Table::new(
                &mut store,
                TableType::new(Type::FuncRef, 1, Some(1)),
                Value::FuncRef(Some(getter)),
            )
            .unwrap(),
        );
        // Simulate the active outer callbacks without building a deep native stack.
        for _ in 0..crate::guest::MAX_CALLBACK_DEPTH {
            assert!(env.enter_callback());
        }
        let func_env = FunctionEnv::new(&mut store, env);
        let mut ctx = CallbackInvocationCtx {
            func_env: func_env.clone(),
            store_id: store.objects_mut().id().as_raw().get(),
        };
        let mut invoke = || snapi_host_invoke_wasm_callback((&raw mut ctx).cast(), 0, 0, 0);
        assert_eq!(store.as_store_mut().parked(&mut invoke), 0);
        func_env.as_mut(&mut store).leave_callback();
        assert_eq!(store.as_store_mut().parked(invoke), 17);
    }

    #[test]
    fn a_callback_context_only_runs_with_its_own_store_lent() {
        let mut store = Store::default();
        let mut env = NapiEnv::new(
            crate::budget::ResourceBudget::unlimited(),
            None,
            Default::default(),
            Default::default(),
        );
        let getter = Function::new_typed(&mut store, |_: u32, _: u32| 17u32);
        env.table = Some(
            Table::new(
                &mut store,
                TableType::new(Type::FuncRef, 1, Some(1)),
                Value::FuncRef(Some(getter)),
            )
            .unwrap(),
        );
        let func_env = FunctionEnv::new(&mut store, env);
        let mut ctx = CallbackInvocationCtx {
            func_env,
            store_id: store.objects_mut().id().as_raw().get(),
        };
        let mut invoke = || snapi_host_invoke_wasm_callback((&raw mut ctx).cast(), 0, 0, 0);
        // No store lent, then another store lent: the guest is out of reach.
        assert_eq!(invoke(), 0);
        let mut other = Store::default();
        assert_eq!(other.as_store_mut().parked(&mut invoke), 0);
        assert_eq!(store.as_store_mut().parked(invoke), 17);
    }
}
