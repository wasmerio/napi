//! fast-wasmer prototype: what a guest must still be able to do when it runs
//! on the thread's own stack (`WASMER_WASM_ON_THREAD_STACK=1`): trap, overflow
//! its stack, call host functions that fail, panic or call back into it, and
//! go on afterwards. Without the variable the same checks run on the
//! coroutine stacks, and the placement checks expect the opposite.

#![cfg(not(target_arch = "wasm32"))]

use std::sync::atomic::{AtomicUsize, Ordering};

use wasmer::{
    Function, FunctionEnv, FunctionEnvMut, Instance, Module, RuntimeError, Store, TypedFunction,
    imports,
};

const WAT: &str = r#"(module
  (import "env" "fail" (func $fail (param i32) (result i32)))
  (import "env" "panic" (func $panic))
  (import "env" "reenter" (func $reenter (param i32) (result i32)))
  (import "env" "native" (func $native (param i32) (result i32)))
  (memory (export "memory") 1 5)
  (func $recurse (param i32) (result i32)
    (i32.add (call $recurse (i32.add (local.get 0) (i32.const 1))) (i32.const 1)))
  (func (export "trap") unreachable)
  (func (export "oob") (result i32) (i32.load (i32.const 0x7ffffff0)))
  (func (export "recurse") (result i32) (call $recurse (i32.const 0)))
  (func (export "call_fail") (param i32) (result i32) (call $fail (local.get 0)))
  (func (export "call_panic") (call $panic))
  (func (export "call_reenter") (param i32) (result i32) (call $reenter (local.get 0)))
  (func (export "call_native") (param i32) (result i32) (call $native (local.get 0)))
  (func (export "grow") (result i32) (memory.grow (i32.const 1)))
  ;; 0: returns 7 after a host call. 1: traps. 2: a host function fails.
  ;; 3: overflows the stack. 4: grows the memory.
  (func (export "inner") (param i32) (result i32)
    (if (i32.eq (local.get 0) (i32.const 1)) (then unreachable))
    (if (i32.eq (local.get 0) (i32.const 2)) (then (return (call $fail (i32.const 1)))))
    (if (i32.eq (local.get 0) (i32.const 3)) (then (return (call $recurse (i32.const 0)))))
    (if (i32.eq (local.get 0) (i32.const 4)) (then (return (memory.grow (i32.const 1)))))
    (i32.add (call $fail (i32.const 0)) (i32.const 7)))
)"#;

/// The address of a local of the last call of each host function.
static FAIL_AT: AtomicUsize = AtomicUsize::new(0);
static REENTER_AT: AtomicUsize = AtomicUsize::new(0);
static NATIVE_AT: AtomicUsize = AtomicUsize::new(0);

#[derive(Default)]
struct Env {
    inner: Option<TypedFunction<i32, i32>>,
}

#[derive(Debug)]
struct Failure;
impl std::fmt::Display for Failure {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("the host function failed")
    }
}
impl std::error::Error for Failure {}

#[inline(never)]
fn here() -> usize {
    let probe = 0u8;
    std::hint::black_box(std::ptr::from_ref(&probe)) as usize
}

fn fail(_env: FunctionEnvMut<Env>, fail: i32) -> Result<i32, RuntimeError> {
    FAIL_AT.store(here(), Ordering::Relaxed);
    if fail != 0 {
        return Err(RuntimeError::user(Box::new(Failure)));
    }
    Ok(0)
}

fn panic_now(_env: FunctionEnvMut<Env>) {
    panic!("the host function panicked");
}

/// Calls `inner(what)`: returns its result, 100 after a stack overflow, 200
/// after any other trap of `inner(1)`, and fails with its error otherwise.
fn reenter(mut env: FunctionEnvMut<Env>, what: i32) -> Result<i32, RuntimeError> {
    REENTER_AT.store(here(), Ordering::Relaxed);
    let (data, mut store) = env.data_and_store_mut();
    let inner = data.inner.clone().unwrap();
    match inner.call(&mut store, what) {
        Ok(value) => Ok(value),
        Err(error) if what == 3 => {
            assert!(error.message().contains("stack"), "{error}");
            Ok(100)
        }
        Err(_) if what == 1 => Ok(200),
        Err(error) => Err(error),
    }
}

/// Uses 64 KiB of stack, which the host stack must have.
fn native(_env: FunctionEnvMut<Env>, value: i32) -> i32 {
    NATIVE_AT.store(here(), Ordering::Relaxed);
    let mut block = [0u8; 64 * 1024];
    for (i, byte) in block.iter_mut().enumerate() {
        *byte = i as u8;
    }
    let sum: u32 = std::hint::black_box(&block).iter().map(|b| u32::from(*b)).sum();
    value + 1 + (sum & 0) as i32
}

