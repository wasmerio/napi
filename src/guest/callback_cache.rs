//! Validated native callback dispatch, with generic compatibility fallback.
use std::collections::HashMap;

use wasmer::{AsStoreRef, AsyncCallStore, Function, RuntimeError, TypedFunction, Value};

use super::drive::run_to_completion;

/// How many of the guest functions called last are found without hashing.
const RECENT: usize = 256;

/// The guest functions JavaScript has called, by their index in the guest's
/// `__indirect_function_table`.
///
/// `Function::from_vm_funcref` (invoked by `Table::get`) unconditionally
/// appends a new entry to the store's function arena on every call with no
/// dedup, so re-resolving the same index on every guest→host→guest callback
/// would leak memory unboundedly. `Function` clones are cheap (a store
/// handle), so each index is resolved once and kept.
///
/// The guest chooses the indices, so the map that keeps them all hashes them
/// with SipHash, which is too much to pay on every callback. The functions
/// called last are therefore also in an array, each in the slot its index
/// alone decides. A guest that makes its indices share slots gets the cost
/// of the map back, and no more.
#[derive(Default)]
pub(crate) struct GuestCallbacks {
    /// Empty until the first callback, then `RECENT` slots.
    recent: Vec<Option<(u32, CachedGuestCallback)>>,
    all: HashMap<u32, CachedGuestCallback>,
}

impl GuestCallbacks {
    /// The function resolved for `index`, if one was.
    #[inline]
    pub(crate) fn get(&mut self, index: u32) -> Option<CachedGuestCallback> {
        if let Some(Some((recent, function))) = self.recent.get(index as usize % RECENT)
            && *recent == index
        {
            return Some(function.clone());
        }
        let function = self.all.get(&index)?.clone();
        self.recent[index as usize % RECENT] = Some((index, function.clone()));
        Some(function)
    }

