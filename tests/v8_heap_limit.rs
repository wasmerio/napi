#![cfg(not(target_arch = "wasm32"))]

//! V8 heap exhaustion must stop the offending context, never the process.
//!
//! V8 gives the near-heap-limit callback one answer per occasion and aborts
//! the process (`FatalProcessOutOfMemory`) if that answer leaves the committed
//! old generation or the retried allocation over the hard limit. Two V8
//! behaviours make a plain grow-step policy fatal in a shared host: the first
//! large young object is admitted without consulting the limit and promoted
//! by the next full collection, and a single allocation can be a 1 GiB
//! object. These tests drive such allocations through the guest imports and
//! check that the context ends terminated while the process survives.

use std::{
    sync::{Arc, Barrier},
    thread,
};

use wasmer::{AsStoreMut, Imports, Instance, Module, Store};
use wasmer_napi::NapiCtx;
use wasmer_napi::budget::{DEFAULT_HEAP_EMERGENCY_HEADROOM, ResourceUsage, heap_emergency_stats};

const MIB: u64 = 1024 * 1024;

/// `napi_pending_exception`: what a terminated isolate reports from
/// `napi_run_script`.
const NAPI_PENDING_EXCEPTION: i32 = 10;

/// Leaves a default isolate about two grow steps before the budget refuses.
const BUDGET: u64 = 160 * MIB;

/// The largest emergency exposure a test accepts: the headroom, the unwind
/// slack, and an overshoot of up to one maximal V8 object.
const MAX_EMERGENCY: u64 = DEFAULT_HEAP_EMERGENCY_HEADROOM + 2048 * MIB;

/// Escapes `js` for a WAT string literal.
fn wat_string(js: &str) -> String {
    let mut out = String::with_capacity(js.len());
    for ch in js.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            c if c.is_ascii_graphic() || c == ' ' => out.push(c),
            c => {
                let mut buf = [0u8; 4];
                for byte in c.encode_utf8(&mut buf).bytes() {
                    out.push_str(&format!("\\{byte:02x}"));
                }
            }
        }
    }
    out
}

/// What one guest saw running a script in its env (or its worker env).
#[derive(Debug)]
struct Statuses {
    /// The script under test.
    script: i32,
    /// A trivial script run right after it in the same env. A termination
    /// requested while the first script sat in a native builtin is only
    /// observed at the next JavaScript entry, so this is where a stopped
    /// context shows.
    after: i32,
    /// `1+1` in the other env (worker runs only), or 0.
    after_main: i32,
    /// The budget while the envs were alive.
    in_flight: ResourceUsage,
    /// The budget after the store, with its envs, was dropped.
    released: ResourceUsage,
}

impl Statuses {
    fn completed(&self) -> bool {
        self.script == 0 && self.after == 0
    }

    /// The context was terminated: either the script unwound on the
    /// near-heap-limit refusal, or the refusal landed on the collection after
    /// it and the probe script sees it.
    fn terminated(&self) -> bool {
        self.script == NAPI_PENDING_EXCEPTION || self.after == NAPI_PENDING_EXCEPTION
    }

    /// The store's envs gave everything back.
    fn assert_released(&self, name: &str) {
        let released = &self.released;
        assert_eq!(released.v8_heap_emergency, 0, "{name}: {released:?}");
        assert_eq!(released.v8_heap_reserved, 0, "{name}: {released:?}");
        assert_eq!(released.live_isolates, 0, "{name}: {released:?}");
    }
}

