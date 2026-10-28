//! Not correctness tests: the cost of a guest N-API import and of a
//! JavaScript call into a guest function, through real Wasmer dispatch, the
//! bridge and V8, with 1, 2 and 4 guests on their own threads. Run with
//! `./cargo-standalone.sh test --release --test perf_calls -- --ignored --nocapture`.
//!
//! `PERF_ROUNDS` (default 5) sets the rounds per case, of which the median is
//! printed. `PERF_CASE` (0-3) runs one case, `PERF_THREADS` (e.g. `1,4`)
//! picks the thread counts and `PERF_SCALE` multiplies the calls per round,
//! for example to keep a profiler attached long enough.

#![cfg(not(target_arch = "wasm32"))]

use std::{
    sync::{Arc, Barrier},
    thread,
    time::{Duration, Instant},
};

use wasmer::{AsStoreMut, Engine, Imports, Instance, Memory, Module, Store, TypedFunction};
use wasmer_napi::NapiCtx;

const SOURCE_PTR: u32 = 4096;

const WAT: &str = r#"(module
  (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
    (func $configure (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
    (func $create_env (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
    (func $release_env (param i32 i32) (result i32)))
  (import "napi" "napi_create_string_utf8"
    (func $create_string (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_run_script" (func $run_script (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_int32" (func $get_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_int32" (func $create_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_undefined" (func $get_undefined (param i32 i32) (result i32)))
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_named_property (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_function"
    (func $create_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_cb_info"
    (func $get_cb_info (param i32 i32 i32 i32 i32 i32) (result i32)))

  ;; 8 env, 12 scope, 28 string, 32 script result, 44 int handle, 48 global,
  ;; 52 function, 56 callback result, 60 argc, 64 argv, 68 this, 72 int out.
  (memory (export "memory") 2)
  (table (export "__indirect_function_table") 4 funcref)
  (global $env (mut i32) (i32.const 0))
  (global $scope (mut i32) (i32.const 0))
  (data (i32.const 100) "f\00")
  (data (i32.const 104) "g\00")

  (func $define (param $name i32) (param $index i32)
    (if (call $create_function (global.get $env) (local.get $name) (i32.const 1)
          (local.get $index) (i32.const 0) (i32.const 52))
      (then unreachable))
    (if (call $get_global (global.get $env) (i32.const 48)) (then unreachable))
    (if (call $set_named_property (global.get $env) (i32.load (i32.const 48))
          (local.get $name) (i32.load (i32.const 52)))
      (then unreachable)))

  (func (export "create")
    (if (call $configure (i32.const 0)) (then unreachable))
    (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
      (then unreachable))
    (global.set $env (i32.load (i32.const 8)))
    (global.set $scope (i32.load (i32.const 12)))
    (if (call $create_int32 (global.get $env) (i32.const 42) (i32.const 44))
      (then unreachable))
    (call $define (i32.const 100) (i32.const 1))
    (call $define (i32.const 104) (i32.const 2)))

  (func (export "release") (result i32)
    (call $release_env (global.get $scope) (i32.const 0)))

  (func (export "eval") (param $ptr i32) (param $len i32) (result i32)
    (if (call $create_string (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28))
      (then unreachable))
    (call $run_script (global.get $env) (i32.load (i32.const 28)) (i32.const 32)))

  ;; n x napi_get_value_int32 on one handle.
  (func (export "loop_get_int32") (param $n i32)
    (loop $again
      (if (call $get_int32 (global.get $env) (i32.load (i32.const 44)) (i32.const 72))
        (then unreachable))
      (br_if $again (local.tee $n (i32.sub (local.get $n) (i32.const 1))))))

  ;; n x napi_get_undefined.
  (func (export "loop_get_undefined") (param $n i32)
    (loop $again
      (if (call $get_undefined (global.get $env) (i32.const 56)) (then unreachable))
      (br_if $again (local.tee $n (i32.sub (local.get $n) (i32.const 1))))))

  ;; f(x): reads one argument and `this`, returns x as a new int32, like a
  ;; small DOM method.
  (func $f (param $env i32) (param $info i32) (result i32)
    (i32.store (i32.const 60) (i32.const 1))
    (if (call $get_cb_info (local.get $env) (local.get $info) (i32.const 60) (i32.const 64)
          (i32.const 68) (i32.const 0))
      (then unreachable))
    (if (call $get_int32 (local.get $env) (i32.load (i32.const 64)) (i32.const 72))
      (then unreachable))
    (if (call $create_int32 (local.get $env) (i32.load (i32.const 72)) (i32.const 56))
      (then unreachable))
    (i32.load (i32.const 56)))
  ;; g(): does nothing, returns undefined.
  (func $g (param $env i32) (param $info i32) (result i32) (i32.const 0))
  (elem (i32.const 1) $f $g)
)"#;

struct Guest {
    store: Store,
    instance: Instance,
    memory: Memory,
}

impl Guest {
    fn new(ctx: &NapiCtx, engine: &Engine, module: &Module) -> Self {
        let mut store = Store::new(engine.clone());
        let hooks = ctx.runtime_hooks();
        let (imports, state): (Imports, _) = hooks
            .additional_imports(module, &mut store.as_store_mut())
            .expect("failed to create N-API imports");
        let instance = Instance::new(&mut store, module, &imports).expect("instantiate");
        let memory = instance.exports.get_memory("memory").unwrap().clone();
        hooks
            .configure_instance(
                module,
                &mut store.as_store_mut(),
                &instance,
                Some(&memory),
                state,
            )
            .expect("configure instance");
        let mut guest = Self {
            store,
            instance,
            memory,
        };
        let create: TypedFunction<(), ()> = guest.typed("create");
        create.call(&mut guest.store).expect("create trapped");
        guest
    }

    fn typed<A: wasmer::WasmTypeList, R: wasmer::WasmTypeList>(
        &self,
        name: &str,
    ) -> TypedFunction<A, R> {
        self.instance
            .exports
            .get_typed_function(&self.store, name)
            .unwrap()
    }

    fn eval(&mut self, source: &str) -> i32 {
        self.memory
            .view(&self.store)
            .write(u64::from(SOURCE_PTR), source.as_bytes())
            .unwrap();
        let eval: TypedFunction<(i32, i32), i32> = self.typed("eval");
        eval.call(&mut self.store, SOURCE_PTR as i32, source.len() as i32)
            .expect("eval trapped")
    }

    fn release(&mut self) -> i32 {
        let release: TypedFunction<(), i32> = self.typed("release");
        release.call(&mut self.store).expect("release trapped")
    }
}

/// One benchmark case: performs `n` operations in a guest.
type Work = fn(&mut Guest, u32);

/// Reads a numeric setting from the environment.
fn setting<T: std::str::FromStr>(name: &str) -> Option<T> {
    std::env::var(name)
        .ok()
        .and_then(|value| value.parse().ok())
}

/// Runs `work` on `threads` guests at once; returns the slowest thread's ns
/// per operation.
fn measure(
    ctx: &NapiCtx,
    engine: &Engine,
    module: &Module,
    threads: usize,
    ops: u32,
    work: Work,
) -> f64 {
    let start = Arc::new(Barrier::new(threads + 1));
    let workers: Vec<_> = (0..threads)
        .map(|_| {
            let (ctx, engine, module, start) = (
                ctx.clone(),
                engine.clone(),
                module.clone(),
                Arc::clone(&start),
            );
            thread::spawn(move || {
                let mut guest = Guest::new(&ctx, &engine, &module);
                work(&mut guest, ops / 10); // warm up
                start.wait();
                let began = Instant::now();
                work(&mut guest, ops);
                let elapsed = began.elapsed();
                assert_eq!(guest.release(), 0);
                elapsed
            })
        })
        .collect();
    start.wait();
    let slowest: Duration = workers
        .into_iter()
        .map(|w| w.join().unwrap())
        .max()
        .unwrap();
    slowest.as_nanos() as f64 / f64::from(ops)
}

fn loop_get_int32(guest: &mut Guest, n: u32) {
    let f: TypedFunction<i32, ()> = guest.typed("loop_get_int32");
    f.call(&mut guest.store, n as i32).unwrap();
}

fn loop_get_undefined(guest: &mut Guest, n: u32) {
    let f: TypedFunction<i32, ()> = guest.typed("loop_get_undefined");
    f.call(&mut guest.store, n as i32).unwrap();
}

fn js_calls_f(guest: &mut Guest, n: u32) {
    // In a function, so the warm-up and the measured run can both declare `s`.
    let script =
        format!("(() => {{ let s = 0; for (let i = 0; i < {n}; i++) s += f(i); return s; }})()");
    assert_eq!(guest.eval(&script), 0);
}

fn js_calls_g(guest: &mut Guest, n: u32) {
    let script = format!("for (let i = 0; i < {n}; i++) g();");
    assert_eq!(guest.eval(&script), 0);
}

#[test]
#[ignore]
fn guest_call_costs() {
    let store = Store::default();
    let module = Module::new(&store, wat::parse_str(WAT).unwrap()).unwrap();
    let engine = store.engine().clone();
    let ctx = NapiCtx::default();
    let rounds: usize = setting("PERF_ROUNDS").unwrap_or(5);
    let cases: [(&str, u32, Work); 4] = [
        ("import napi_get_value_int32", 4_000_000, loop_get_int32),
        ("import napi_get_undefined", 4_000_000, loop_get_undefined),
        ("js->guest g() empty", 1_000_000, js_calls_g),
        ("js->guest f(x) cb_info+get+create", 1_000_000, js_calls_f),
    ];
    let only: Option<usize> = setting("PERF_CASE");
    let scale: u32 = setting("PERF_SCALE").unwrap_or(1);
    let thread_counts: Vec<usize> = std::env::var("PERF_THREADS")
        .ok()
        .map(|v| v.split(',').filter_map(|t| t.parse().ok()).collect())
        .unwrap_or_else(|| vec![1, 2, 4]);
    for (index, (name, ops, work)) in cases.into_iter().enumerate() {
        if only.is_some_and(|only| only != index) {
            continue;
        }
        let ops = ops * scale;
        for &threads in &thread_counts {
            let mut samples: Vec<f64> = (0..rounds)
                .map(|_| measure(&ctx, &engine, &module, threads, ops, work))
                .collect();
            samples.sort_by(|a, b| a.partial_cmp(b).unwrap());
            println!(
                "PERF case=\"{name}\" threads={threads} median_ns={:.1} min_ns={:.1}",
                samples[samples.len() / 2],
                samples[0]
            );
        }
    }
}
