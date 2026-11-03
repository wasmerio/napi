use std::{
    any::Any,
    cell::{Cell, RefCell},
    ffi::c_void,
    panic::{AssertUnwindSafe, catch_unwind, resume_unwind},
    sync::{Arc, atomic::Ordering},
};

use wasmer::sys::FunctionEnvFramePin;
use wasmer::{AsStoreMut, FunctionEnvMut, RuntimeError, Store, StoreMut, Table, Value};

use crate::{NapiEnv, snapi::SnapiEnv};

enum CallbackFailure {
    Guest(RuntimeError),
    Panic(Box<dyn Any + Send>),
}

// Native typed handlers expose the same primitive Wasm ABI while returning
// callback failures only after original-cell cleanup has completed.
pub(super) trait NativeImportOutput {
    type Value: Copy;
    fn into_native_result(self) -> Result<Self::Value, RuntimeError>;
}

impl NativeImportOutput for i32 {
    type Value = i32;
    fn into_native_result(self) -> Result<i32, RuntimeError> {
        Ok(self)
    }
}
impl NativeImportOutput for () {
    type Value = ();
    fn into_native_result(self) -> Result<(), RuntimeError> {
        Ok(self)
    }
}
impl NativeImportOutput for Result<i32, RuntimeError> {
    type Value = i32;
    fn into_native_result(self) -> Result<i32, RuntimeError> {
        self
    }
}

pub(super) fn with_native_import<O: NativeImportOutput>(
    env: FunctionEnvMut<NapiEnv>,
    body: impl FnOnce(FunctionEnvMut<NapiEnv>) -> O,
) -> Result<O::Value, RuntimeError> {
    // Sys imports retain their exact original allocation even after its Store
    // slot is removed. Other backends retain their existing import behavior;
    // native physical callback admission remains sys-only.
    let frame = env.pin_frame();
    let mut value = None;
    let mut failure = match catch_unwind(AssertUnwindSafe(|| body(env).into_native_result())) {
        Ok(Ok(result)) => {
            value = Some(result);
            None
        }
        Ok(Err(error)) => Some(CallbackFailure::Guest(error)),
        Err(panic) => Some(CallbackFailure::Panic(panic)),
    };
    // The original FunctionEnvMut has already ended, including on panic. Its
    // retained cell can now run its last destructor outside that body unwind.
    if let Err(panic) = catch_unwind(AssertUnwindSafe(|| drop(frame))) {
        if failure.is_none() {
            failure = Some(CallbackFailure::Panic(panic));
        } else {
            let _ = catch_unwind(AssertUnwindSafe(|| drop(panic)));
        }
    }
    match failure {
        Some(CallbackFailure::Guest(error)) => Err(error),
        Some(CallbackFailure::Panic(panic)) => resume_unwind(panic),
        None => Ok(value.unwrap()),
    }
}