fn on_thread_stack() -> bool {
    std::env::var_os("WASMER_WASM_ON_THREAD_STACK").is_some_and(|v| v == "1")
        && cfg!(all(target_arch = "aarch64", target_vendor = "apple"))
}

#[test]
fn a_guest_traps_overflows_and_calls_back() {
    let mut store = Store::default();
    let module = Module::new(&store, wat::parse_str(WAT).unwrap()).unwrap();
    let env = FunctionEnv::new(&mut store, Env::default());
    let native_fn = Function::new_typed_with_env(&mut store, &env, native);
    native_fn.set_calls_on_native_stack(&mut store, true);
    let imports = imports! {
        "env" => {
            "fail" => Function::new_typed_with_env(&mut store, &env, fail),
            "panic" => Function::new_typed_with_env(&mut store, &env, panic_now),
            "reenter" => Function::new_typed_with_env(&mut store, &env, reenter),
            "native" => native_fn,
        },
    };
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    let unit = |name: &str| -> TypedFunction<(), ()> {
        instance.exports.get_typed_function(&store, name).unwrap()
    };
    let to_i32 = |name: &str| -> TypedFunction<(), i32> {
        instance.exports.get_typed_function(&store, name).unwrap()
    };
    let i32_to_i32 = |name: &str| -> TypedFunction<i32, i32> {
        instance.exports.get_typed_function(&store, name).unwrap()
    };
    let (trap, call_panic) = (unit("trap"), unit("call_panic"));
    let (oob, recurse, grow) = (to_i32("oob"), to_i32("recurse"), to_i32("grow"));
    let (call_fail, call_reenter, call_native, inner) = (
        i32_to_i32("call_fail"),
        i32_to_i32("call_reenter"),
        i32_to_i32("call_native"),
        i32_to_i32("inner"),
    );
    env.as_mut(&mut store).inner = Some(inner.clone());
    let test_frame = here();
    let near = |at: &AtomicUsize| {
        let at = at.load(Ordering::Relaxed);
        at < test_frame && test_frame - at < 1024 * 1024
    };

    for round in 0..3 {
        // Traps of the guest's own.
        let error = trap.call(&mut store).unwrap_err();
        assert!(error.message().contains("unreachable"), "{error}");
        let error = oob.call(&mut store).unwrap_err();
        assert!(error.message().contains("out of bounds"), "{error}");
        let error = recurse.call(&mut store).unwrap_err();
        assert!(error.message().contains("stack"), "{error}");

        // Host functions: in place, failing, panicking, on the host stack.
        assert_eq!(inner.call(&mut store, 0).unwrap(), 7);
        assert_eq!(
            near(&FAIL_AT),
            on_thread_stack(),
            "where an in-place host function ran"
        );
        assert_eq!(call_fail.call(&mut store, 0).unwrap(), 0);
        let error = call_fail.call(&mut store, 1).unwrap_err();
        assert!(error.downcast_ref::<Failure>().is_some(), "{error}");
        let panicked = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            call_panic.call(&mut store)
        }));
        assert!(panicked.is_err());
        assert_eq!(call_native.call(&mut store, 5).unwrap(), 6);
        assert!(near(&NATIVE_AT), "a host-stack function runs on the thread's stack");

        // Calls back into the guest from a host function.
        assert_eq!(call_reenter.call(&mut store, 0).unwrap(), 7);
        if on_thread_stack() {
            // One stack, each activation below the one that made it.
            let (outer, nested) = (
                REENTER_AT.load(Ordering::Relaxed),
                FAIL_AT.load(Ordering::Relaxed),
            );
            assert!(nested < outer && outer < test_frame, "{nested:#x} {outer:#x}");
        }
        assert_eq!(call_reenter.call(&mut store, 1).unwrap(), 200);
        let error = call_reenter.call(&mut store, 2).unwrap_err();
        assert!(error.downcast_ref::<Failure>().is_some(), "{error}");
        assert_eq!(call_reenter.call(&mut store, 3).unwrap(), 100);

        // Libcalls that ask for the host stack.
        assert_eq!(grow.call(&mut store).unwrap(), 1 + round * 2);
        assert_eq!(call_reenter.call(&mut store, 4).unwrap(), 2 + round * 2);
        if round == 1 {
            // The memory is at its maximum of 5 pages after this round.
            break;
        }
    }
    assert_eq!(grow.call(&mut store).unwrap(), -1);
}
