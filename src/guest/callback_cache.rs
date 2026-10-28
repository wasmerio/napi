//! Validated native callback dispatch, with generic compatibility fallback.
use wasmer::{AsStoreMut, AsStoreRef, Function, RuntimeError, TypedFunction, Value};

#[derive(Clone)]
pub(crate) struct CachedGuestCallback {
    function: Function,
    napi_callback: Option<TypedFunction<(i32, i32), i32>>,
}

impl CachedGuestCallback {
    pub(crate) fn new(function: Function, store: &impl AsStoreRef) -> Self {
        // Validate once, through Wasmer's public API. Noncanonical signatures
        // (including interrupt callbacks returning void) retain generic dispatch.
        let napi_callback = function.typed::<(i32, i32), i32>(store).ok();
        Self {
            function,
            napi_callback,
        }
    }

    // Guest handles and pointers are u32; the wasm signature carries them as i32.
    pub(crate) fn call(
        &self,
        store: &mut impl AsStoreMut,
        env: u32,
        info: u32,
    ) -> Result<u32, RuntimeError> {
        if let Some(callback) = &self.napi_callback {
            // TypedFunction retains Wasmer's store, trap, and on_called guards;
            // only per-call signature validation and heap argument/result arrays
            // are removed. Never jump directly into compiled Wasm code.
            return callback
                .call(store, env as i32, info as i32)
                .map(|value| value as u32);
        }
        self.function
            .call(store, &[Value::I32(env as i32), Value::I32(info as i32)])
            .map(|values| result(&values))
    }

    pub(crate) fn call_finalizer(
        &self,
        store: &mut impl AsStoreMut,
        env: u32,
        data: u32,
        hint: u32,
    ) -> Result<u32, RuntimeError> {
        self.function
            .call(
                store,
                &[
                    Value::I32(env as i32),
                    Value::I32(data as i32),
                    Value::I32(hint as i32),
                ],
            )
            .map(|values| result(&values))
    }

    #[cfg(test)]
    pub(crate) fn is_typed(&self) -> bool {
        self.napi_callback.is_some()
    }
}

