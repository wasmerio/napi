use std::{
    any::Any,
    cell::{Cell, RefCell},
    ffi::c_void,
    panic::{AssertUnwindSafe, catch_unwind, resume_unwind},
    sync::{Arc, atomic::Ordering},
};

use wasmer::{AsStoreMut, FunctionEnv, FunctionEnvMut, RuntimeError, Store, StoreMut, Value};

use crate::{NapiEnv, snapi::SnapiEnv};

enum CallbackFailure {
    Guest(RuntimeError),
    Panic(Box<dyn Any + Send>),
}

/// One physical bridge call owns its callback result. Nested imports install
/// their own context and return the original error through the guest stack.
struct CallbackInvocationCtx {
    func_env: FunctionEnv<NapiEnv>,
    failure: RefCell<Option<CallbackFailure>>,
    terminated: Cell<bool>,
    snapi_env: SnapiEnv,
}

impl CallbackInvocationCtx {
    fn fail(&self, failure: CallbackFailure) {
        let mut slot = self.failure.borrow_mut();
        if slot.is_none() {
            *slot = Some(failure);
            // Returning a null value alone lets JS catch and continue forever.
            self.terminated.set(true);
            unsafe {
                crate::snapi::snapi_bridge_unofficial_terminate_execution(self.snapi_env);
            }
        }
    }
}

fn call_guest_callback(
    store: &mut StoreMut<'_>,
    func_env: &FunctionEnv<NapiEnv>,
    wasm_fn_ptr: u32,
    args: &[Value],
) -> Result<u32, RuntimeError> {
    let Some(table) = func_env.as_ref(store).table.clone() else {
        return Ok(0);
    };
    // A guest can replace a table entry without changing its signature.
    let Some(Value::FuncRef(Some(func))) = table.get(store, wasm_fn_ptr) else {
        return Ok(0);
    };
    let values = func.call(store, args)?;
    Ok(match values.first() {
        Some(Value::I32(value)) => *value as u32,
        Some(Value::I64(value)) => *value as u32,
        _ => 0,
    })
}

/// Lend the store only while the bridge may synchronously call the guest.
/// Errors and panics cannot cross C++ frames; restore those frames first.
pub fn with_callback_state<R>(
    env: &mut FunctionEnvMut<NapiEnv>,
    snapi_env: SnapiEnv,
    f: impl FnOnce() -> R,
) -> Result<R, RuntimeError> {
    if snapi_env.is_null() {
        return Ok(f());
    }

    let mut ctx = CallbackInvocationCtx {
        func_env: env.as_ref(),
        failure: RefCell::new(None),
        terminated: Cell::new(false),
        snapi_env,
    };
    let host_stopped = env.data().host_stop_flag();
    let already_terminating =
        unsafe { crate::snapi::snapi_bridge_unofficial_is_execution_terminating(snapi_env) != 0 };
    let prev = unsafe {
        crate::snapi::snapi_bridge_swap_active_callback_ctx(
            snapi_env,
            (&mut ctx as *mut CallbackInvocationCtx).cast::<c_void>(),
        )
    };
    struct CallbackStateGuard<'a> {
        snapi_env: SnapiEnv,
        prev: *mut c_void,
        terminated: &'a Cell<bool>,
        already_terminating: bool,
        host_stopped: Arc<std::sync::atomic::AtomicBool>,
    }
    impl Drop for CallbackStateGuard<'_> {
        fn drop(&mut self) {
            unsafe {
                crate::snapi::snapi_bridge_swap_active_callback_ctx(self.snapi_env, self.prev);
                if self.terminated.get()
                    && !self.already_terminating
                    && !self.host_stopped.load(Ordering::Acquire)
                {
                    crate::snapi::snapi_bridge_unofficial_cancel_terminate_execution(
                        self.snapi_env,
                    );
                    // terminate_all sets its flag before entering the registry.
                    if self.host_stopped.load(Ordering::Acquire) {
                        crate::snapi::snapi_bridge_unofficial_terminate_execution(self.snapi_env);
                    }
                }
            }
        }
    }
    let result = {
        let _guard = CallbackStateGuard {
            snapi_env,
            prev,
            terminated: &ctx.terminated,
            already_terminating,
            host_stopped,
        };
        env.as_store_mut().parked(f)
    };
    match ctx.failure.into_inner() {
        Some(CallbackFailure::Guest(error)) => Err(error),
        Some(CallbackFailure::Panic(panic)) => resume_unwind(panic),
        None => Ok(result),
    }
}

fn invoke(callback_ctx: *mut c_void, wasm_fn_ptr: u32, args: &[Value]) -> u32 {
    if callback_ctx.is_null() {
        return 0;
    }
    // The invocation's stack context outlives the bridge and its callbacks.
    let ctx = unsafe { &*callback_ctx.cast::<CallbackInvocationCtx>() };
    match catch_unwind(AssertUnwindSafe(|| invoke_inner(ctx, wasm_fn_ptr, args))) {
        Ok(value) => value,
        Err(panic) => {
            ctx.fail(CallbackFailure::Panic(panic));
            0
        }
    }
}

fn invoke_inner(ctx: &CallbackInvocationCtx, wasm_fn_ptr: u32, args: &[Value]) -> u32 {
    if ctx.failure.borrow().is_some() {
        return 0;
    }
    let dispatched = Store::with_current(|store| {
        let entered = Cell::new(false);
        // Catch before Store::with_current drops its borrow wrapper: that wrapper
        // cannot restore its borrow count while unwinding.
        let result = catch_unwind(AssertUnwindSafe(|| {
            if !ctx.func_env.as_mut(store).enter_callback() {
                return Ok(0);
            }
            entered.set(true);
            call_guest_callback(store, &ctx.func_env, wasm_fn_ptr, args)
        }));
        if entered.get() {
            ctx.func_env.as_mut(store).leave_callback();
        }
        result
    });
    match dispatched {
        Some(Ok(Ok(value))) => value,
        Some(Ok(Err(error))) => {
            ctx.fail(CallbackFailure::Guest(error));
            0
        }
        Some(Err(panic)) => {
            ctx.fail(CallbackFailure::Panic(panic));
            0
        }
        None => 0,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snapi_host_invoke_wasm_callback(
    callback_ctx: *mut c_void,
    guest_env: u32,
    wasm_fn_ptr: u32,
    callback_arg: u32,
) -> u32 {
    invoke(
        callback_ctx,
        wasm_fn_ptr,
        &[
            Value::I32(guest_env as i32),
            Value::I32(callback_arg as i32),
        ],
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn snapi_host_invoke_wasm_finalizer(
    callback_ctx: *mut c_void,
    guest_env: u32,
    wasm_fn_ptr: u32,
    data: u32,
    hint: u32,
) -> u32 {
    invoke(
        callback_ctx,
        wasm_fn_ptr,
        &[
            Value::I32(guest_env as i32),
            Value::I32(data as i32),
            Value::I32(hint as i32),
        ],
    )
}