/// One physical bridge call owns its callback result. Nested imports install
/// their own context and return the original error through the guest stack.
struct CallbackInvocationCtx {
    environment: FunctionEnvFramePin<NapiEnv>,
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
    table: Option<Table>,
    wasm_fn_ptr: u32,
    args: &[Value],
) -> Result<u32, RuntimeError> {
    let Some(table) = table else {
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

    let environment = env
        .pin_frame()
        .ok_or_else(|| RuntimeError::new("native callbacks require the sys backend"))?;
    let mut ctx = CallbackInvocationCtx {
        environment,
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
        let Some(mut environment) = ctx.environment.try_as_env_mut(store) else {
            return Ok(Ok(0));
        };
        if !environment.data_mut().enter_callback() {
            return Ok(Ok(0));
        }
        let table = environment.data().table.clone();
        // End every T borrow before guest entry. The same temporary retains the
        // exact original cell for leave_callback even if guest removes its slot.
        // Catch before with_current releases its counted Store borrow wrapper.
        let result = catch_unwind(AssertUnwindSafe(|| {
            call_guest_callback(&mut environment.as_store_mut(), table, wasm_fn_ptr, args)
        }));
        let cleanup = catch_unwind(AssertUnwindSafe(|| environment.data_mut().leave_callback()));
        match (result, cleanup) {
            (result, Ok(())) => result,
            (Ok(Ok(_)), Err(panic)) => Err(panic),
            (result, Err(secondary)) => {
                // Preserve the original callback failure after paired cleanup.
                let _ = catch_unwind(AssertUnwindSafe(|| drop(secondary)));
                result
            }
        }
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

#[cfg(test)]
mod import_tests {
    use super::*;
    use crate::budget::{NapiMemoryAccountant, Pool, ResourceBudget};
    use std::{
        collections::HashSet,
        process::{Child, Command},
        sync::{
            Mutex,
            atomic::{AtomicBool, AtomicU64, AtomicUsize},
        },
        time::{Duration, Instant},
    };
    use wasmer::sys::vm::{StoreObject, VMFunctionEnvironment, VMFunctionEnvironmentIdentity};
    use wasmer::{Function, FunctionEnv, Imports, Instance, Module};

    struct Accountant {
        charged: AtomicU64,
        armed: AtomicBool,
        drops: AtomicUsize,
        payload: Arc<()>,
    }
    impl NapiMemoryAccountant for Accountant {
        fn memory_limit(&self) -> u64 {
            1024 * 1024
        }
        fn memory_charged(&self) -> u64 {
            self.charged.load(Ordering::SeqCst)
        }
        fn try_charge(&self, bytes: u64) -> bool {
            self.charged.fetch_add(bytes, Ordering::SeqCst);
            true
        }
        fn uncharge(&self, bytes: u64) {
            self.charged.fetch_sub(bytes, Ordering::SeqCst);
            if self.armed.swap(false, Ordering::SeqCst) {
                self.drops.fetch_add(1, Ordering::SeqCst);
                std::panic::panic_any(self.payload.clone());
            }
        }
    }
    #[derive(Debug)]
    struct Original(Arc<()>);
    impl std::fmt::Display for Original {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            f.write_str("original import failure")
        }
    }
    impl std::error::Error for Original {}
    #[derive(Clone, Copy)]
    enum Body {
        Success,
        Error,
        Panic,
    }

    fn body(
        mut env: FunctionEnvMut<NapiEnv>,
        identity: &VMFunctionEnvironmentIdentity,
        mode: Body,
        original: &Arc<()>,
    ) -> Result<i32, RuntimeError> {
        // This is the sole original NapiEnv, validated while Store was idle.
        // Its raw owner is removed without accessing T through that owner.
        let owner = &mut VMFunctionEnvironment::list_mut(env.objects_mut().as_sys_mut())[0];
        assert_eq!(owner.identity(), *identity);
        drop(std::mem::replace(owner, VMFunctionEnvironment::new(())));
        match mode {
            Body::Success => Ok(42),
            Body::Error => Err(RuntimeError::user(Box::new(Original(original.clone())))),
            Body::Panic => std::panic::panic_any(original.clone()),
        }
    }
    fn proof(mode: Body) {
        for typed in [false, true] {
            let original = Arc::new(());
            let cleanup = Arc::new(());
            let accountant = Arc::new(Accountant {
                charged: AtomicU64::new(0),
                armed: AtomicBool::new(false),
                drops: AtomicUsize::new(0),
                payload: cleanup.clone(),
            });
            let budget = ResourceBudget::with_accountant(accountant.clone());
            budget.try_charge(Pool::V8External, 64).unwrap();
            let mut data = NapiEnv::new(
                budget.clone(),
                None,
                Arc::new(Mutex::new(HashSet::new())),
                Arc::new(AtomicBool::new(false)),
            );
            // Owned ledger-only cleanup, not an actual V8 provider callback.
            // It supplies a deterministic final T destructor after slot removal.
            data.record_declared_external(1, 64);
            let mut store = Store::default();
            assert!(VMFunctionEnvironment::list(store.objects_mut().as_sys_mut()).is_empty());
            let env = FunctionEnv::new(&mut store, data);
            let identity =
                VMFunctionEnvironment::list(store.objects_mut().as_sys_mut())[0].identity();
            let payload = original.clone();
            let function = Function::new_typed_with_env(
                &mut store,
                &env,
                move |env: FunctionEnvMut<NapiEnv>| -> Result<i32, RuntimeError> {
                    with_native_import(env, |env| body(env, &identity, mode, &payload))
                },
            );
            let module = Module::new(
                &store,
                r#"(module
                (import "test" "import" (func $import (result i32)))
                (func (export "run") (result i32) call $import)
                (func (export "fresh") (result i32) i32.const 42))"#,
            )
            .unwrap();
            let mut imports = Imports::new();
            imports.define("test", "import", function);
            let instance = Instance::new(&mut store, &module, &imports).unwrap();
            accountant.armed.store(true, Ordering::SeqCst);
            let outcome = catch_unwind(AssertUnwindSafe(|| {
                if typed {
                    instance
                        .exports
                        .get_typed_function::<(), i32>(&store, "run")
                        .unwrap()
                        .call(&mut store)
                } else {
                    instance
                        .exports
                        .get_function("run")
                        .unwrap()
                        .call(&mut store, &[])
                        .map(|values| values[0].i32().unwrap())
                }
            }));
            let fresh = instance
                .exports
                .get_typed_function::<(), i32>(&store, "fresh")
                .unwrap()
                .call(&mut store)
                .unwrap();
            drop(instance);
            drop(imports);
            drop(env);
            drop(store);
            assert_eq!(fresh, 42);
            assert_eq!(budget.snapshot().v8_external, 0);
            assert_eq!(accountant.memory_charged(), 0);
            assert_eq!(accountant.drops.load(Ordering::SeqCst), 1);
            match mode {
                Body::Error => {
                    let error = outcome
                        .expect("cleanup panic replaced original import error")
                        .unwrap_err();
                    assert!(Arc::ptr_eq(
                        &error.downcast_ref::<Original>().unwrap().0,
                        &original
                    ));
                }
                Body::Panic => {
                    let panic = outcome.unwrap_err();
                    assert!(Arc::ptr_eq(
                        panic.downcast_ref::<Arc<()>>().unwrap(),
                        &original
                    ));
                }
                Body::Success => {
                    let panic = outcome.unwrap_err();
                    assert!(Arc::ptr_eq(
                        panic.downcast_ref::<Arc<()>>().unwrap(),
                        &cleanup
                    ));
                }
            }
        }
    }
    fn subprocess(name: &str) -> bool {
        const CHILD: &str = "NAPI_NATIVE_IMPORT_OUTCOME_CHILD";
        if std::env::var_os(CHILD).is_some() {
            return true;
        }
        struct OwnedChild(Child);
        impl Drop for OwnedChild {
            fn drop(&mut self) {
                let _ = self.0.kill();
                let _ = self.0.wait();
            }
        }
        let mut child = OwnedChild(
            Command::new(std::env::current_exe().unwrap())
                .args(["--exact", name, "--nocapture"])
                .env(CHILD, "1")
                .spawn()
                .unwrap(),
        );
        let started = Instant::now();
        loop {
            if let Some(status) = child.0.try_wait().unwrap() {
                assert!(
                    status.success(),
                    "native import outcome proof failed: {status}"
                );
                return false;
            }
            assert!(started.elapsed() < Duration::from_secs(30));
            std::thread::sleep(Duration::from_millis(10));
        }
    }
    #[test]
    fn native_import_preserves_error_after_removed_cell_cleanup_panics() {
        if subprocess(
            "guest::callback::import_tests::native_import_preserves_error_after_removed_cell_cleanup_panics",
        ) {
            proof(Body::Error);
        }
    }
    #[test]
    fn native_import_preserves_panic_after_removed_cell_cleanup_panics() {
        if subprocess(
            "guest::callback::import_tests::native_import_preserves_panic_after_removed_cell_cleanup_panics",
        ) {
            proof(Body::Panic);
        }
    }
    #[test]
    fn native_import_reports_cleanup_panic_after_successful_body() {
        if subprocess(
            "guest::callback::import_tests::native_import_reports_cleanup_panic_after_successful_body",
        ) {
            proof(Body::Success);
        }
    }
}