fn result(values: &[Value]) -> u32 {
    match values.first() {
        Some(Value::I32(value)) => *value as u32,
        Some(Value::I64(value)) => *value as u32,
        _ => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::CachedGuestCallback;
    use wasmer::{
        AsStoreMut, Function, FunctionEnv, FunctionEnvMut, Instance, Module, Store, imports,
    };
    use wasmer_wasix::WasiError;
    use wasmer_wasix::wasmer_wasix_types::wasi::ExitCode;

    fn instance(store: &mut Store, wat: &str) -> Instance {
        let module = Module::new(store, wat::parse_str(wat).unwrap()).unwrap();
        Instance::new(store, &module, &imports! {}).unwrap()
    }

    fn fixtures(store: &mut Store) -> Instance {
        instance(
            store,
            r#"(module
              (func (export "add") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add)
              (func (export "void") (param i32 i32))
              (func (export "wide") (param i32 i32) (result i64) i64.const 4294967298)
              (func (export "trap") (param i32 i32) (result i32) unreachable)
              (func (export "finalize") (param i32 i32 i32))
              (func (export "finalize_result") (param i32 i32 i32) (result i32) local.get 2))"#,
        )
    }

    fn cached(store: &Store, instance: &Instance, name: &str) -> CachedGuestCallback {
        CachedGuestCallback::new(instance.exports.get_function(name).unwrap().clone(), store)
    }

    #[test]
    fn napi_callback_is_typed_and_keeps_pointer_bits() {
        let mut store = Store::default();
        let instance = fixtures(&mut store);
        let callback = cached(&store, &instance, "add");
        assert!(callback.is_typed());
        assert_eq!(callback.call(&mut store, 10, 20).unwrap(), 30);
        assert_eq!(
            callback.call(&mut store, 0, 0xffff_ffff).unwrap(),
            0xffff_ffff
        );
    }

    #[test]
    fn void_callback_uses_generic_dispatch() {
        let mut store = Store::default();
        let instance = fixtures(&mut store);
        let callback = cached(&store, &instance, "void");
        assert!(!callback.is_typed());
        assert_eq!(callback.call(&mut store, 1, 2).unwrap(), 0);
    }

    #[test]
    fn i64_result_uses_generic_dispatch() {
        let mut store = Store::default();
        let instance = fixtures(&mut store);
        let callback = cached(&store, &instance, "wide");
        assert!(!callback.is_typed());
        assert_eq!(callback.call(&mut store, 1, 2).unwrap(), 2);
    }

    #[test]
    fn trap_is_returned_and_store_stays_usable() {
        let mut store = Store::default();
        let instance = fixtures(&mut store);
        assert!(
            cached(&store, &instance, "trap")
                .call(&mut store, 1, 2)
                .is_err()
        );
        assert_eq!(
            cached(&store, &instance, "add")
                .call(&mut store, 1, 2)
                .unwrap(),
            3
        );
    }

    #[test]
    fn finalizer_keeps_generic_dispatch() {
        let mut store = Store::default();
        let instance = fixtures(&mut store);
        let finalize = cached(&store, &instance, "finalize");
        assert_eq!(finalize.call_finalizer(&mut store, 1, 2, 3).unwrap(), 0);
        let finalize_result = cached(&store, &instance, "finalize_result");
        assert_eq!(
            finalize_result
                .call_finalizer(&mut store, 1, 2, 0xffff_ffff)
                .unwrap(),
            0xffff_ffff
        );
        // A two-argument function called as a finalizer fails, as before.
        let add = cached(&store, &instance, "add");
        assert!(add.call_finalizer(&mut store, 1, 2, 3).is_err());
    }

    #[test]
    fn other_parameter_types_use_generic_dispatch() {
        let mut store = Store::default();
        let instance = instance(
            &mut store,
            r#"(module
              (func (export "one") (param i32) (result i32) local.get 0)
              (func (export "wide_params") (param i64 i64) (result i64) local.get 0))"#,
        );
        // Both fail the generic signature check on every call, as before.
        for name in ["one", "wide_params"] {
            let callback = cached(&store, &instance, name);
            assert!(!callback.is_typed());
            assert!(callback.call(&mut store, 1, 2).is_err());
        }
    }

    #[test]
    fn guest_exit_stays_a_wasi_error_through_the_typed_path() {
        let mut store = Store::default();
        let exit = Function::new_typed(&mut store, || -> Result<i32, WasiError> {
            Err(WasiError::Exit(ExitCode::from(7u16)))
        });
        let module = Module::new(
            &store,
            wat::parse_str(
                r#"(module
                  (import "wasi" "exit" (func $exit (result i32)))
                  (func (export "callback") (param i32 i32) (result i32) call $exit))"#,
            )
            .unwrap(),
        )
        .unwrap();
        let instance = Instance::new(
            &mut store,
            &module,
            &imports! { "wasi" => { "exit" => exit } },
        )
        .unwrap();
        let callback = cached(&store, &instance, "callback");
        assert!(callback.is_typed());
        let err = callback.call(&mut store, 1, 2).unwrap_err();
        assert!(matches!(
            err.downcast_ref::<WasiError>(),
            Some(WasiError::Exit(code)) if code.raw() == 7
        ));
    }

    #[test]
    fn typed_callback_reenters_a_parked_store() {
        let mut store = Store::default();
        let instance = fixtures(&mut store);
        let inner = cached(&store, &instance, "add");
        let env = FunctionEnv::new(&mut store, inner);
        let reenter = Function::new_typed_with_env(
            &mut store,
            &env,
            |mut env: FunctionEnvMut<CachedGuestCallback>, a: i32, b: i32| -> i32 {
                let callback = env.data().clone();
                env.as_store_mut().parked(|| {
                    Store::with_current(|store| {
                        callback.call(store, a as u32, b as u32).unwrap() as i32
                    })
                    .unwrap()
                })
            },
        );
        let module = Module::new(
            &store,
            wat::parse_str(
                r#"(module
                  (import "host" "reenter" (func $reenter (param i32 i32) (result i32)))
                  (func (export "outer") (param i32 i32) (result i32)
                    local.get 0 local.get 1 call $reenter))"#,
            )
            .unwrap(),
        )
        .unwrap();
        let outer = Instance::new(
            &mut store,
            &module,
            &imports! { "host" => { "reenter" => reenter } },
        )
        .unwrap();
        assert_eq!(
            cached(&store, &outer, "outer")
                .call(&mut store, 19, 23)
                .unwrap(),
            42
        );
    }
}
