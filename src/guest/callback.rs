use std::{cell::Cell, ffi::c_void, ptr, sync::Arc};

use wasmer::sys::vm::TrapCode;
use wasmer::{AsyncEnv, FunctionEnvMut, RuntimeError, Value};

use super::{callback_cache::CachedGuestCallback, import::Cx};
use crate::{NapiEnv, env::GuestShared, snapi::SnapiEnv};

/// What an import that is in JavaScript and the JavaScript it runs leave for
/// each other on their thread. One value, so that each of them looks its
/// thread's up once.
struct InJavaScript {
    /// The call of the import that is in JavaScript on this thread, while
    /// JavaScript can enter the guest through it. See [`enter_javascript`].
    cx: Cell<*mut Cx<'static>>,
    /// The error that ended a guest callback and that its guest must unwind
    /// with: the exit of its process, an abort by the host. The trampoline
    /// cannot propagate an error through V8's native frames. The enclosing
    /// N-API import retrieves it and returns it to Wasmer.
    pending_unwind: Cell<Option<RuntimeError>>,
}

thread_local! {
    static IN_JAVASCRIPT: InJavaScript = const {
        InJavaScript {
            cx: Cell::new(ptr::null_mut()),
            pending_unwind: Cell::new(None),
        }
    };
}

/// Puts a thread's [`InJavaScript::cx`] back when dropped, also on an unwind.
struct PutBack<'a> {
    thread: &'a InJavaScript,
    cx: *mut Cx<'static>,
}

impl Drop for PutBack<'_> {
    fn drop(&mut self) {
        self.thread.cx.set(self.cx);
    }
}

/// Runs `f`, a bridge call that may run JavaScript, with `cx` as the way
/// back into the guest. Returns any error the bridge call must unwind its
/// guest with.
///
/// The import holds no store while `f` runs, so there is none to lend. What
/// V8 calls meanwhile (a guest function, a guest finalizer, the array-buffer
/// allocator) reaches the store as the import itself does, with
/// [`Cx::with`], and calls the guest with `cx`. The engine decides at each
/// of those whether the store can be had.
pub(crate) fn enter_javascript<R>(
    cx: &mut Cx<'_>,
    f: impl FnOnce() -> R,
) -> (R, Option<RuntimeError>) {
    IN_JAVASCRIPT.with(|thread| {
        let result = {
            // Only the pointer's type outlives `cx`: it is taken out again
            // before this returns.
            let cx = ptr::from_mut(cx).cast::<Cx<'static>>();
            let _leave = PutBack {
                thread,
                cx: thread.cx.replace(cx),
            };
            f()
        };
        (result, thread.pending_unwind.take())
    })
}