/// Runs `js` through `napi_run_script` in a new env of `ctx`, then `1+1` in
/// the same env. With `worker`, a first env stays idle and `js` runs in a
/// second one (its own isolate and heap limit, the same budget); `1+1` then
/// also runs in the first.
fn run(ctx: &NapiCtx, js: &str, worker: bool) -> Statuses {
    assert!(!js.contains('\0'));
    let wat = format!(
        r#"(module
      (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
        (func $configure (param i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
        (func $create (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
      (memory (export "memory") 1)
      (data (i32.const 256) "1+1\00")
      (data (i32.const 512) "{}\00")
      ;; the main env at 8, the worker env at 40
      (func $run_at (param $env i32) (param $source i32) (result i32)
        (if (call $string (local.get $env) (local.get $source) (i32.const -1) (i32.const 20))
          (then (return (i32.const -2))))
        (call $script (local.get $env) (i32.load (i32.const 20)) (i32.const 24)))
      (func $target (result i32)
        (if (result i32) (i32.load (i32.const 40))
          (then (i32.load (i32.const 40)))
          (else (i32.load (i32.const 8)))))
      (func (export "run") (param $worker i32) (result i32)
        (if (call $configure (i32.const 0))
          (then (return (i32.const -4))))
        (if (call $create (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
          (then (return (i32.const -1))))
        (if (local.get $worker)
          (then
            (if (call $create (i32.const 8) (i32.const 0) (i32.const 40) (i32.const 44))
              (then (return (i32.const -3))))))
        (call $run_at (call $target) (i32.const 512)))
      (func (export "after") (result i32)
        (call $run_at (call $target) (i32.const 256)))
      (func (export "after_main") (result i32)
        (call $run_at (i32.load (i32.const 8)) (i32.const 256))))"#,
        wat_string(js)
    );
    let in_flight;
    let (script, after, after_main);
    {
        let mut store = Store::default();
        let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
        let hooks = ctx.runtime_hooks();
        let (imports, state): (Imports, _) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .expect("failed to create N-API imports");
        let instance = Instance::new(&mut store, &module, &imports).expect("instantiate");
        let memory = instance.exports.get_memory("memory").unwrap().clone();
        hooks
            .configure_instance(
                &module,
                &mut store.as_store_mut(),
                &instance,
                Some(&memory),
                state,
            )
            .expect("configure instance");
        script = instance
            .exports
            .get_typed_function::<i32, i32>(&store, "run")
            .unwrap()
            .call(&mut store, i32::from(worker))
            .unwrap();
        let call = |name: &str, store: &mut Store| {
            instance
                .exports
                .get_typed_function::<(), i32>(store, name)
                .unwrap()
                .call(store)
                .unwrap()
        };
        after = call("after", &mut store);
        after_main = if worker {
            call("after_main", &mut store)
        } else {
            0
        };
        in_flight = ctx.budget().snapshot();
    }
    Statuses {
        script,
        after,
        after_main,
        in_flight,
        released: ctx.budget().snapshot(),
    }
}

fn run_script(ctx: &NapiCtx, js: &str) -> Statuses {
    run(ctx, js, false)
}

/// Scripts that retain allocations far larger than the budget, through the
/// env's global, so no collection can reclaim them. Each used to abort the
/// process; each must terminate its own context instead.
const RETAINING_BOMBS: &[(&str, &str)] = &[
    ("array_fill", "var a=new Array(30000000).fill(1.5);"),
    (
        "array_from_length",
        "var a=Array.from({length:40000000},function(_,i){return i;});",
    ),
    (
        "json_parse",
        "var a=JSON.parse('['+'1,'.repeat(30000000)+'1]');",
    ),
    (
        "map_growth",
        "var m=globalThis.m=new Map();for(var i=0;i<100000000;i++){m.set(i,i);}",
    ),
    (
        "object_graph",
        "var r=globalThis.r=[];for(var i=0;i<100000000;i++){r.push({a:i,b:[i]});}",
    ),
    (
        "nested_arrays",
        "function f(n){var o=[];for(var i=0;i<96;i++){o.push(n>0?f(n-1):i);}return o;}globalThis.t=f(5);",
    ),
];

/// Scripts that request an enormous allocation but need not retain a large
/// object (V8 may throw, reclaim, or never make it). The guarantee for these:
/// the process survives, and the context either completes or is terminated.
const TRANSIENT_BOMBS: &[(&str, &str)] = &[
    ("string_repeat", "var s=globalThis.s='x'.repeat(500000000);"),
    (
        "two_byte_string_repeat",
        "var s=globalThis.s='\\u0100'.repeat(400000000);",
    ),
    (
        "array_fill_transient",
        "for(var i=0;i<8;i++){new Array(30000000).fill(1.5);}",
    ),
];

/// Off-heap backing stores are refused softly by the allocator: a
/// `RangeError`, no termination.
const BACKING_STORE_BOMBS: &[(&str, &str)] = &[
    (
        "typed_array",
        "try{new Float64Array(400000000);}catch(e){if(!(e instanceof RangeError))throw e;}",
    ),
    (
        "array_buffer_2gib",
        "try{new ArrayBuffer(2147483648);}catch(e){if(!(e instanceof RangeError))throw e;}",
    ),
];

const BENIGN: &str =
    "var a=[];for(var i=0;i<1000;i++){a.push({i:i});}if(a.length!==1000)throw new Error('bad');";

#[test]
fn retained_heap_bombs_terminate_only_their_context() {
    for (name, js) in RETAINING_BOMBS {
        let grants_before = heap_emergency_stats().grants;
        let ctx = NapiCtx::builder().total_memory_bytes(BUDGET).build();
        let statuses = run_script(&ctx, js);
        assert!(
            statuses.terminated(),
            "{name}: expected the context to be terminated, got {statuses:?}"
        );
        let usage = &statuses.in_flight;
        assert!(
            usage.v8_heap_emergency > 0,
            "{name}: the emergency headroom was not used: {usage:?}"
        );
        assert!(
            usage.v8_heap_emergency <= MAX_EMERGENCY,
            "{name}: implausible emergency exposure: {usage:?}"
        );
        assert!(heap_emergency_stats().grants > grants_before, "{name}");
        statuses.assert_released(name);
    }
    assert_eq!(heap_emergency_stats().exhausted, 0);
}

#[test]
fn transient_heap_bombs_never_abort_the_process() {
    for (name, js) in TRANSIENT_BOMBS {
        let ctx = NapiCtx::builder().total_memory_bytes(BUDGET).build();
        let statuses = run_script(&ctx, js);
        assert!(
            statuses.completed() || statuses.terminated(),
            "{name}: unexpected status {statuses:?}"
        );
        if statuses.in_flight.v8_heap_emergency > 0 {
            assert!(
                statuses.terminated(),
                "{name}: used emergency headroom but was not terminated: {statuses:?}"
            );
        }
        assert!(
            statuses.in_flight.v8_heap_emergency <= MAX_EMERGENCY,
            "{name}: implausible emergency exposure: {statuses:?}"
        );
        statuses.assert_released(name);
    }
}

#[test]
fn backing_store_bombs_are_refused_softly() {
    for (name, js) in BACKING_STORE_BOMBS {
        let ctx = NapiCtx::builder().total_memory_bytes(BUDGET).build();
        let statuses = run_script(&ctx, js);
        assert!(
            statuses.completed(),
            "{name}: expected a soft RangeError, got {statuses:?}"
        );
        assert_eq!(statuses.in_flight.v8_heap_emergency, 0, "{name}");
    }
}

/// A benign tenant keeps running while another tenant's context is stopped
/// for exhausting its heap.
#[test]
fn a_heap_bomb_does_not_disturb_a_benign_context() {
    let start = Arc::new(Barrier::new(2));
    let bomb = {
        let start = Arc::clone(&start);
        thread::spawn(move || {
            let ctx = NapiCtx::builder().total_memory_bytes(BUDGET).build();
            start.wait();
            run_script(&ctx, "var a=new Array(30000000).fill(1.5);")
        })
    };
    let benign = {
        let start = Arc::clone(&start);
        thread::spawn(move || {
            let ctx = NapiCtx::builder().total_memory_bytes(BUDGET).build();
            start.wait();
            (0..20)
                .map(|_| run_script(&ctx, BENIGN))
                .collect::<Vec<_>>()
        })
    };
    let bomb = bomb.join().unwrap();
    assert!(bomb.terminated(), "{bomb:?}");
    assert!(bomb.in_flight.v8_heap_emergency > 0, "{bomb:?}");
    let benign = benign.join().unwrap();
    assert!(
        benign.iter().all(Statuses::completed),
        "the benign context was disturbed: {benign:?}"
    );
    assert!(
        benign
            .iter()
            .all(|statuses| statuses.in_flight.v8_heap_emergency == 0),
        "the benign context's budget saw the other tenant's emergency: {benign:?}"
    );
}

/// Two isolates share one budget (a main env and a worker); the bomb runs in
/// the worker, whose own near-heap-limit callback refuses, terminates it and
/// exposes headroom. The process survives and the budget is released with the
/// envs.
#[test]
fn a_heap_bomb_in_a_worker_isolate_terminates_only_that_tenant() {
    let grants_before = heap_emergency_stats().grants;
    // Room for two default isolates plus the guest's memory.
    let ctx = NapiCtx::builder().total_memory_bytes(256 * MIB).build();
    let worker = run(&ctx, "var a=new Array(30000000).fill(1.5);", true);
    assert!(
        worker.terminated(),
        "the worker isolate should be terminated, got {worker:?}"
    );
    assert_eq!(worker.in_flight.live_isolates, 2, "{worker:?}");
    assert!(worker.in_flight.v8_heap_emergency > 0, "{worker:?}");
    assert!(heap_emergency_stats().grants > grants_before);
    // The refusal stops the whole context (the embedder ends the instance),
    // so the main env either still answers or reports the stop; it must not
    // take the process with it.
    assert!(
        worker.after_main == 0 || worker.after_main == NAPI_PENDING_EXCEPTION,
        "unexpected main env status {}",
        worker.after_main
    );
    worker.assert_released("worker");
}

/// Several tenants reach their emergency at the same time. Each is terminated
/// with its own headroom, the process survives, and a benign tenant is not
/// disturbed.
#[test]
fn simultaneous_heap_bombs_each_terminate_and_a_benign_tenant_is_unaffected() {
    let grants_before = heap_emergency_stats().grants;
    let start = Arc::new(Barrier::new(3));
    let bombs: Vec<_> = (0..2)
        .map(|_| {
            let start = Arc::clone(&start);
            thread::spawn(move || {
                let ctx = NapiCtx::builder().total_memory_bytes(BUDGET).build();
                start.wait();
                run_script(&ctx, "var a=new Array(30000000).fill(1.5);")
            })
        })
        .collect();
    let benign = {
        let start = Arc::clone(&start);
        thread::spawn(move || {
            let ctx = NapiCtx::builder().total_memory_bytes(BUDGET).build();
            start.wait();
            (0..20)
                .map(|_| run_script(&ctx, BENIGN))
                .collect::<Vec<_>>()
        })
    };
    for bomb in bombs {
        let statuses = bomb.join().unwrap();
        assert!(statuses.terminated(), "{statuses:?}");
        assert!(statuses.in_flight.v8_heap_emergency > 0, "{statuses:?}");
        statuses.assert_released("bomb");
    }
    assert!(heap_emergency_stats().grants >= grants_before + 2);
    let benign = benign.join().unwrap();
    assert!(
        benign.iter().all(Statuses::completed),
        "the benign tenant was disturbed: {benign:?}"
    );
}
