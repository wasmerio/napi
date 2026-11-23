use std::{
    cell::{Cell, RefCell},
    ffi::c_void,
    ptr,
    sync::Arc,
};

use wasmer::sys::vm::TrapCode;
use wasmer::{AsStoreMut, AsyncEnv, FunctionEnv, FunctionEnvMut, RuntimeError, Table, Value};

use super::callback_cache::CachedGuestCallback;
use crate::{NapiEnv, snapi::SnapiEnv};

thread_local! {
    /// The error that ended a guest callback and that its guest must unwind
    /// with: the exit of its process, an abort by the host. The trampoline
    /// cannot propagate an error through V8's native frames. The enclosing
    /// N-API import retrieves it and returns it to Wasmer.
    static PENDING_GUEST_UNWIND: RefCell<Option<RuntimeError>> = const { RefCell::new(None) };

    /// The import that is in the bridge on this thread, for what the bridge
    /// calls back: a V8 callback into the guest, the guest-heap allocator.
    /// Null outside a bridge call, and while one of those uses it.
    static IN_BRIDGE: Cell<*mut AsyncEnv<NapiEnv>> = const { Cell::new(ptr::null_mut()) };
}

/// Runs the bridge call `f` of the import `cx`, which holds no borrow of its
/// store meanwhile: V8 callbacks and the guest-heap allocator reach the store
/// through `cx` (see [`with_import_in_bridge`]). Returns any error the bridge
/// call must unwind its guest with.
pub(crate) fn in_bridge<R>(
    cx: &mut AsyncEnv<NapiEnv>,
    f: impl FnOnce() -> R,
) -> (R, Option<RuntimeError>) {
    struct Leave(*mut AsyncEnv<NapiEnv>);
    impl Drop for Leave {
        fn drop(&mut self) {
            IN_BRIDGE.set(self.0);
        }
    }
    let _leave = Leave(IN_BRIDGE.replace(cx));
    let result = f();
    (result, take_pending_guest_unwind())
}

/// Hands `f` the import that is in the bridge on this thread, if one is and
/// nothing it called back is using it already.
pub(crate) fn with_import_in_bridge<R>(f: impl FnOnce(&mut AsyncEnv<NapiEnv>) -> R) -> Option<R> {
    let cx = IN_BRIDGE.replace(ptr::null_mut());
    if cx.is_null() {
        return None;
    }
    struct GiveBack(*mut AsyncEnv<NapiEnv>);
    impl Drop for GiveBack {
        fn drop(&mut self) {
            IN_BRIDGE.set(self.0);
        }
    }
    let _give_back = GiveBack(cx);
    // SAFETY: `in_bridge` holds the import's exclusive borrow for as long as
    // the pointer is published, and waits in the bridge call. The pointer was
    // taken out above, so nothing else reaches the import while `f` runs.
    Some(f(unsafe { &mut *cx }))
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
    PENDING_GUEST_UNWIND.with(|pending| {
        pending.borrow_mut().get_or_insert_with(|| err.clone());
    });
    true
}

/// Take the pending error stashed by a callback trampoline, if any. For an
/// import boundary that made no bridge call with [`in_bridge`], which hands
/// the error to one that did.
pub(crate) fn take_pending_guest_unwind() -> Option<RuntimeError> {
    PENDING_GUEST_UNWIND.with(|pending| pending.borrow_mut().take())
}

/// What [`snapi_host_invoke_wasm_callback`] returns in place of a value id
/// when no import of the guest's store is in the bridge, so the guest
/// function did not run. The bridge turns it into a JavaScript error.
///
/// No value id has this value: an id holds its slot index plus one in the
/// upper 20 bits, which is never zero, and here those bits are zero.
/// `kCallbackStoreNotLent` in `napi_bridge_init.cc` is the same constant and
/// checks that against the id layout. A guest that returns this number
/// itself is handled in [`snapi_host_invoke_wasm_callback`].
pub(crate) const CALLBACK_STORE_NOT_LENT: u32 = 1;

/// What a V8 callback needs to reach the guest: a handle to the guest
/// instance's N-API env and the id of its store. Neither changes for the
/// life of a bridge env, so [`install_callback_ctx`] gives each env one when
/// it is created and imports never swap it. The handle is store-free; the
/// store is reached through the import that is in the bridge (see
/// [`in_bridge`]). C++ only ever passes this back as an opaque pointer.
pub(crate) struct CallbackInvocationCtx {
    func_env: FunctionEnv<NapiEnv>,
    /// The store `func_env` lives in. A callback only runs from an import of
    /// that store: none may be in the bridge (JavaScript outside a bridge
    /// call), or another store's, when stores share the thread.
    store_id: usize,
}

