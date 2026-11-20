use std::{
    cell::{Cell, UnsafeCell},
    ffi::c_void,
    sync::Arc,
};

use wasmer::{AsStoreMut, FunctionEnv, FunctionEnvMut, Store, StoreMut, Table, Value};
use wasmer_wasix::WasiError;
use wasmer_wasix::wasmer_wasix_types::wasi::ExitCode;

use super::callback_cache::CachedGuestCallback;
use crate::{NapiEnv, env::CallbackDepth, snapi::SnapiEnv};

/// What the imports running on a thread lend to the callbacks their bridge
/// calls run, and what those callbacks hand back. One thread-local holds
/// both, so that an import reaches them with a single lookup.
struct Lent {
    /// The innermost import on this thread whose bridge call is running, as
    /// a pointer to its own `FunctionEnvMut`: the store it lends. Null while
    /// no import is lending, and while a callback holds the store.
    ///
    /// The import holds `&mut` to that `FunctionEnvMut` for as long as the
    /// pointer is here (see [`lend_store`]), so nothing else can reach it,
    /// and a callback that takes the pointer reborrows from that `&mut`.
    /// This is what `StoreMut::parked` and `Store::with_current` do through
    /// Wasmer's own thread-local stack, at a fraction of the cost: one
    /// pointer written and one restored.
    env: Cell<*mut FunctionEnvMut<'static, NapiEnv>>,
    /// A guest instance exit (`WasiError::Exit`) that trapped out of a guest
    /// callback and must be re-raised through the N-API import layers.
    ///
    /// When the guest calls `_exit`/`proc_exit` — e.g. Node's fatal-exception
    /// path taking `std::_Exit` on the main thread after an unhandled exception
    /// — WASI `proc_exit` unwinds the guest and the [`func.call`] that dispatched
    /// the callback returns a trap carrying [`WasiError::Exit`]. The trampoline
    /// runs inside host V8 (a native C++ frame that cannot carry a Rust trap), so
    /// it cannot re-raise directly. Instead it stashes the exit here; the nearest
    /// enclosing N-API import boundary ([`lend_store`]) takes it back out and
    /// the import returns it as an `Err`, so wasmer re-raises the same exit
    /// through every guest→host layer until the runner
    /// (`bin_factory`/`thread_spawn`) handles it — the standard WASIX
    /// process-exit propagation. Swallowing it (the old behavior) left the host
    /// event loop spinning forever (observed as `test-tls-handshake-exception`
    /// hanging).
    ///
    /// This terminates only the guest instance whose callback exited, never the
    /// host process.
    ///
    /// [`func.call`]: wasmer::Function::call
    exit: Cell<Option<ExitCode>>,
}

thread_local! {
    static LENT: Lent = const {
        Lent {
            env: Cell::new(std::ptr::null_mut()),
            exit: Cell::new(None),
        }
    };
}

/// Puts a lent pointer back when the code that replaced it is done, on
/// return and on unwind.
struct RestoreLent<'a> {
    lent: &'a Lent,
    env: *mut FunctionEnvMut<'static, NapiEnv>,
}

impl Drop for RestoreLent<'_> {
    fn drop(&mut self) {
        self.lent.env.set(self.env);
    }
}

