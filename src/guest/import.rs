//! Registers the N-API handlers that can run JavaScript.
//!
//! Such a handler holds no store while JavaScript runs. It is an async host
//! function of the engine: it has an `AsyncEnv` where a synchronous one has
//! the store, and takes the store one slice at a time (`AsyncEnv::with`), as
//! whatever JavaScript calls meanwhile does (see `callback`).
//!
//! None of these handlers waits, so none of them is `async`. Each is
//! registered as a function that returns `Deferred::ready`, which the engine
//! calls as it calls a synchronous host function, without a future. A guest
//! function that JavaScript calls may wait, and the callback trampoline
//! waits for that one itself.
//!
//! A slice is for what only the store has. Most of what a handler needs of
//! its guest is not that: which bridge env the call is for, and the guest
//! memory its arguments and results are in. The handler reaches those
//! through [`Cx`] with no slice at all (see [`GuestShared`]).

use std::sync::Arc;

use wasmer::{
    AsStoreMut, AsyncEnv, Deferred, FromToNativeWasmType, Function, FunctionEnv, FunctionEnvMut,
    IntoResult, WasmTypeList,
};

use crate::{NapiEnv, env::GuestShared, snapi::SnapiEnv};

/// What a handler that can run JavaScript has of its call.
pub(crate) struct Cx<'a> {
    /// Where slices of the store come from, and the way back into the guest
    /// for the JavaScript the handler runs.
    pub(crate) async_env: &'a mut AsyncEnv<NapiEnv>,
    /// What the handler reaches of its guest with no slice.
    pub(crate) shared: &'a GuestShared,
}

impl Cx<'_> {
    /// A slice of the store: [`AsyncEnv::with`].
    #[inline]
    pub(crate) fn with<R>(&mut self, f: impl FnOnce(FunctionEnvMut<'_, NapiEnv>) -> R) -> R {
        self.async_env.with(f)
    }

    /// The bridge env the guest's `guest_env` stands for, null if it stands
    /// for none, and whether its heap is tracked against a budget.
    #[inline]
    pub(crate) fn napi_env(&mut self, guest_env: u32) -> (SnapiEnv, bool) {
        match self.shared.single_env(guest_env) {
            Some(found) => found,
            None => self.napi_env_of_store(guest_env),
        }
    }

    /// [`Self::napi_env`] in a slice, for a store with several envs or none.
    #[inline(never)]
    fn napi_env_of_store(&mut self, guest_env: u32) -> (SnapiEnv, bool) {
        self.with(|env| {
            let state = env.data();
            (
                state.resolve_napi_env(guest_env),
                state.heap_tracker_for(guest_env).is_some(),
            )
        })
    }
}

/// An N-API handler that can run JavaScript: a function of the [`Cx`] of its
/// call and of Wasm values, which returns when its call is over.
///
/// `Args` and `Rets` only tell the implementations for different arities
/// apart; [`import`] infers them.
pub(crate) trait Handler<Args, Rets> {
    fn into_import(self, store: &mut impl AsStoreMut, env: &FunctionEnv<NapiEnv>) -> Function;
}

/// The import that runs `handler`.
pub(crate) fn import<H, Args, Rets>(
    store: &mut impl AsStoreMut,
    env: &FunctionEnv<NapiEnv>,
    handler: H,
) -> Function
where
    H: Handler<Args, Rets>,
{
    handler.into_import(store, env)
}

macro_rules! impl_handler {
    ( $( $x:ident ),* ) => {
        // `(A1)` is the type itself, which is what a function of one
        // parameter takes its arguments as.
        #[allow(unused_parens)]
        impl<H, R, Rets, $( $x, )*> Handler<( $( $x, )* ), (R, Rets)> for H
        where
            H: for<'a, 'e> Fn(&'a mut Cx<'e>, $( $x ),*) -> R + 'static,
            R: IntoResult<Rets> + 'static,
            Rets: WasmTypeList + 'static,
            ( $( $x ),* ): WasmTypeList + 'static,
            $( $x: FromToNativeWasmType + 'static, )*
        {
            #[allow(non_snake_case)]
            fn into_import(
                self,
                store: &mut impl AsStoreMut,
                env: &FunctionEnv<NapiEnv>,
            ) -> Function {
                let shared = Arc::clone(&env.as_ref(store).shared);
                Function::new_typed_async(
                    store,
                    env,
                    move |async_env: &mut AsyncEnv<NapiEnv>, $( $x: $x ),*| -> Deferred<R> {
                        let mut cx = Cx { async_env, shared: &shared };
                        Deferred::ready(self(&mut cx, $( $x ),*))
                    },
                )
            }
        }
    };
}

impl_handler!();
impl_handler!(A1);
impl_handler!(A1, A2);
impl_handler!(A1, A2, A3);
impl_handler!(A1, A2, A3, A4);
impl_handler!(A1, A2, A3, A4, A5);
impl_handler!(A1, A2, A3, A4, A5, A6);
impl_handler!(A1, A2, A3, A4, A5, A6, A7);
impl_handler!(A1, A2, A3, A4, A5, A6, A7, A8);
impl_handler!(A1, A2, A3, A4, A5, A6, A7, A8, A9);
impl_handler!(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10);
impl_handler!(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11);
impl_handler!(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12);
impl_handler!(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13);
impl_handler!(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14);
impl_handler!(
    A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15
);
impl_handler!(
    A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15, A16
);
