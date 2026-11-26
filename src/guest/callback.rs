use std::{cell::Cell, ffi::c_void, sync::Arc};

use wasmer::sys::vm::TrapCode;
use wasmer::{AsyncEnv, FunctionEnv, FunctionEnvMut, RuntimeError, Value};

use super::callback_cache::CachedGuestCallback;
use crate::{NapiEnv, snapi::SnapiEnv};

thread_local! {
    /// The error that ended a guest callback and that its guest must unwind
    /// with: the exit of its process, an abort by the host. The trampoline
    /// cannot propagate an error through V8's native frames. The enclosing
    /// N-API import retrieves it and returns it to Wasmer.
    static PENDING_UNWIND: Cell<Option<RuntimeError>> = const { Cell::new(None) };
}

/// Whether an error that ended a guest callback has to unwind the guest that
/// called into JavaScript, too.
///
/// A fault in the callback's own code (an out-of-bounds access, `unreachable`,
/// an exception it does not catch) ends only the callback: the trampoline
/// reports it and JavaScript goes on. Every other error is the host ending
/// the guest: its WASIX process exited, its thread was interrupted, the host
/// aborted it. Those carry no trap code, except the interrupt.
fn unwinds_guest(err: &RuntimeError) -> bool {
    if err.is_exception() {
        return false;
    }
    match err.clone().to_trap() {
        None => true,
        Some(code) => code == TrapCode::HostInterrupt,
    }
}

/// If a guest callback ended with an error its guest must unwind with, stash
/// it so the enclosing import boundary can re-raise it, and report whether it
/// did. Other errors return `false` and are left for the caller to log.
///
/// The first error wins: a callback that runs while one is already stashed
/// (JavaScript went on after the bridge reported the first as an exception)
/// must not replace the error its guest ends with.
fn stash_guest_unwind(err: &RuntimeError) -> bool {
    if !unwinds_guest(err) {
        return false;
    }
    PENDING_UNWIND.with(|pending| {
        let first = pending.take();
        pending.set(Some(first.unwrap_or_else(|| err.clone())));
    });
    true
}

/// Take the pending error stashed by a callback trampoline, if any: what an
/// import boundary does when its bridge call is over.
pub(crate) fn take_pending_guest_unwind() -> Option<RuntimeError> {
    PENDING_UNWIND.take()
}

/// What [`snapi_host_invoke_wasm_callback`] returns in place of a value id
/// when JavaScript cannot enter the guest from where it runs, so the guest
/// function did not run. The bridge turns it into a JavaScript error.
///
/// No value id has this value: an id holds its slot index plus one in the
/// upper 20 bits, which is never zero, and here those bits are zero.
/// `kCallbackGuestOutOfReach` in `napi_bridge_init.cc` is the same constant
/// and checks that against the id layout. A guest that returns this number
/// itself is handled in [`snapi_host_invoke_wasm_callback`].
pub(crate) const CALLBACK_GUEST_OUT_OF_REACH: u32 = 1;

/// What a V8 callback has of the guest it belongs to: the guest's env. That
/// does not change for the life of a bridge env, so [`install_callback_ctx`]
/// gives each env one when it is created. It holds no store. Whether the
/// store can be had where a callback fires is the engine's to say (see
/// [`dispatch`]). C++ only ever passes this back as an opaque pointer.
pub(crate) struct CallbackInvocationCtx {
    env: FunctionEnv<NapiEnv>,
}

impl CallbackInvocationCtx {
    /// The context of the guest whose env is `env`.
    pub(crate) fn of(env: FunctionEnv<NapiEnv>) -> Self {
        Self { env }
    }

    /// A handle to the guest's env, for one callback. It is not a borrow of
    /// this context, which the guest function can release with its env.
    fn async_env(&self) -> AsyncEnv<NapiEnv> {
        self.env.async_env()
    }
}

/// Why a trampoline does not call the guest function it was asked for.
enum Refused {
    /// JavaScript cannot enter this guest from where it runs.
    OutOfReach,
    /// The function does not exist, or the thread is too deep in callbacks.
    /// The trampoline returns the null id.
    NotCalled,
}