/// Why a guest function did not run.
enum Refused {
    /// No import of the context's store is in the bridge.
    Unreachable,
    /// It is not a function, or the callbacks are nested too deep: the value
    /// is what the trampoline returns.
    Result(u32),
}

impl CallbackInvocationCtx {
    /// Takes the store for the slice that finds the guest function
    /// `wasm_fn_ptr` and counts the callback in.
    fn enter(
        &self,
        cx: &mut AsyncEnv<NapiEnv>,
        wasm_fn_ptr: u32,
        what: &str,
    ) -> Result<CachedGuestCallback, Refused> {
        cx.with(|mut env| {
            let mut store = env.as_store_mut();
            if store.objects_mut().id().as_raw().get() != self.store_id {
                return Err(Refused::Unreachable);
            }
            let Some(table) = self.func_env.as_ref(&store).table.clone() else {
                return Err(Refused::Result(0));
            };
            let func = resolve_guest_function(&mut store, &self.func_env, &table, wasm_fn_ptr)
                .ok_or(Refused::Result(0))?;
            // Bound guest↔host callback reentrancy so a runaway recursion
            // cannot overflow the host native stack (an uncatchable SIGSEGV
            // that would take down every co-tenant). Refuse the callback past
            // the limit rather than crash. `leave` pairs with this.
            if !self.func_env.as_mut(&mut store).enter_callback() {
                eprintln!("[{what} trampoline] reentrancy depth limit exceeded");
                return Err(Refused::Result(0));
            }
            Ok(func)
        })
    }

    /// Counts the callback out again.
    fn leave(&self, cx: &mut AsyncEnv<NapiEnv>) {
        cx.with(|mut env| {
            self.func_env
                .as_mut(&mut env.as_store_mut())
                .leave_callback()
        });
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

/// The guest function at `wasm_fn_ptr` in the indirect function table, from
/// the env's cache once it was called.
fn resolve_guest_function(
    store: &mut impl AsStoreMut,
    func_env: &FunctionEnv<NapiEnv>,
    table: &Table,
    wasm_fn_ptr: u32,
) -> Option<CachedGuestCallback> {
    if let Some(func) = func_env.as_ref(store).func_cache.get(&wasm_fn_ptr) {
        return Some(func.clone());
    }
    let Value::FuncRef(Some(func)) = table.get(store, wasm_fn_ptr)? else {
        return None;
    };
    let callback = CachedGuestCallback::new(func, store);
    func_env
        .as_mut(store)
        .func_cache
        .insert(wasm_fn_ptr, callback.clone());
    Some(callback)
}

/// What the trampoline returns for a guest function that ended with `result`.
fn callback_result(result: Result<u32, RuntimeError>, what: &str) -> u32 {
    match result {
        Ok(result) => result,
        Err(err) => {
            if !stash_guest_unwind(&err) {
                eprintln!("[{what} trampoline] error calling guest function: {err}");
            }
            0
        }
    }
}

/// Rust trampoline called from C++ when a V8 callback fires.
/// Dispatches into the WASM guest through the import that is in the bridge.
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

    let dispatched = with_import_in_bridge(|cx| {
        let func = ctx.enter(cx, wasm_fn_ptr, "callback")?;
        let result = func.call(cx, guest_env, callback_arg);
        ctx.leave(cx);
        Ok(callback_result(result, "callback"))
    });

    match dispatched {
        // A guest function that returns the sentinel's number returned no
        // valid value id; report that as the null id, so the bridge does not
        // take it for a call that never ran.
        Some(Ok(CALLBACK_STORE_NOT_LENT)) => 0,
        Some(Ok(result)) | Some(Err(Refused::Result(result))) => result,
        None | Some(Err(Refused::Unreachable)) => {
            // No import of this guest's store is in the bridge, so there is
            // no way back into the guest from here. The bridge throws in the
            // JavaScript that made the call.
            eprintln!("[callback trampoline] no import of the guest's store is in the bridge");
            CALLBACK_STORE_NOT_LENT
        }
    }
}

/// Rust trampoline called from C++ when a guest-registered N-API finalizer
/// fires (during the deferred finalizer drain, which runs inside a guest→bridge
/// import call). Dispatches `finalize(env, data, hint)` through the indirect
/// table.
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