/// Lends `env`'s store for the duration of `f`, so that host V8 callbacks
/// fired inside a bridge call can be dispatched into the wasm guest (see
/// [`snapi_host_invoke_wasm_callback`]) and V8's array-buffer allocator can
/// grow guest memory (see `napi_host_guest_heap_alloc`).
///
/// Returns what `f` returned, and a guest exit that a callback stashed while
/// it ran. The caller must re-raise that exit.
///
/// `f` cannot use `env`: the `&mut` this function holds keeps it out of
/// reach until `f` returns, which is what makes handing it to a callback
/// sound rather than aliasing.
pub(crate) fn lend_store<R>(
    env: &mut FunctionEnvMut<NapiEnv>,
    f: impl FnOnce() -> R,
) -> (R, Option<ExitCode>) {
    LENT.with(|lent| {
        let env = (env as *mut FunctionEnvMut<'_, NapiEnv>).cast();
        // Lends nest: a callback's guest code calls imports that lend in turn.
        let _restore = RestoreLent {
            lent,
            env: lent.env.replace(env),
        };
        let result = f();
        (result, lent.exit.take())
    })
}

/// Runs `f` with the store lent to this thread, if one is: the store of the
/// innermost import inside [`lend_store`], else a store that Wasmer says is
/// parked (`StoreMut::parked`). `None` when neither is.
///
/// The store is not necessarily the caller's: several can share a thread,
/// so check its id before using a handle with it.
pub(crate) fn with_lent_store<R>(f: impl FnOnce(&mut StoreMut<'_>) -> R) -> Option<R> {
    LENT.with(|lent| {
        let env = lent.env.replace(std::ptr::null_mut());
        if env.is_null() {
            return Store::with_current(f);
        }
        // `f` holds the store until it returns: it is not lent again, unless
        // by an import that guest code `f` runs calls.
        let _restore = RestoreLent { lent, env };
        // SAFETY: the import that stored `env` holds `&mut` to this
        // `FunctionEnvMut` and cannot touch it until its bridge call, which
        // this code runs inside, returns. Taking the pointer out above keeps
        // a second borrow from being made while this one lives.
        let env = unsafe { &mut *env };
        let mut store = env.as_store_mut();
        Some(f(&mut store))
    })
}

/// If a guest callback trapped with a WASI process exit, stash the exit code so
/// the enclosing import boundary can re-raise it, and report whether it did.
/// Non-exit traps return `false` and are left for the caller to log.
fn stash_guest_exit(err: &wasmer::RuntimeError) -> bool {
    if let Some(WasiError::Exit(code)) = err.downcast_ref::<WasiError>() {
        LENT.with(|lent| lent.exit.set(Some(*code)));
        return true;
    }
    false
}

/// Take a pending guest instance exit stashed by a callback trampoline, if
/// any. For an import boundary that did not lend its store; [`lend_store`]
/// hands the exit to one that did.
pub(crate) fn take_pending_guest_exit() -> Option<ExitCode> {
    LENT.with(|lent| lent.exit.take())
}

/// What [`snapi_host_invoke_wasm_callback`] returns in place of a value id
/// when the guest's store is not lent to the thread, so the guest function
/// did not run. The bridge turns it into a JavaScript error. Value ids never
/// reach this value: `kCallbackStoreNotLent` in `napi_bridge_init.cc` is the
/// same constant.
pub(crate) const CALLBACK_STORE_NOT_LENT: u32 = u32::MAX;

/// What a V8 callback needs to reach the guest: a handle to the guest
/// instance's N-API env and the id of its store. Neither changes for the
/// life of a bridge env, so [`install_callback_ctx`] gives each env one when
/// it is created and imports never swap it. The handle is store-free; the
/// store itself is picked back up from the thread, to which an import lends
/// it (see [`lend_store`]). C++ only ever passes this back as an opaque
/// pointer.
pub(crate) struct CallbackInvocationCtx {
    func_env: FunctionEnv<NapiEnv>,
    /// The store `func_env` lives in. A callback only runs with that store
    /// lent: no store may be lent (JavaScript outside an import that lends
    /// it), or another store's, when stores share the thread.
    store_id: usize,
    /// The store's callback depth, shared with its `NapiEnv` so that a
    /// callback counts its crossing without looking the env data up.
    depth: CallbackDepth,
    /// The guest functions this env's callbacks called last, so that a
    /// repeated call finds its function without the env data either.
    hot: StoreLocal<HotCallbacks>,
}

/// A value that belongs to one store and is only reached by whoever holds
/// that store's `&mut`, which is what lets it sit behind a shared pointer
/// (the bridge's pointer to a [`CallbackInvocationCtx`]) without a lock.
struct StoreLocal<T>(UnsafeCell<T>);

// SAFETY: every access goes through `get`, whose caller holds the store's
// `&mut`: one thread at a time, so the value is never used from two threads
// at once, only handed from one to the next along with the store.
unsafe impl<T: Send> Sync for StoreLocal<T> {}

impl<T> StoreLocal<T> {
    /// # Safety
    /// The caller holds the `&mut` of the store the value belongs to, and
    /// drops the reference before anything that could call `get` again runs:
    /// in particular before it enters the guest, whose imports can call back.
    #[allow(clippy::mut_from_ref)]
    #[inline]
    unsafe fn get(&self) -> &mut T {
        unsafe { &mut *self.0.get() }
    }
}

/// How many guest functions a callback context keeps at hand. A power of two.
const HOT_CALLBACKS: usize = 64;

/// A direct-mapped cache in front of `NapiEnv::func_cache`, by table index.
/// Two functions that share a slot take turns at it; both stay in the env's
/// cache, which answers whenever this one does not.
struct HotCallbacks(Box<[Option<(u32, CachedGuestCallback)>; HOT_CALLBACKS]>);

impl CallbackInvocationCtx {
    fn new(func_env: FunctionEnv<NapiEnv>, store_id: usize, depth: CallbackDepth) -> Self {
        Self {
            func_env,
            store_id,
            depth,
            hot: StoreLocal(UnsafeCell::new(HotCallbacks(Box::new(std::array::from_fn(
                |_| None,
            ))))),
        }
    }

    /// The lent store, if it is the one this context's env lives in.
    #[inline]
    fn with_store<R>(&self, f: impl FnOnce(&mut StoreMut<'_>) -> R) -> Option<R> {
        with_lent_store(|store| {
            (store.objects_mut().id().as_raw().get() == self.store_id).then(|| f(store))
        })
        .flatten()
    }

    /// The guest function at `wasm_fn_ptr` in the indirect function table.
    /// `None` if the instance has no table or the element is not a function.
    ///
    /// `store` is this context's store ([`Self::with_store`]).
    #[inline]
    fn guest_callback(
        &self,
        store: &mut StoreMut<'_>,
        wasm_fn_ptr: u32,
    ) -> Option<CachedGuestCallback> {
        // SAFETY: the caller holds this context's store, and the reference
        // ends with this function, which runs no guest code: reading the
        // table and the env data does not.
        let hot = unsafe { self.hot.get() };
        let entry = &mut hot.0[wasm_fn_ptr as usize % HOT_CALLBACKS];
        if let Some((index, func)) = entry
            && *index == wasm_fn_ptr
        {
            return Some(func.clone());
        }
        let func = cached_guest_callback(store, &self.func_env, wasm_fn_ptr)?;
        *entry = Some((wasm_fn_ptr, func.clone()));
        Some(func)
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
    let ctx = Arc::new(CallbackInvocationCtx::new(
        env.as_ref(),
        env.objects_mut().id().as_raw().get(),
        env.data().callback_depth(),
    ));
    let ctx_ptr = Arc::as_ptr(&ctx).cast_mut().cast::<c_void>();
    env.data_mut().native_callback_contexts.insert(env_id, ctx);
    unsafe {
        crate::snapi::snapi_bridge_swap_active_callback_ctx(snapi_env, ctx_ptr);
    }
}

/// The guest function at `wasm_fn_ptr` in the indirect function table, from
/// the store's cache or, the first time, from the table. `None` if the
/// instance has no table or the element is not a function.
#[cold]
#[inline(never)]
fn cached_guest_callback(
    store: &mut StoreMut<'_>,
    func_env: &FunctionEnv<NapiEnv>,
    wasm_fn_ptr: u32,
) -> Option<CachedGuestCallback> {
    match func_env.as_ref(store).func_cache.get(&wasm_fn_ptr) {
        Some(func) => Some(func.clone()),
        None => resolve_guest_callback(store, func_env, wasm_fn_ptr),
    }
}

/// The first call of a guest function: reads it from the table and caches it.
#[cold]
#[inline(never)]
fn resolve_guest_callback(
    store: &mut StoreMut<'_>,
    func_env: &FunctionEnv<NapiEnv>,
    wasm_fn_ptr: u32,
) -> Option<CachedGuestCallback> {
    let table: Table = func_env.as_ref(store).table.clone()?;
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

#[inline]
fn call_guest_callback(
    store: &mut StoreMut<'_>,
    ctx: &CallbackInvocationCtx,
    guest_env: u32,
    wasm_fn_ptr: u32,
    callback_arg: u32,
) -> u32 {
    let Some(func) = ctx.guest_callback(store, wasm_fn_ptr) else {
        return 0;
    };
    match func.call(store, guest_env, callback_arg) {
        Ok(result) => result,
        Err(err) => callback_trapped(&err, "[callback trampoline] error calling function"),
    }
}

/// Like [`call_guest_callback`] but passes two data arguments in addition to
/// the guest env handle. Used to dispatch N-API finalizers into the guest,
/// whose ABI is `void finalize(napi_env env, void* data, void* hint)`.
fn call_guest_callback2(
    store: &mut StoreMut<'_>,
    func_env: &FunctionEnv<NapiEnv>,
    guest_env: u32,
    wasm_fn_ptr: u32,
    arg0: u32,
    arg1: u32,
) -> u32 {
    // Finalizers are not hot: straight from the store's cache.
    let Some(func) = cached_guest_callback(store, func_env, wasm_fn_ptr) else {
        return 0;
    };
    match func.call_finalizer(store, guest_env, arg0, arg1) {
        Ok(result) => result,
        Err(err) => callback_trapped(
            &err,
            "[finalizer trampoline] error calling guest finalizer",
        ),
    }
}

/// A guest callback trapped: stashes a guest exit for the import boundary,
/// logs anything else. The callback's result is 0 either way.
#[cold]
#[inline(never)]
fn callback_trapped(err: &wasmer::RuntimeError, what: &str) -> u32 {
    if !stash_guest_exit(err) {
        eprintln!("{what}: {err}");
    }
    0
}

/// The callback at the reentrancy depth limit is refused. As before the
/// counter moved out of the env data, an instance without a table refuses it
/// silently.
#[cold]
#[inline(never)]
fn depth_limit_exceeded(
    store: &mut StoreMut<'_>,
    func_env: &FunctionEnv<NapiEnv>,
    what: &str,
) -> u32 {
    if func_env.as_ref(store).table.is_some() {
        eprintln!("{what} reentrancy depth limit exceeded");
    }
    0
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

    let dispatched = ctx.with_store(|store| {
        // Bound guest↔host callback reentrancy so a runaway recursion cannot
        // overflow the host native stack (an uncatchable SIGSEGV that would take
        // down every co-tenant). Refuse the callback past the limit rather than
        // crash. The increment below always pairs with the decrement, since
        // `call_guest_callback` returns normally on both success and trap.
        if !ctx.depth.enter() {
            return depth_limit_exceeded(store, &ctx.func_env, "[callback trampoline]");
        }
        let result = call_guest_callback(store, ctx, guest_env, wasm_fn_ptr, callback_arg);
        ctx.depth.leave();
        result
    });

    match dispatched {
        Some(result) => result,
        None => store_not_lent(),
    }
}

/// No import of this guest lent its store to the code that entered V8, so
/// there is no way back into the guest from here. The bridge throws in the
/// JavaScript that made the call.
#[cold]
#[inline(never)]
fn store_not_lent() -> u32 {
    eprintln!("[callback trampoline] the guest's store is not lent to this thread");
    CALLBACK_STORE_NOT_LENT
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

    ctx.with_store(|store| {
        // Same reentrancy bound as the regular callback trampoline: a finalizer
        // may itself create/free values and trigger more callbacks.
        if !ctx.depth.enter() {
            return depth_limit_exceeded(store, &ctx.func_env, "[finalizer trampoline]");
        }
        let result =
            call_guest_callback2(store, &ctx.func_env, guest_env, wasm_fn_ptr, data, hint);
        ctx.depth.leave();
        result
    })
    .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use wasmer::{Function, TableType, Type};

    fn napi_env() -> NapiEnv {
        NapiEnv::new(
            crate::budget::ResourceBudget::unlimited(),
            None,
            Default::default(),
            Default::default(),
        )
    }

    fn store_id(store: &mut StoreMut<'_>) -> usize {
        store.objects_mut().id().as_raw().get()
    }

    /// Whether a store is lent to this thread, as a callback would find out.
    fn lent() -> bool {
        with_lent_store(|_| ()).is_some()
    }

    /// Calls `import` as Wasm would call an N-API import and returns what it
    /// returned: a bit per check that held.
    fn call_import(store: &mut Store, import: &Function) -> i32 {
        let results = import.call(store, &[]).expect("the import trapped");
        results[0].unwrap_i32()
    }

    #[test]
    fn an_import_lends_its_store_only_while_it_says_so() {
        let mut store = Store::default();
        let env = FunctionEnv::new(&mut store, napi_env());
        let import = Function::new_typed_with_env(
            &mut store,
            &env,
            |mut env: FunctionEnvMut<NapiEnv>| -> i32 {
                let id = store_id(&mut env.as_store_mut());
                // The import holds its store: nothing is lent.
                let mut held = i32::from(!lent());
                let ((), exit) = lend_store(&mut env, || {
                    for _ in 0..2 {
                        let callback = with_lent_store(|store| {
                            // A callback gets the import's store, and holds
                            // it: nothing lends it a second time.
                            (store_id(store) == id, !lent())
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
                // The import has its store back.
                (held << 2) | i32::from(exit.is_none()) << 1 | i32::from(!lent())
            },
        );
        assert_eq!(call_import(&mut store, &import), 0b1_11_11_11);
        assert!(!lent());
    }

    thread_local! {
        static INNER_IMPORT: std::cell::RefCell<Option<Function>> =
            const { std::cell::RefCell::new(None) };
    }

    #[test]
    fn lends_nest_through_a_callback() {
        let mut store = Store::default();
        let env = FunctionEnv::new(&mut store, napi_env());
        let inner = Function::new_typed_with_env(
            &mut store,
            &env,
            |mut env: FunctionEnvMut<NapiEnv>| -> i32 {
                let id = store_id(&mut env.as_store_mut());
                // The callback that called this import holds the store.
                let held = !lent();
                let (found, _) =
                    lend_store(&mut env, || with_lent_store(|store| store_id(store) == id));
                i32::from(held) << 1 | i32::from(found == Some(true))
            },
        );
        INNER_IMPORT.with(|slot| *slot.borrow_mut() = Some(inner));
        let outer = Function::new_typed_with_env(
            &mut store,
            &env,
            |mut env: FunctionEnvMut<NapiEnv>| -> i32 {
                let (nested, _) = lend_store(&mut env, || {
                    let nested = with_lent_store(|store| {
                        let inner = INNER_IMPORT.with(|slot| slot.borrow().clone().unwrap());
                        let nested = inner.call(store, &[]).unwrap()[0].unwrap_i32();
                        // The inner import's lend ended with it: this
                        // callback still holds the store.
                        nested << 1 | i32::from(!lent())
                    });
                    // The callback returned: the outer import lends again.
                    nested.unwrap_or(0) << 1 | i32::from(lent())
                });
                nested
            },
        );
        assert_eq!(call_import(&mut store, &outer), 0b11_1_1);
        INNER_IMPORT.with(|slot| slot.borrow_mut().take());
    }

    #[test]
    fn a_lend_hands_back_a_stashed_exit_and_ends_on_unwind() {
        let mut store = Store::default();
        let env = FunctionEnv::new(&mut store, napi_env());
        let import = Function::new_typed_with_env(
            &mut store,
            &env,
            |mut env: FunctionEnvMut<NapiEnv>| -> i32 {
                let exited =
                    wasmer::RuntimeError::user(Box::new(WasiError::Exit(ExitCode::from(7u16))));
                let (stashed, exit) = lend_store(&mut env, || stash_guest_exit(&exited));
                let handed_back = matches!(exit, Some(code) if code.raw() == 7);
                // Taken, not copied.
                let taken = take_pending_guest_exit().is_none();

                // resume_unwind raises a panic without the hook's message.
                let unwound = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    lend_store(&mut env, || std::panic::resume_unwind(Box::new(())))
                }))
                .is_err();
                i32::from(stashed) << 4
                    | i32::from(handed_back) << 3
                    | i32::from(taken) << 2
                    | i32::from(unwound) << 1
                    | i32::from(!lent())
            },
        );
        assert_eq!(call_import(&mut store, &import), 0b11111);
    }

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
        let depth = env.callback_depth();
        let func_env = FunctionEnv::new(&mut store, env);
        let mut ctx = CallbackInvocationCtx::new(
            func_env.clone(),
            store.objects_mut().id().as_raw().get(),
            depth,
        );
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
        let depth = env.callback_depth();
        let func_env = FunctionEnv::new(&mut store, env);
        let mut ctx = CallbackInvocationCtx::new(
            func_env,
            store.objects_mut().id().as_raw().get(),
            depth,
        );
        let mut invoke = || snapi_host_invoke_wasm_callback((&raw mut ctx).cast(), 0, 0, 0);
        // No store lent, then another store lent: the guest is out of reach.
        assert_eq!(invoke(), CALLBACK_STORE_NOT_LENT);
        let mut other = Store::default();
        assert_eq!(
            other.as_store_mut().parked(&mut invoke),
            CALLBACK_STORE_NOT_LENT
        );
        assert_eq!(store.as_store_mut().parked(invoke), 17);
    }
}