/// Calls the guest function at `wasm_fn_ptr` of the guest `cx` is of, and
/// returns what `call` did, or 0 if the function ended with an error.
///
/// JavaScript can enter a guest where the engine can give it the guest's
/// store: while an import of that store that can run JavaScript is in a
/// bridge call, which is an async host function that holds no slice. It
/// cannot from a synchronous import, which has the store itself, nor from
/// outside every import, nor from an import of another store. The engine
/// tells those apart (`AsyncEnv::with_if_in_reach`), and a guest function
/// that is called takes the store with it: JavaScript that one of its
/// synchronous imports runs is refused in turn.
///
/// The one slice this takes of the store is for the function itself. How
/// deep the thread is in such calls is the driver's to say (see `drive`),
/// which is where `call` gets `None` from.
fn dispatch(
    mut cx: AsyncEnv<NapiEnv>,
    wasm_fn_ptr: u32,
    trampoline: &str,
    call: impl FnOnce(&CachedGuestCallback, &mut AsyncEnv<NapiEnv>) -> Option<Result<u32, RuntimeError>>,
) -> Result<u32, Refused> {
    let func = cx
        .with_if_in_reach(|mut env| guest_function(&mut env, wasm_fn_ptr))
        .ok_or(Refused::OutOfReach)?
        .ok_or(Refused::NotCalled)?;
    let Some(result) = call(&func, &mut cx) else {
        eprintln!("[{trampoline} trampoline] reentrancy depth limit exceeded");
        return Err(Refused::NotCalled);
    };
    Ok(result.unwrap_or_else(|err| {
        if !stash_guest_unwind(&err) {
            eprintln!("[{trampoline} trampoline] error calling guest function: {err}");
        }
        0
    }))
}

/// The function at `wasm_fn_ptr` of the guest's table, if it has one.
fn guest_function(
    env: &mut FunctionEnvMut<'_, NapiEnv>,
    wasm_fn_ptr: u32,
) -> Option<CachedGuestCallback> {
    if let Some(func) = env.data_mut().func_cache.get(wasm_fn_ptr) {
        return Some(func);
    }
    let table = env.data().table.clone()?;
    let func = match table.get(env, wasm_fn_ptr) {
        Some(Value::FuncRef(Some(func))) => CachedGuestCallback::new(func, env),
        _ => return None,
    };
    env.data_mut().func_cache.insert(wasm_fn_ptr, func.clone());
    Some(func)
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
    let ctx = Arc::new(CallbackInvocationCtx::of(env.as_ref()));
    let ctx_ptr = Arc::as_ptr(&ctx).cast_mut().cast::<c_void>();
    env.data_mut().native_callback_contexts.insert(env_id, ctx);
    unsafe {
        crate::snapi::snapi_bridge_swap_active_callback_ctx(snapi_env, ctx_ptr);
    }
}

/// Rust trampoline called from C++ when a V8 callback fires. Calls the guest
/// function, if JavaScript can enter the guest from where it runs.
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
    // env, which lives at least as long as the env's agent points to it. It
    // is only used here: the guest function can release the env, and the
    // context with it.
    let cx = unsafe { &*(callback_ctx as *const CallbackInvocationCtx) }.async_env();

    match dispatch(cx, wasm_fn_ptr, "callback", |func, cx| {
        func.call(cx, guest_env, callback_arg)
    }) {
        // A guest function that returns the sentinel's number returned no
        // valid value id; report that as the null id, so the bridge does not
        // take it for a call that never ran.
        Ok(CALLBACK_GUEST_OUT_OF_REACH) => 0,
        Ok(result) => result,
        Err(Refused::NotCalled) => 0,
        Err(Refused::OutOfReach) => {
            // No import of this guest that can run JavaScript led here, so
            // the guest's store cannot be had. The bridge throws in the
            // JavaScript that made the call.
            eprintln!("[callback trampoline] the guest is out of reach of this call");
            CALLBACK_GUEST_OUT_OF_REACH
        }
    }
}