    let dispatched = with_import_in_bridge(|cx| {
        // Same reentrancy bound as the regular callback trampoline: a
        // finalizer may itself create/free values and trigger more callbacks.
        let func = ctx.enter(cx, wasm_fn_ptr, "finalizer")?;
        let result = func.call_finalizer(cx, guest_env, data, hint);
        ctx.leave(cx);
        Ok(callback_result(result, "finalizer"))
    });
    match dispatched {
        Some(Ok(result)) | Some(Err(Refused::Result(result))) => result,
        None | Some(Err(Refused::Unreachable)) => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::guest::drive::run_to_completion;
    use wasmer::{Function, Store, TableType, Type};

    fn napi_env() -> NapiEnv {
        NapiEnv::new(
            crate::budget::ResourceBudget::unlimited(),
            None,
            Default::default(),
            Default::default(),
        )
    }

    /// Whether an import is in the bridge on this thread with nothing using
    /// it, as a callback would find out.
    fn reachable() -> bool {
        with_import_in_bridge(|_| ()).is_some()
    }

    /// Calls `import` as Wasm would call an N-API import and returns what it
    /// returned: a bit per check that held.
    fn call_import(store: &mut Store, import: &Function) -> i32 {
        let results = import.call(store, &[]).expect("the import trapped");
        results[0].unwrap_i32()
    }

    #[test]
    fn an_import_is_reachable_only_during_its_bridge_call() {
        let mut store = Store::default();
        let env = FunctionEnv::new(&mut store, napi_env());
        let import = Function::new_typed_async(
            &mut store,
            &env,
            async |cx: &mut AsyncEnv<NapiEnv>| -> i32 {
                let own: *const AsyncEnv<NapiEnv> = &raw const *cx;
                // The import is in no bridge call: nothing reaches it.
                let mut held = i32::from(!reachable());
                let ((), unwind) = in_bridge(cx, || {
                    for _ in 0..2 {
                        let callback = with_import_in_bridge(|cx| {
                            // A callback gets the import, with its store in
                            // reach, and holds it: nothing reaches it a
                            // second time.
                            cx.with(|_| ());
                            (std::ptr::eq(cx, own), !reachable())
                        });
                        held = (held << 2)
                            | match callback {
                                Some((same, exclusive)) => {
                                    i32::from(same) << 1 | i32::from(exclusive)
                                }
                                None => 0,
                            };
                    }
                });
                // The bridge call is over.
                (held << 2) | i32::from(unwind.is_none()) << 1 | i32::from(!reachable())
            },
        );
        assert_eq!(call_import(&mut store, &import), 0b1_11_11_11);
        assert!(!reachable());
    }

    #[test]
    fn bridge_calls_nest_through_a_callback() {
        let mut store = Store::default();
        let env = FunctionEnv::new(&mut store, napi_env());
        let inner = Function::new_typed_async(
            &mut store,
            &env,
            async |cx: &mut AsyncEnv<NapiEnv>| -> i32 {
                // The callback that called this import holds the outer one.
                let held = !reachable();
                let (found, _) = in_bridge(cx, reachable);
                i32::from(held) << 1 | i32::from(found)
            },
        );
        let outer = Function::new_typed_async(
            &mut store,
            &env,
            async move |cx: &mut AsyncEnv<NapiEnv>| -> i32 {
                let (nested, _) = in_bridge(cx, || {
                    let nested = with_import_in_bridge(|cx| {
                        let nested =
                            run_to_completion(inner.call_async(cx, &[])).unwrap()[0].unwrap_i32();
                        // The inner import's bridge call ended with it: this
                        // callback still holds the outer import.
                        nested << 1 | i32::from(!reachable())
                    });
                    // The callback returned: the outer import is free again.
                    nested.unwrap_or(0) << 1 | i32::from(reachable())
                });
                nested
            },
        );
        assert_eq!(call_import(&mut store, &outer), 0b11_1_1);
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

    /// An import that reports whether a callback of its bridge call reaches
    /// it, and through it the store.
    fn reaching_probe(store: &mut Store) -> Function {
        let env = FunctionEnv::new(store, napi_env());
        Function::new_typed_async(store, &env, async |cx: &mut AsyncEnv<NapiEnv>| -> i32 {
            let (reached, _) = in_bridge(cx, || with_import_in_bridge(|cx| cx.with(|_| ())));
            i32::from(reached.is_some())
        })
    }

    #[test]
    fn an_import_of_an_async_call_is_reached_as_one_of_a_sync_call_is() {
        let mut store = Store::default();
        let import = reaching_probe(&mut store);
        let results = ready(import.call_async(&mut store, &[])).expect("the import trapped");
        assert_eq!(results[0].unwrap_i32(), 1);
    }

    #[test]
    fn an_import_of_a_shared_store_task_is_reached_as_one_of_a_sync_call_is() {
        // How a WASIX v2 machine runs its guests: every task has a handle to
        // its store and takes the store for the slice it runs.
        let mut store = Store::default();
        let import = reaching_probe(&mut store);
        let mut shared = store.into_shared();
        let results = ready(import.call_async(&mut shared, &[])).expect("the import trapped");
        assert_eq!(results[0].unwrap_i32(), 1);
    }

    /// A context for `func_env`, which lives in `store`, and an import of
    /// that env whose bridge call has JavaScript call the guest function at
    /// index 0, as a V8 callback of that context would.
    fn calling_back(
        store: &mut Store,
        func_env: &FunctionEnv<NapiEnv>,
    ) -> (Arc<CallbackInvocationCtx>, Function) {
        let ctx = Arc::new(CallbackInvocationCtx {
            func_env: func_env.clone(),
            store_id: store.objects_mut().id().as_raw().get(),
        });
        let import = call_back_from(store, func_env, &ctx);
        (ctx, import)
    }

    /// An import of `func_env` whose bridge call has JavaScript call the
    /// guest function at index 0 of the context `ctx`.
    fn call_back_from(
        store: &mut Store,
        func_env: &FunctionEnv<NapiEnv>,
        ctx: &Arc<CallbackInvocationCtx>,
    ) -> Function {
        let ctx = Arc::clone(ctx);
        Function::new_typed_async(
            store,
            func_env,
            async move |cx: &mut AsyncEnv<NapiEnv>| -> i32 {
                let ctx = Arc::as_ptr(&ctx).cast_mut().cast::<c_void>();
                let (result, _) = in_bridge(cx, || snapi_host_invoke_wasm_callback(ctx, 0, 0, 0));
                result as i32
            },
        )
    }

    /// An env whose table holds one guest function that returns 17.
    fn env_with_a_getter(store: &mut Store) -> NapiEnv {
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
        let module = wasmer::Module::new(
            &store,
            r#"(module
              (import "host" "slow" (func $slow (result i32)))
              (table (export "table") 1 1 funcref)
              (elem (i32.const 0) $callback)
              (func $callback (param i32 i32) (result i32) call $slow))"#,
        )
        .unwrap();
        let instance = wasmer::Instance::new(
            &mut store,
            &module,
            &wasmer::imports! { "host" => { "slow" => slow } },
        )
        .unwrap();
        let mut env = napi_env();
        env.table = Some(instance.exports.get_table("table").unwrap().clone());
        let func_env = FunctionEnv::new(&mut store, env);
        let (_ctx, import) = calling_back(&mut store, &func_env);
        assert_eq!(call_import(&mut store, &import), 17);
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
    fn a_bridge_call_hands_back_a_stashed_error_and_ends_on_unwind() {
        let mut store = Store::default();
        let env = FunctionEnv::new(&mut store, napi_env());
        let import = Function::new_typed_async(
            &mut store,
            &env,
            async |cx: &mut AsyncEnv<NapiEnv>| -> i32 {
                let exited = RuntimeError::user(Box::new(Exit(7)));
                let (stashed, unwind) = in_bridge(cx, || stash_guest_unwind(&exited));
                let handed_back = unwind
                    .is_some_and(|error| matches!(error.downcast_ref::<Exit>(), Some(Exit(7))));
                // Taken, not copied.
                let taken = take_pending_guest_unwind().is_none();

                // resume_unwind raises a panic without the hook's message.
                let unwound = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    in_bridge(cx, || std::panic::resume_unwind(Box::new(())))
                }))
                .is_err();
                i32::from(stashed) << 4
                    | i32::from(handed_back) << 3
                    | i32::from(taken) << 2
                    | i32::from(unwound) << 1
                    | i32::from(!reachable())
            },
        );
        assert_eq!(call_import(&mut store, &import), 0b11111);
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

