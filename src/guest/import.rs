//! Makes the N-API handlers imports that run on their thread's own stack.
//!
//! V8 only runs on the stack its thread started with: it keeps its stack
//! limit there, and throwing, compiling or collecting garbage anywhere else
//! is a fatal error ("Check failed: IsOnCentralStack()"). Wasmer runs a guest
//! of an async call (every guest of a WASIX v2 machine) on a stack of its
//! own, and a synchronous host function on that stack too. An async host
//! function is different: the engine suspends the guest and polls the
//! function's future on the stack of whoever polls the call, the executor.
//! That is the thread's own stack, also for a guest function that JavaScript
//! calls, which the callback trampoline polls from inside V8's frames.
//!
//! So every handler is registered as an async host function whose future
//! runs the handler when it is first polled and is then done. The handlers
//! themselves stay synchronous. A handler never waits: what would make it
//! wait is a guest function called from JavaScript, and the trampoline waits
//! for that one itself.
//!
//! This costs every import a suspension of the guest and the allocations of
//! an async host call: about 140 ns for the simplest import where a
//! synchronous one took 25 ns. An engine that ran the host functions of an
//! async call on its caller's stack would let these be synchronous again.

use wasmer::{
    AsStoreMut, AsyncEnv, FromToNativeWasmType, Function, FunctionEnv, FunctionEnvMut, IntoResult,
    WasmTypeList,
};

use crate::NapiEnv;

/// An N-API handler: a function of the env and of Wasm values.
///
/// `Args` and `Rets` only tell the implementations for different arities
/// apart; [`import`] infers them.
pub(crate) trait Handler<Args, Rets> {
    fn into_import(self, store: &mut impl AsStoreMut, env: &FunctionEnv<NapiEnv>) -> Function;
}

/// The import that runs `handler` on the stack of whoever polls its call.
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
            H: Fn(FunctionEnvMut<'_, NapiEnv>, $( $x ),*) -> R + 'static,
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
                    async move |cx: &mut AsyncEnv<NapiEnv>, $( $x: $x ),*| {
                        cx.with(|env| self(env, $( $x ),*))
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
impl_handler!(
    A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15, A16, A17
);
impl_handler!(
    A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15, A16, A17, A18
);
impl_handler!(
    A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15, A16, A17, A18, A19
);
impl_handler!(
    A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15, A16, A17, A18, A19, A20
);