    /// Keeps `function` as the one resolved for `index`.
    pub(crate) fn insert(&mut self, index: u32, function: CachedGuestCallback) {
        if self.recent.is_empty() {
            self.recent.resize_with(RECENT, || None);
        }
        self.recent[index as usize % RECENT] = Some((index, function.clone()));
        self.all.insert(index, function);
    }
}

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
    //
    // The call is an async one that this thread stays in until it is done
    // (see `drive`): the guest may wait in one of its runtime's async imports,
    // and V8's frames below this one cannot be suspended.
    //
    // `store` is what the engine runs an async call on: an `AsyncEnv` of
    // the guest, under an import that the engine is polling, or a store
    // itself.
    //
    // `None` if the thread is too deep in such calls for one more (see
    // `drive`): the function did not run.
    pub(crate) fn call<S: AsyncCallStore>(
        &self,
        store: &mut S,
        env: u32,
        info: u32,
    ) -> Option<Result<u32, RuntimeError>> {
        if let Some(callback) = &self.napi_callback {
            // TypedFunction retains Wasmer's store, trap, and on_called guards;
            // only per-call signature validation is removed. Never jump
            // directly into compiled Wasm code.
            let called = run_to_completion(callback.call_async(store, env as i32, info as i32))?;
            return Some(called.map(|value| value as u32));
        }
        let params = [Value::I32(env as i32), Value::I32(info as i32)];
        let called = run_to_completion(self.function.call_async(store, &params))?;
        Some(called.map(|values| result(&values)))
    }

    /// As [`Self::call`], for a finalizer.
    pub(crate) fn call_finalizer<S: AsyncCallStore>(
        &self,
        store: &mut S,
        env: u32,
        data: u32,
        hint: u32,
    ) -> Option<Result<u32, RuntimeError>> {
        let params = [
            Value::I32(env as i32),
            Value::I32(data as i32),
            Value::I32(hint as i32),
        ];
        let called = run_to_completion(self.function.call_async(store, &params))?;
        Some(called.map(|values| result(&values)))
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
    use super::{CachedGuestCallback, GuestCallbacks, RECENT};
    use wasmer::{
        AsStoreMut, AsyncEnv, Function, FunctionEnv, Instance, Module, RuntimeError, Store, imports,
    };

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
        assert_eq!(
            callback
                .call(&mut store.as_store_mut(), 10, 20)
                .unwrap()
                .unwrap(),
            30
        );
        assert_eq!(
            callback
                .call(&mut store.as_store_mut(), 0, 0xffff_ffff)
                .unwrap()
                .unwrap(),
            0xffff_ffff
        );
    }

    #[test]
    fn void_callback_uses_generic_dispatch() {
        let mut store = Store::default();
        let instance = fixtures(&mut store);
        let callback = cached(&store, &instance, "void");
        assert!(!callback.is_typed());
        assert_eq!(
            callback
                .call(&mut store.as_store_mut(), 1, 2)
                .unwrap()
                .unwrap(),
            0
        );
    }

    #[test]
    fn i64_result_uses_generic_dispatch() {
        let mut store = Store::default();
        let instance = fixtures(&mut store);
        let callback = cached(&store, &instance, "wide");
        assert!(!callback.is_typed());
        assert_eq!(
            callback
                .call(&mut store.as_store_mut(), 1, 2)
                .unwrap()
                .unwrap(),
            2
        );
    }

    #[test]
    fn trap_is_returned_and_store_stays_usable() {
        let mut store = Store::default();
        let instance = fixtures(&mut store);
        assert!(
            cached(&store, &instance, "trap")
                .call(&mut store.as_store_mut(), 1, 2)
                .unwrap()
                .is_err()
        );
        assert_eq!(
            cached(&store, &instance, "add")
                .call(&mut store.as_store_mut(), 1, 2)
                .unwrap()
                .unwrap(),
            3
        );
    }

    #[test]
    fn finalizer_keeps_generic_dispatch() {
        let mut store = Store::default();
        let instance = fixtures(&mut store);
        let finalize = cached(&store, &instance, "finalize");
        assert_eq!(
            finalize
                .call_finalizer(&mut store.as_store_mut(), 1, 2, 3)
                .unwrap()
                .unwrap(),
            0
        );
        let finalize_result = cached(&store, &instance, "finalize_result");
        assert_eq!(
            finalize_result
                .call_finalizer(&mut store.as_store_mut(), 1, 2, 0xffff_ffff)
                .unwrap()
                .unwrap(),
            0xffff_ffff
        );
        // A two-argument function called as a finalizer fails, as before.
        let add = cached(&store, &instance, "add");
        assert!(
            add.call_finalizer(&mut store.as_store_mut(), 1, 2, 3)
                .unwrap()
                .is_err()
        );
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
            assert!(
                callback
                    .call(&mut store.as_store_mut(), 1, 2)
                    .unwrap()
                    .is_err()
            );
        }
    }

    #[test]
    fn a_host_error_keeps_its_type_through_the_typed_path() {
        #[derive(Debug)]
        struct Exit(i32);
        impl std::fmt::Display for Exit {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                write!(f, "exit {}", self.0)
            }
        }
        impl std::error::Error for Exit {}

        let mut store = Store::default();
        let exit = Function::new_typed(&mut store, || -> Result<i32, RuntimeError> {
            Err(RuntimeError::user(Box::new(Exit(7))))
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
        let err = callback
            .call(&mut store.as_store_mut(), 1, 2)
            .unwrap()
            .unwrap_err();
        assert!(matches!(err.downcast_ref::<Exit>(), Some(Exit(7))));
    }

    #[test]
    fn typed_callback_reenters_through_an_async_import() {
        let mut store = Store::default();
        let instance = fixtures(&mut store);
        let inner = cached(&store, &instance, "add");
        let env = FunctionEnv::new(&mut store, inner);
        // As the trampoline calls a guest function: with the env of the
        // import that led to it, which holds no store.
        let reenter = Function::new_typed_async(
            &mut store,
            &env,
            async |cx: &mut AsyncEnv<CachedGuestCallback>, a: i32, b: i32| -> i32 {
                let callback = cx.with(|env| env.data().clone());
                callback.call(cx, a as u32, b as u32).unwrap().unwrap() as i32
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
                .call(&mut store.as_store_mut(), 19, 23)
                .unwrap()
                .unwrap(),
            42
        );
    }

    /// A function is found again whether or not it is still among the ones
    /// called last, where another whose index shares its slot replaces it.
    #[test]
    fn callbacks_are_kept_when_their_indices_share_a_slot() {
        let mut store = Store::default();
        let instance = fixtures(&mut store);
        let typed = cached(&store, &instance, "add");
        let generic = cached(&store, &instance, "void");
        let mut callbacks = GuestCallbacks::default();
        let shared_slot = 5 + RECENT as u32;
        assert!(callbacks.get(5).is_none());

        callbacks.insert(5, typed);
        callbacks.insert(6, generic.clone());
        assert!(callbacks.get(5).is_some_and(|found| found.is_typed()));
        assert!(callbacks.get(6).is_some_and(|found| !found.is_typed()));
        assert!(callbacks.get(shared_slot).is_none());
        // A miss does not take the slot.
        assert!(callbacks.get(5).is_some_and(|found| found.is_typed()));

        // Each takes the slot from the other, and both are still found.
        callbacks.insert(shared_slot, generic);
        for _ in 0..2 {
            assert!(
                callbacks
                    .get(shared_slot)
                    .is_some_and(|found| !found.is_typed())
            );
            assert!(callbacks.get(5).is_some_and(|found| found.is_typed()));
        }
        assert!(callbacks.get(u32::MAX).is_none());
    }
}
