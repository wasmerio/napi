//! Not a correctness test: the cost of Wasmer's own call paths with nothing
//! of N-API in them. A Wasm loop calling a no-op typed host function with an
//! environment, in place and on the native stack, against a loop calling a
//! local Wasm function; and a host loop calling an empty exported function,
//! from the top level and from inside a host function.
//!
//! `PERF_ROUNDS` (default 7) rounds per case, median printed. `PERF_CASE`
//! picks one case. `WASMER_WASM_ON_THREAD_STACK=1` runs the guest on the
//! thread's own stack (fast-wasmer prototype).

#![cfg(not(target_arch = "wasm32"))]

use std::time::Instant;

use wasmer::{
    Function, FunctionEnv, FunctionEnvMut, Instance, Module, Store, TypedFunction, imports,
};

const WAT: &str = r#"(module
  (import "env" "noop" (func $noop (param i32 i32) (result i32)))
  (import "env" "noop_ns" (func $noop_ns (param i32 i32) (result i32)))
  (import "env" "reenter" (func $reenter (param i32) (result i32)))
  (memory (export "memory") 1)
  (func $local (param i32 i32) (result i32) (i32.const 0))
  (func (export "loop_local") (param $n i32)
    (loop $again
      (if (call $local (i32.const 1) (local.get $n)) (then unreachable))
      (br_if $again (local.tee $n (i32.sub (local.get $n) (i32.const 1))))))
  (func (export "loop_noop") (param $n i32)
    (loop $again
      (if (call $noop (i32.const 1) (local.get $n)) (then unreachable))
      (br_if $again (local.tee $n (i32.sub (local.get $n) (i32.const 1))))))
  (func (export "loop_noop_ns") (param $n i32)
    (loop $again
      (if (call $noop_ns (i32.const 1) (local.get $n)) (then unreachable))
      (br_if $again (local.tee $n (i32.sub (local.get $n) (i32.const 1))))))
  (func (export "empty") (param i32 i32) (result i32) (i32.const 0))
  (func (export "call_reenter") (param $n i32) (result i32) (call $reenter (local.get $n)))
)"#;

#[derive(Default)]
struct Env {
    empty: Option<TypedFunction<(i32, i32), i32>>,
    calls: u64,
}

fn noop(mut env: FunctionEnvMut<Env>, _a: i32, _b: i32) -> i32 {
    env.data_mut().calls += 1;
    0
}

fn reenter(mut env: FunctionEnvMut<Env>, n: i32) -> i32 {
    let (data, mut store) = env.data_and_store_mut();
    let empty = data.empty.clone().unwrap();
    let mut sum = 0;
    for i in 0..n {
        sum += empty.call(&mut store, 1, i).unwrap();
    }
    sum
}

fn setting<T: std::str::FromStr>(name: &str) -> Option<T> {
    std::env::var(name).ok().and_then(|v| v.parse().ok())
}

#[test]
#[ignore]
fn wasmer_call_costs() {
    let rounds: usize = setting("PERF_ROUNDS").unwrap_or(7);
    let only: Option<usize> = setting("PERF_CASE");
    let scale: i32 = setting("PERF_SCALE").unwrap_or(1);

    let mut store = Store::default();
    let module = Module::new(&store, wat::parse_str(WAT).unwrap()).unwrap();
    let env = FunctionEnv::new(&mut store, Env::default());
    let noop_fn = Function::new_typed_with_env(&mut store, &env, noop);
    let noop_ns_fn = Function::new_typed_with_env(&mut store, &env, noop);
    noop_ns_fn.set_calls_on_native_stack(&mut store, true);
    let reenter_fn = Function::new_typed_with_env(&mut store, &env, reenter);
    reenter_fn.set_calls_on_native_stack(&mut store, true);
    let imports = imports! {
        "env" => { "noop" => noop_fn, "noop_ns" => noop_ns_fn, "reenter" => reenter_fn },
    };
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    let get = |store: &Store, name: &str| -> TypedFunction<i32, ()> {
        instance.exports.get_typed_function(store, name).unwrap()
    };
    let loop_local = get(&store, "loop_local");
    let loop_noop = get(&store, "loop_noop");
    let loop_noop_ns = get(&store, "loop_noop_ns");
    let empty: TypedFunction<(i32, i32), i32> =
        instance.exports.get_typed_function(&store, "empty").unwrap();
    let call_reenter: TypedFunction<i32, i32> = instance
        .exports
        .get_typed_function(&store, "call_reenter")
        .unwrap();
    env.as_mut(&mut store).empty = Some(empty.clone());

    type Case<'a> = (&'a str, i32, Box<dyn FnMut(&mut Store, i32) + 'a>);
    let cases: Vec<Case> = vec![
        (
            "wasm->wasm local call",
            40_000_000,
            Box::new(|s, n| loop_local.call(s, n).unwrap()),
        ),
        (
            "wasm->host no-op, in place",
            40_000_000,
            Box::new(|s, n| loop_noop.call(s, n).unwrap()),
        ),
        (
            "wasm->host no-op, native stack",
            40_000_000,
            Box::new(|s, n| loop_noop_ns.call(s, n).unwrap()),
        ),
        (
            "host->wasm empty, top level",
            10_000_000,
            Box::new(|s, n| {
                for i in 0..n {
                    empty.call(s, 1, i).unwrap();
                }
            }),
        ),
        (
            "host->wasm empty, from a host function",
            10_000_000,
            Box::new(|s, n| {
                call_reenter.call(s, n).unwrap();
            }),
        ),
    ];
    for (index, (name, ops, mut work)) in cases.into_iter().enumerate() {
        if only.is_some_and(|only| only != index) {
            continue;
        }
        let ops = ops * scale;
        work(&mut store, ops / 10);
        let mut samples: Vec<f64> = (0..rounds)
            .map(|_| {
                let began = Instant::now();
                work(&mut store, ops);
                began.elapsed().as_nanos() as f64 / f64::from(ops)
            })
            .collect();
        samples.sort_by(|a, b| a.partial_cmp(b).unwrap());
        println!(
            "HPERF case=\"{name}\" median_ns={:.2} min_ns={:.2}",
            samples[samples.len() / 2],
            samples[0]
        );
    }
}