    #[test]
    fn a_callback_respects_the_callback_depth_limit() {
        let mut store = Store::default();
        let mut env = env_with_a_getter(&mut store);
        // Simulate the active outer callbacks without building a deep native stack.
        for _ in 0..crate::guest::MAX_CALLBACK_DEPTH {
            assert!(env.enter_callback());
        }
        let func_env = FunctionEnv::new(&mut store, env);
        let (_ctx, import) = calling_back(&mut store, &func_env);
        assert_eq!(call_import(&mut store, &import), 0);
        func_env.as_mut(&mut store).leave_callback();
        assert_eq!(call_import(&mut store, &import), 17);
    }

    #[test]
    fn a_callback_context_only_runs_from_an_import_of_its_own_store() {
        let mut store = Store::default();
        let env = env_with_a_getter(&mut store);
        let func_env = FunctionEnv::new(&mut store, env);
        let (ctx, import) = calling_back(&mut store, &func_env);

        // No import in the bridge, then one of another store: the guest is
        // out of reach.
        let raw = Arc::as_ptr(&ctx).cast_mut().cast::<c_void>();
        assert_eq!(
            snapi_host_invoke_wasm_callback(raw, 0, 0, 0),
            CALLBACK_STORE_NOT_LENT
        );
        let mut other = Store::default();
        let other_env = FunctionEnv::new(&mut other, napi_env());
        let stranger = call_back_from(&mut other, &other_env, &ctx);
        assert_eq!(
            call_import(&mut other, &stranger),
            CALLBACK_STORE_NOT_LENT as i32
        );
        assert_eq!(call_import(&mut store, &import), 17);
    }
}