/// Runs `f` with the call of the import that is in JavaScript on this thread
/// (see [`enter_javascript`]). `None` if JavaScript cannot enter a guest from
/// here: it runs outside such an import, or inside a guest function that it
/// called, whose own imports are the way in from there.
///
/// Nothing says the import is of the guest the caller expects; a thread may
/// run several guests.
pub(crate) fn from_javascript<R>(f: impl FnOnce(&mut Cx<'_>) -> R) -> Option<R> {
    IN_JAVASCRIPT.with(|thread| {
        // Taken for as long as `f` runs. A guest function `f` calls has the
        // store while one of its synchronous imports runs, where `cx.with`
        // would find none and panic: JavaScript that such an import runs
        // finds nothing here, and its guest functions are refused.
        let taken = PutBack {
            thread,
            cx: thread.cx.replace(ptr::null_mut()),
        };
        // SAFETY: the import that put its call here is beneath this frame,
        // in the bridge call that led here. It does not use the call until
        // that bridge call returns, and takes it back from here before it
        // does.
        let cx = unsafe { taken.cx.as_mut() }?;
        Some(f(cx))
    })
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
    IN_JAVASCRIPT.with(|thread| {
        let first = thread.pending_unwind.take();
        thread
            .pending_unwind
            .set(Some(first.unwrap_or_else(|| err.clone())));
    });
    true
}

/// Take the pending error stashed by a callback trampoline, if any. For an
/// import boundary that did not enter JavaScript; [`enter_javascript`] hands
/// the error to one that did.
pub(crate) fn take_pending_guest_unwind() -> Option<RuntimeError> {
    IN_JAVASCRIPT.with(|thread| thread.pending_unwind.take())
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

/// What a V8 callback needs to know of the guest it belongs to: which one it
/// is. That does not change for the life of a bridge env, so
/// [`install_callback_ctx`] gives each env one when it is created. It holds
/// no store and no way to one; that is the call of the import that is in
/// JavaScript (see [`from_javascript`]). C++ only ever passes this back as
/// an opaque pointer.
#[derive(Clone, Copy)]
pub(crate) struct CallbackInvocationCtx {
    /// [`GuestShared::id`] of the guest. A callback only runs through an
    /// import of that guest: guests can share a thread.
    guest: u64,
}

/// Why a trampoline does not call the guest function it was asked for.
enum Refused {
    /// JavaScript cannot enter this guest from where it runs.
    OutOfReach,
    /// The function does not exist, or the guest is too deep in callbacks.
    /// The trampoline returns the null id.
    NotCalled,
}

impl CallbackInvocationCtx {
    /// The context of the guest that `guest` is of.
    pub(crate) fn of(guest: &GuestShared) -> Self {
        Self { guest: guest.id() }
    }

    /// Calls the guest function at `wasm_fn_ptr` through the import that is
    /// in JavaScript, and returns what `call` did, or 0 if the function ended
    /// with an error.
    ///
    /// Whose import that is and how deep its guest is in callbacks are not
    /// the store's to say, so the one slice this takes of the store is for
    /// the function itself.
    fn dispatch(
        self,
        wasm_fn_ptr: u32,
        trampoline: &str,
        call: impl FnOnce(&CachedGuestCallback, &mut AsyncEnv<NapiEnv>) -> Result<u32, RuntimeError>,
    ) -> Result<u32, Refused> {
        from_javascript(|cx| {
            if cx.shared.id() != self.guest {
                return Err(Refused::OutOfReach);
            }
            // Bound guest↔host callback reentrancy so a runaway recursion
            // cannot overflow the host native stack (an uncatchable SIGSEGV
            // that would take down every co-tenant). Refuse the callback past
            // the limit rather than crash.
            if !cx.shared.enter_callback() {
                eprintln!("[{trampoline} trampoline] reentrancy depth limit exceeded");
                return Err(Refused::NotCalled);
            }
            let result = match cx.with(|env| guest_function(env, wasm_fn_ptr)) {
                Some(func) => Ok(call(&func, cx.async_env).unwrap_or_else(|err| {
                    if !stash_guest_unwind(&err) {
                        eprintln!("[{trampoline} trampoline] error calling guest function: {err}");
                    }
                    0
                })),
                None => Err(Refused::NotCalled),
            };
            // The guest function returned, normally or with an error, or
            // there was none to call.
            cx.shared.leave_callback();
            result
        })
        .unwrap_or(Err(Refused::OutOfReach))
    }
}

/// The function at `wasm_fn_ptr` of the guest's table, if it has one. `env`
/// is of the import that is in JavaScript, which is the guest's own.
fn guest_function(
    mut env: FunctionEnvMut<'_, NapiEnv>,
    wasm_fn_ptr: u32,
) -> Option<CachedGuestCallback> {
    if let Some(func) = env.data_mut().func_cache.get(wasm_fn_ptr) {
        return Some(func);
    }
    let table = env.data().table.clone()?;
    let func = match table.get(&mut env, wasm_fn_ptr) {
        Some(Value::FuncRef(Some(func))) => CachedGuestCallback::new(func, &env),
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
    let ctx = Arc::new(CallbackInvocationCtx::of(&env.data().shared));
    let ctx_ptr = Arc::as_ptr(&ctx).cast_mut().cast::<c_void>();
    env.data_mut().native_callback_contexts.insert(env_id, ctx);
    unsafe {
        crate::snapi::snapi_bridge_swap_active_callback_ctx(snapi_env, ctx_ptr);
    }
}

/// Rust trampoline called from C++ when a V8 callback fires. Enters the
/// guest through the import that is in JavaScript and calls the guest
/// function.
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
    // Copied: the guest function can release the env, and the context with it.
    let ctx = unsafe { *(callback_ctx as *const CallbackInvocationCtx) };

    match ctx.dispatch(wasm_fn_ptr, "callback", |func, cx| {
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
            // there is no way back into the guest. The bridge throws in the
            // JavaScript that made the call.
            eprintln!("[callback trampoline] the guest is out of reach of this call");
            CALLBACK_GUEST_OUT_OF_REACH
        }
    }
}

/// Rust trampoline called from C++ when a guest-registered N-API finalizer
/// fires (during the deferred finalizer drain, which runs inside a guest→bridge
/// import call). Enters the guest through that import and dispatches
/// `finalize(env, data, hint)` through the indirect table. The guest's ABI is
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
    let ctx = unsafe { *(callback_ctx as *const CallbackInvocationCtx) };

    // A finalizer may itself create and free values and trigger more
    // callbacks: it has the reentrancy bound of the callback trampoline.
    ctx.dispatch(wasm_fn_ptr, "finalizer", |func, cx| {
        func.call_finalizer(cx, guest_env, data, hint)
    })
    .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::super::{drive::run_to_completion, import::import};
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

    /// Whether JavaScript that ran here could enter a guest.
    fn in_reach() -> bool {
        from_javascript(|_| ()).is_some()
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
    fn callback_ctx(store: &mut Store, func_env: &FunctionEnv<NapiEnv>) -> *mut c_void {
        let ctx = CallbackInvocationCtx::of(&func_env.as_ref(store).shared);
        ptr::from_mut(Box::leak(Box::new(ctx))).cast()
    }

    /// Calls the guest function at index 0 of `ctx` as the bridge does when
    /// JavaScript that an import of `env` runs calls it.
    fn call_from_javascript(
        store: &mut Store,
        env: &FunctionEnv<NapiEnv>,
        ctx: *mut c_void,
    ) -> u32 {
        let import = import(store, env, move |cx: &mut Cx<'_>| -> i32 {
            let (result, _) =
                enter_javascript(cx, || snapi_host_invoke_wasm_callback(ctx, 0, 0, 0));
            result as i32
        });
        run(store, import) as u32
    }

    #[test]
    fn an_import_is_the_way_into_its_guest_only_while_it_is_in_javascript() {
        let mut store = Store::default();
        let env = FunctionEnv::new(&mut store, napi_env());
        let import = import(&mut store, &env, |cx: &mut Cx<'_>| -> i32 {
            let id = store_of(cx);
            // The import is not in JavaScript: nothing leads to its guest.
            let mut checks = i32::from(!in_reach());
            let ((), unwind) = enter_javascript(cx, || {
                for _ in 0..2 {
                    let callback = from_javascript(|cx| {
                        // A callback reaches the import's store, and is
                        // the only one that does until it returns.
                        (store_of(cx) == id, !in_reach())
                    });
                    checks = (checks << 2)
                        | match callback {
                            Some((same, alone)) => i32::from(same) << 1 | i32::from(alone),
                            None => 0,
                        };
                }
            });
            // Back from JavaScript: nothing leads to the guest again.
            (checks << 2) | i32::from(unwind.is_none()) << 1 | i32::from(!in_reach())
        });
        assert_eq!(run(&mut store, import), 0b1_11_11_11);
        assert!(!in_reach());
    }

    #[test]
    fn imports_nest_through_a_callback() {
        let mut store = Store::default();
        let env = FunctionEnv::new(&mut store, napi_env());
        let inner = import(&mut store, &env, |cx: &mut Cx<'_>| -> i32 {
            let id = store_of(cx);
            // The callback that called this import's guest has the outer
            // import's env.
            let alone = !in_reach();
            let (found, _) = enter_javascript(cx, || from_javascript(|cx| store_of(cx) == id));
            i32::from(alone) << 1 | i32::from(found == Some(true))
        });
        let guest = guest_calling(&mut store, inner);
        let outer = import(&mut store, &env, move |cx: &mut Cx<'_>| -> i32 {
            let (nested, _) = enter_javascript(cx, || {
                let nested = from_javascript(|cx| {
                    // As the trampoline does: a guest function, called
                    // with the env of the import that is in JavaScript.
                    let nested = run_to_completion(guest.call_async(cx.async_env, &[])).unwrap()[0]
                        .unwrap_i32();
                    // The inner import left JavaScript when it returned:
                    // this callback still has the outer import's env.
                    nested << 1 | i32::from(!in_reach())
                });
                // The callback returned: the outer import is the way in
                // again.
                nested.unwrap_or(0) << 1 | i32::from(in_reach())
            });
            nested
        });
        // One bit per check, in the order they are made: alone, found, still
        // taken, in reach again.
        assert_eq!(run(&mut store, outer), 0b1111);
    }

    #[test]
    fn a_synchronous_import_of_a_called_guest_function_is_out_of_reach() {
        // A synchronous import holds the store: JavaScript that it runs
        // cannot enter the guest, and must find that out without asking the
        // engine for a store it would refuse.
        let mut store = Store::default();
        let env = FunctionEnv::new(&mut store, napi_env());
        let synchronous =
            Function::new_typed_with_env(&mut store, &env, |_: FunctionEnvMut<NapiEnv>| -> i32 {
                i32::from(in_reach())
            });
        let guest = guest_calling(&mut store, synchronous);
        let import = import(&mut store, &env, move |cx: &mut Cx<'_>| -> i32 {
            let (reached, _) = enter_javascript(cx, || {
                from_javascript(|cx| {
                    run_to_completion(guest.call_async(cx.async_env, &[])).unwrap()[0].unwrap_i32()
                })
            });
            // Then the import can use its store again.
            reached.unwrap_or(1) << 1 | i32::from(store_of(cx) != 0)
        });
        assert_eq!(run(&mut store, import), 0b01);
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

    /// A guest whose import reports whether JavaScript it runs can take a
    /// slice of the store.
    fn reach_probe(store: &mut Store) -> Function {
        let env = FunctionEnv::new(store, napi_env());
        let import = import(store, &env, |cx: &mut Cx<'_>| -> i32 {
            let id = store_of(cx);
            let (reached, _) = enter_javascript(cx, || from_javascript(|cx| store_of(cx) == id));
            i32::from(reached == Some(true))
        });
        guest_calling(store, import)
    }

    #[test]
    fn javascript_enters_the_guest_however_the_guest_was_called() {
        // A synchronous call.
        let mut store = Store::default();
        let guest = reach_probe(&mut store);
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
        let ctx = callback_ctx(&mut store, &func_env);
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
    fn an_import_gets_a_stashed_error_back_and_leaves_javascript_on_unwind() {
        let mut store = Store::default();
        let env = FunctionEnv::new(&mut store, napi_env());
        let import = import(&mut store, &env, |cx: &mut Cx<'_>| -> i32 {
            let exited = RuntimeError::user(Box::new(Exit(7)));
            let (stashed, unwind) = enter_javascript(cx, || stash_guest_unwind(&exited));
            let handed_back =
                unwind.is_some_and(|error| matches!(error.downcast_ref::<Exit>(), Some(Exit(7))));
            // Taken, not copied.
            let taken = take_pending_guest_unwind().is_none();

            // resume_unwind raises a panic without the hook's message.
            let unwound = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                enter_javascript(cx, || std::panic::resume_unwind(Box::new(())))
            }))
            .is_err();
            i32::from(stashed) << 4
                | i32::from(handed_back) << 3
                | i32::from(taken) << 2
                | i32::from(unwound) << 1
                | i32::from(!in_reach())
        });
        assert_eq!(run(&mut store, import), 0b11111);
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
        let env = env_with_a_guest_function(&mut store);
        // Simulate the active outer callbacks without building a deep native stack.
        let guest = Arc::clone(&env.shared);
        for _ in 0..crate::guest::MAX_CALLBACK_DEPTH {
            assert!(guest.enter_callback());
        }
        let func_env = FunctionEnv::new(&mut store, env);
        let ctx = callback_ctx(&mut store, &func_env);
        assert_eq!(call_from_javascript(&mut store, &func_env, ctx), 0);
        guest.leave_callback();
        assert_eq!(call_from_javascript(&mut store, &func_env, ctx), 17);
        // Each call gave its level back.
        assert_eq!(call_from_javascript(&mut store, &func_env, ctx), 17);
    }

    #[test]
    fn a_callback_context_only_runs_through_an_import_of_its_own_store() {
        let mut store = Store::default();
        let env = env_with_a_guest_function(&mut store);
        let func_env = FunctionEnv::new(&mut store, env);
        let ctx = callback_ctx(&mut store, &func_env);
        // No import in JavaScript, then one of another store: the guest is
        // out of reach.
        assert_eq!(
            snapi_host_invoke_wasm_callback(ctx, 0, 0, 0),
            CALLBACK_GUEST_OUT_OF_REACH
        );
        let mut other = Store::default();
        let other_env = FunctionEnv::new(&mut other, napi_env());
        assert_eq!(
            call_from_javascript(&mut other, &other_env, ctx),
            CALLBACK_GUEST_OUT_OF_REACH
        );
        assert_eq!(call_from_javascript(&mut store, &func_env, ctx), 17);
    }
}