/// Rust trampoline called from C++ when a guest-registered N-API finalizer
/// fires (during the deferred finalizer drain, which runs inside a guest→bridge
/// import call). Dispatches `finalize(env, data, hint)` through the indirect
/// table, as a callback is. The guest's ABI is
/// `void finalize(napi_env env, void* data, void* hint)`.
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
    let cx = unsafe { &*(callback_ctx as *const CallbackInvocationCtx) }.async_env();

    // A finalizer may itself create and free values and trigger more
    // callbacks: it has the reentrancy bound of the callback trampoline.
    dispatch(cx, wasm_fn_ptr, "finalizer", |func, cx| {
        func.call_finalizer(cx, guest_env, data, hint)
    })
    .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::super::import::{Cx, import};
    use super::*;
    use wasmer::{
        AsStoreMut, Function, FunctionEnv, Instance, Module, Store, Table, TableType, Type, imports,
    };

    fn napi_env() -> NapiEnv {
        NapiEnv::new(
            crate::budget::ResourceBudget::unlimited(),
            None,
            Default::default(),
            Default::default(),
        )
    }

    /// The id of the store `cx` reaches, read in a slice of it.
    fn store_of(cx: &mut Cx<'_>) -> usize {
        cx.with(|mut env| env.objects_mut().id().as_raw().get())
    }

    /// A guest function that calls `import` and returns what it returned.
    fn guest_calling(store: &mut Store, import: Function) -> Function {
        let module = Module::new(
            &store,
            r#"(module
              (import "host" "import" (func $import (result i32)))
              (func (export "run") (result i32) call $import))"#,
        )
        .unwrap();
        let instance = Instance::new(
            store,
            &module,
            &imports! { "host" => { "import" => import } },
        )
        .unwrap();
        instance.exports.get_function("run").unwrap().clone()
    }

    /// Calls `import` as a guest would and returns what it returned.
    fn run(store: &mut Store, import: Function) -> i32 {
        let guest = guest_calling(store, import);
        guest.call(store, &[]).expect("the guest trapped")[0].unwrap_i32()
    }

    /// An env whose guest has one function, at index 0, which returns 17.
    fn env_with_a_guest_function(store: &mut Store) -> NapiEnv {
        let mut env = napi_env();
        let getter = Function::new_typed(store, |_: u32, _: u32| 17u32);
        env.table = Some(
            Table::new(
                store,
                TableType::new(Type::FuncRef, 1, Some(1)),
                Value::FuncRef(Some(getter)),
            )
            .unwrap(),
        );
        env
    }

    /// The callback context of `func_env`, as the bridge holds it.
    fn callback_ctx(func_env: &FunctionEnv<NapiEnv>) -> *mut c_void {
        let ctx = CallbackInvocationCtx::of(func_env.clone());
        std::ptr::from_mut(Box::leak(Box::new(ctx))).cast()
    }

    /// Calls the guest function at `index` of `ctx` as the bridge does when
    /// JavaScript calls it, and returns what the trampoline returned.
    fn callback(ctx: *mut c_void, index: u32) -> u32 {
        snapi_host_invoke_wasm_callback(ctx, 0, index, 0)
    }

    /// [`callback`] of index 0, from JavaScript that an import of `env` runs.
    fn call_from_javascript(
        store: &mut Store,
        env: &FunctionEnv<NapiEnv>,
        ctx: *mut c_void,
    ) -> u32 {
        let import = import(store, env, move |_: &mut Cx<'_>| -> i32 {
            callback(ctx, 0) as i32
        });
        run(store, import) as u32
    }

    /// An env whose guest is `wat`, with `imports` as the module `host` and
    /// its export `table` as the functions JavaScript can call. `imports`
    /// gets the env and its callback context to make the imports with.
    fn guest(
        store: &mut Store,
        wat: &str,
        imports: impl FnOnce(
            &mut Store,
            &FunctionEnv<NapiEnv>,
            *mut c_void,
        ) -> Vec<(&'static str, Function)>,
    ) -> (FunctionEnv<NapiEnv>, *mut c_void, Instance) {
        let env = FunctionEnv::new(store, napi_env());
        let ctx = callback_ctx(&env);
        let mut host = wasmer::Exports::new();
        for (name, function) in imports(store, &env, ctx) {
            host.insert(name, function);
        }
        let mut import_object = wasmer::Imports::new();
        import_object.register_namespace("host", host);
        let module = Module::new(&store, wat).unwrap();
        let instance = Instance::new(store, &module, &import_object).unwrap();
        env.as_mut(store).table = Some(instance.exports.get_table("table").unwrap().clone());
        (env, ctx, instance)
    }

    /// Calls the export `run` of `instance`.
    fn run_export(store: &mut Store, instance: &Instance) -> i32 {
        let run = instance.exports.get_function("run").unwrap().clone();
        run.call(store, &[]).expect("the guest trapped")[0].unwrap_i32()
    }

    #[test]
    fn javascript_enters_a_guest_only_under_an_import_that_holds_no_slice() {
        let mut store = Store::default();
        let env = env_with_a_guest_function(&mut store);
        let env = FunctionEnv::new(&mut store, env);
        let ctx = callback_ctx(&env);
        // No import of the guest is beneath: its store is out of reach.
        assert_eq!(callback(ctx, 0), CALLBACK_GUEST_OUT_OF_REACH);

        let import = import(&mut store, &env, move |cx: &mut Cx<'_>| -> i32 {
            let id = store_of(cx);
            // The import has the store in a slice: it is not JavaScript's
            // to take.
            let in_slice = cx.with(|_| callback(ctx, 0));
            let mut checks = i32::from(in_slice == CALLBACK_GUEST_OUT_OF_REACH);
            // Outside a slice it is, and each callback gives it back.
            for _ in 0..2 {
                checks = checks << 1 | i32::from(callback(ctx, 0) == 17);
            }
            // The import goes on with its store.
            checks << 1 | i32::from(store_of(cx) == id)
        });
        assert_eq!(run(&mut store, import), 0b1111);
        assert_eq!(callback(ctx, 0), CALLBACK_GUEST_OUT_OF_REACH);
    }

    #[test]
    fn imports_nest_through_a_callback() {
        // JavaScript calls a guest function, which calls an import that runs
        // JavaScript, which calls a guest function.
        let mut store = Store::default();
        let (_env, _ctx, instance) = guest(
            &mut store,
            r#"(module
              (import "host" "outer" (func $outer (result i32)))
              (import "host" "inner" (func $inner (result i32)))
              (table (export "table") 2 2 funcref)
              (elem (i32.const 0) $calls_inner $leaf)
              (func $calls_inner (param i32 i32) (result i32) call $inner)
              (func $leaf (param i32 i32) (result i32) i32.const 17)
              (func (export "run") (result i32) call $outer))"#,
            |store, env, ctx| {
                let inner = import(store, env, move |_: &mut Cx<'_>| -> i32 {
                    callback(ctx, 1) as i32
                });
                let outer = import(store, env, move |cx: &mut Cx<'_>| -> i32 {
                    let id = store_of(cx);
                    let nested = callback(ctx, 0);
                    // The callback returned: the outer import is the way in
                    // again, and has its store.
                    let again = callback(ctx, 1);
                    i32::from(nested == 17) << 2
                        | i32::from(again == 17) << 1
                        | i32::from(store_of(cx) == id)
                });
                vec![("outer", outer), ("inner", inner)]
            },
        );
        assert_eq!(run_export(&mut store, &instance), 0b111);
    }

    #[test]
    fn a_synchronous_import_of_a_called_guest_function_is_out_of_reach() {
        // A synchronous import holds the store: JavaScript that it runs
        // cannot enter the guest, and is told so instead of taking a store
        // that is not there to take.
        let mut store = Store::default();
        let (_env, _ctx, instance) = guest(
            &mut store,
            r#"(module
              (import "host" "outer" (func $outer (result i32)))
              (import "host" "synchronous" (func $synchronous (result i32)))
              (table (export "table") 2 2 funcref)
              (elem (i32.const 0) $calls_synchronous $leaf)
              (func $calls_synchronous (param i32 i32) (result i32) call $synchronous)
              (func $leaf (param i32 i32) (result i32) i32.const 17)
              (func (export "run") (result i32) call $outer))"#,
            |store, env, ctx| {
                let ctx = ctx as usize;
                // 42 if the guest was out of reach of JavaScript that this
                // import ran. Not the sentinel itself, which the trampoline
                // does not let a guest function return.
                let synchronous = Function::new_typed_with_env(
                    store,
                    env,
                    move |_: FunctionEnvMut<NapiEnv>| -> i32 {
                        let refused = callback(ctx as *mut c_void, 1);
                        i32::from(refused == CALLBACK_GUEST_OUT_OF_REACH) * 42
                    },
                );
                let outer = import(store, env, move |cx: &mut Cx<'_>| -> i32 {
                    let refused = callback(ctx as *mut c_void, 0);
                    // Then the import can use its store again, and
                    // JavaScript it runs can enter the guest.
                    i32::from(refused == 42) << 2
                        | i32::from(store_of(cx) != 0) << 1
                        | i32::from(callback(ctx as *mut c_void, 1) == 17)
                });
                vec![("outer", outer), ("synchronous", synchronous)]
            },
        );
        assert_eq!(run_export(&mut store, &instance), 0b111);
    }

    /// Polls `future` once, which is all a call that never waits needs.
    fn ready<T>(future: impl std::future::Future<Output = T>) -> T {
        use std::{
            pin::pin,
            task::{Context, Poll, Waker},
        };

        let mut future = pin!(future);
        match future
            .as_mut()
            .poll(&mut Context::from_waker(Waker::noop()))
        {
            Poll::Ready(output) => output,
            Poll::Pending => panic!("the call suspended"),
        }
    }

    #[test]
    fn javascript_enters_the_guest_however_the_guest_was_called() {
        // A guest whose import reports whether JavaScript it runs can call
        // a guest function.
        let mut store = Store::default();
        let env = env_with_a_guest_function(&mut store);
        let env = FunctionEnv::new(&mut store, env);
        let ctx = callback_ctx(&env);
        let import = import(&mut store, &env, move |_: &mut Cx<'_>| -> i32 {
            i32::from(callback(ctx, 0) == 17)
        });
        let guest = guest_calling(&mut store, import);

        // A synchronous call.
        assert_eq!(guest.call(&mut store, &[]).unwrap()[0].unwrap_i32(), 1);

        // An async call that holds the store.
        let results = ready(guest.call_async(&mut store, &[])).expect("the guest trapped");
        assert_eq!(results[0].unwrap_i32(), 1);

        // How a WASIX v2 machine runs its guests: every task has a handle to
        // its store and takes the store for the slice it runs.
        let mut shared = store.into_shared();
        let results = ready(guest.call_async(&mut shared, &[])).expect("the guest trapped");
        assert_eq!(results[0].unwrap_i32(), 1);
    }

    #[test]
    fn a_callback_waits_for_an_async_import_of_its_guest() {
        // A guest function that JavaScript calls reaches an async import that
        // another thread completes: a WASIX v2 guest reading a file. The
        // trampoline stays in the call until the import is done.
        use std::sync::atomic::{AtomicBool, Ordering};

        let mut store = Store::default();
        let waited = Arc::new(AtomicBool::new(false));
        let slow = {
            let waited = waited.clone();
            let unit = FunctionEnv::new(&mut store, ());
            Function::new_typed_async(
                &mut store,
                &unit,
                async move |_: &mut wasmer::AsyncEnv<()>| -> i32 {
                    let (sender, receiver) = std::sync::mpsc::channel::<std::task::Waker>();
                    let done = Arc::new(AtomicBool::new(false));
                    {
                        let done = done.clone();
                        std::thread::spawn(move || {
                            let waker = receiver.recv().unwrap();
                            std::thread::sleep(std::time::Duration::from_millis(20));
                            done.store(true, Ordering::SeqCst);
                            waker.wake();
                        });
                    }
                    let mut sender = Some(sender);
                    std::future::poll_fn(|cx| {
                        if done.load(Ordering::SeqCst) {
                            return std::task::Poll::Ready(());
                        }
                        if let Some(sender) = sender.take() {
                            sender.send(cx.waker().clone()).unwrap();
                        }
                        std::task::Poll::Pending
                    })
                    .await;
                    waited.store(true, Ordering::SeqCst);
                    17
                },
            )
        };
        let module = Module::new(
            &store,
            r#"(module
              (import "host" "slow" (func $slow (result i32)))
              (table (export "table") 1 1 funcref)
              (elem (i32.const 0) $callback)
              (func $callback (param i32 i32) (result i32) call $slow))"#,
        )
        .unwrap();
        let instance = Instance::new(
            &mut store,
            &module,
            &imports! { "host" => { "slow" => slow } },
        )
        .unwrap();
        let mut env = napi_env();
        env.table = Some(instance.exports.get_table("table").unwrap().clone());
        let func_env = FunctionEnv::new(&mut store, env);
        let ctx = callback_ctx(&func_env);
        assert_eq!(call_from_javascript(&mut store, &func_env, ctx), 17);
        assert!(waited.load(Ordering::SeqCst));
    }

    #[derive(Debug)]
    struct Exit(i32);

    impl std::fmt::Display for Exit {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            write!(f, "exit {}", self.0)
        }
    }

    impl std::error::Error for Exit {}

    #[test]
    fn a_callback_that_ends_its_guest_leaves_the_error_to_its_import() {
        let mut store = Store::default();
        let (_env, _ctx, instance) = guest(
            &mut store,
            r#"(module
              (import "host" "outer" (func $outer (result i32)))
              (import "host" "exit" (func $exit (result i32)))
              (table (export "table") 2 2 funcref)
              (elem (i32.const 0) $exits $leaf)
              (func $exits (param i32 i32) (result i32) call $exit)
              (func $leaf (param i32 i32) (result i32) i32.const 17)
              (func (export "run") (result i32) call $outer))"#,
            |store, env, ctx| {
                let exit = Function::new_typed(store, || -> Result<i32, RuntimeError> {
                    Err(RuntimeError::user(Box::new(Exit(7))))
                });
                let outer = import(store, env, move |_: &mut Cx<'_>| -> i32 {
                    // The trampoline cannot return the error through V8.
                    let returned = callback(ctx, 0);
                    let stashed = take_pending_guest_unwind()
                        .is_some_and(|error| matches!(error.downcast_ref::<Exit>(), Some(Exit(7))));
                    // Taken, not copied.
                    let taken = take_pending_guest_unwind().is_none();
                    // The callback gave its level and the store back.
                    let again = callback(ctx, 1);
                    i32::from(returned == 0) << 3
                        | i32::from(stashed) << 2
                        | i32::from(taken) << 1
                        | i32::from(again == 17)
                });
                vec![("outer", outer), ("exit", exit)]
            },
        );
        assert_eq!(run_export(&mut store, &instance), 0b1111);
    }

    #[test]
    fn only_what_ends_the_guest_is_stashed() {
        // The host ending the guest: stashed, and the first one is kept.
        assert!(stash_guest_unwind(&RuntimeError::user(Box::new(Exit(1)))));
        assert!(stash_guest_unwind(&RuntimeError::user(Box::new(Exit(2)))));
        assert!(stash_guest_unwind(&RuntimeError::new("the host gave up")));
        let first = take_pending_guest_unwind().expect("an error was stashed");
        assert!(matches!(first.downcast_ref::<Exit>(), Some(Exit(1))));
        assert!(take_pending_guest_unwind().is_none());

        // A fault of the callback's own code ends only the callback.
        let mut store = Store::default();
        let module =
            wasmer::Module::new(&store, r#"(module (func (export "fault") unreachable))"#).unwrap();
        let instance = wasmer::Instance::new(&mut store, &module, &wasmer::imports! {}).unwrap();
        let fault = instance
            .exports
            .get_function("fault")
            .unwrap()
            .call(&mut store, &[])
            .unwrap_err();
        assert!(!stash_guest_unwind(&fault));
        assert!(take_pending_guest_unwind().is_none());
    }

    /// Runs `innermost` inside `depth` guest calls, as far as the driver of
    /// guest calls can tell.
    fn nested<R>(depth: u32, innermost: impl FnOnce() -> R) -> R {
        if depth == 0 {
            return innermost();
        }
        super::super::drive::run_to_completion(async { nested(depth - 1, innermost) })
            .expect("within the limit")
    }

    #[test]
    fn a_callback_respects_the_callback_depth_limit() {
        let mut store = Store::default();
        let env = env_with_a_guest_function(&mut store);
        let func_env = FunctionEnv::new(&mut store, env);
        let ctx = callback_ctx(&func_env);
        let max = crate::guest::MAX_CALLBACK_DEPTH;
        // Past the limit the function is not called, and the trampoline
        // returns the null id.
        let refused = nested(max, || call_from_javascript(&mut store, &func_env, ctx));
        assert_eq!(refused, 0);
        let called = nested(max - 1, || call_from_javascript(&mut store, &func_env, ctx));
        assert_eq!(called, 17);
        // Each call gave its level back.
        assert_eq!(call_from_javascript(&mut store, &func_env, ctx), 17);
    }

    #[test]
    fn a_callback_context_only_runs_through_an_import_of_its_own_store() {
        let mut store = Store::default();
        let env = env_with_a_guest_function(&mut store);
        let func_env = FunctionEnv::new(&mut store, env);
        let ctx = callback_ctx(&func_env);
        // No import beneath, then one of another store: the guest is out of
        // reach.
        assert_eq!(callback(ctx, 0), CALLBACK_GUEST_OUT_OF_REACH);
        let mut other = Store::default();
        let other_env = FunctionEnv::new(&mut other, napi_env());
        assert_eq!(
            call_from_javascript(&mut other, &other_env, ctx),
            CALLBACK_GUEST_OUT_OF_REACH
        );
        assert_eq!(call_from_javascript(&mut store, &func_env, ctx), 17);

        // A kept env's context outlives its guest, and names a store that
        // is gone: nothing reaches it.
        drop(store);
        assert_eq!(callback(ctx, 0), CALLBACK_GUEST_OUT_OF_REACH);
        assert_eq!(
            call_from_javascript(&mut other, &other_env, ctx),
            CALLBACK_GUEST_OUT_OF_REACH
        );
    }
}
