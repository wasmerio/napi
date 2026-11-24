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

use wasmer::{
    AsStoreMut, AsyncEnv, Deferred, FromToNativeWasmType, Function, FunctionEnv, IntoResult,
    WasmTypeList,
};

use crate::NapiEnv;

/// An N-API handler that can run JavaScript: a function of the `AsyncEnv`
/// of its call and of Wasm values, which returns when its call is over.
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
            H: for<'e> Fn(&'e mut AsyncEnv<NapiEnv>, $( $x ),*) -> R + 'static,
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
                Function::new_typed_async(
                    store,
                    env,
                    move |cx: &mut AsyncEnv<NapiEnv>, $( $x: $x ),*| -> Deferred<R> {
                        Deferred::ready(self(cx, $( $x ),*))
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
