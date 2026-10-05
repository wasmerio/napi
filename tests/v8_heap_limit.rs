//! V8 heap exhaustion must stop the offending context, never the process.
//!
//! V8 gives the near-heap-limit callback one answer per occasion and aborts
//! the process (`FatalProcessOutOfMemory`) if that answer leaves the committed
//! old generation or the retried allocation over the hard limit. Two V8
//! behaviours make a naive grow-step policy fatal in a multi-tenant host: the
//! first large young object is admitted without consulting the limit and
//! promoted by the next full collection, and a single allocation can be a
//! 1 GiB object. These tests drive such allocations through the guest imports
//! and assert that the context ends terminated while the process survives.

use std::{
    sync::{Arc, Barrier},
    thread,
};

use wasmer::{AsStoreMut, Instance, Module, Store};
use wasmer_napi::NapiCtx;
use wasmer_napi::budget::{DEFAULT_HEAP_EMERGENCY_HEADROOM, ResourceUsage, heap_emergency_stats};

const MIB: u64 = 1024 * 1024;

/// `napi_pending_exception`: what a terminated isolate reports from
/// `napi_run_script`.
const NAPI_PENDING_EXCEPTION: i32 = 10;

/// Leaves a default isolate about one grow step before the budget refuses.
const BUDGET: u64 = 160 * MIB;

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

/// Statuses of the two `napi_run_script` calls one guest makes in one env.
#[derive(Debug, PartialEq, Eq)]
struct Statuses {
    /// The script under test.
    script: i32,
    /// A trivial script run right after it in the same env. A termination
    /// requested while the first script sat in a native builtin is only
    /// observed at the next JavaScript entry, so this is where a stopped
    /// context shows.
    after: i32,
    /// The budget while the env was still alive.
    in_flight: ResourceUsage,
}

impl Statuses {
    fn completed(&self) -> bool {
        self.script == 0 && self.after == 0
    }

    /// The context was terminated: either the script under test unwound on the
    /// near-heap-limit refusal (`script` reports the pending exception), or the
    /// refusal landed on the post-script settle collection and the probe script
    /// sees it (`after`).
    fn terminated(&self) -> bool {
        self.script == NAPI_PENDING_EXCEPTION || self.after == NAPI_PENDING_EXCEPTION
    }
}

/// Creates an env in `ctx`, runs `js` through `napi_run_script`, then runs
/// `1+1` in the same env.
fn run_script(ctx: &NapiCtx, js: &str) -> Statuses {
    assert!(!js.contains('\0'));
    let wat = format!(
        r#"(module
      (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
      (memory (export "memory") 1)
      (data (i32.const 256) "1+1\00")
      (data (i32.const 512) "{}\00")
      (func $run_at (param $source i32) (result i32)
        (if (call $string (i32.load (i32.const 4)) (local.get $source) (i32.const -1) (i32.const 20))
          (then (return (i32.const -2))))
        (call $script (i32.load (i32.const 4)) (i32.load (i32.const 20)) (i32.const 24)))
      (func (export "run") (result i32)
        (if (call $create (i32.const 8) (i32.const 4) (i32.const 8))
          (then (return (i32.const -1))))
        (call $run_at (i32.const 512)))
      (func (export "after") (result i32)
        (call $run_at (i32.const 256))))"#,
        wat_string(js)
    );
    let mut store = Store::default();
    let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
    let session = ctx.new_session(&module).unwrap();
    let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    session
        .configure_instance(&mut store.as_store_mut(), &instance, None)
        .unwrap();
    let call = |name: &str, store: &mut Store| {
        instance
            .exports
            .get_typed_function::<(), i32>(store, name)
            .unwrap()
            .call(store)
            .unwrap()
    };
    let script = call("run", &mut store);
    let after = call("after", &mut store);
    // Before the store (and with it the env) is dropped.
    let in_flight = ctx.budget().snapshot();
    Statuses {
        script,
        after,
        in_flight,
    }
}

/// Scripts that retain one or more heap allocations far larger than the
/// budget. Each used to abort the process; each must now terminate its own
/// context instead, having used (and released) emergency headroom. Every one
/// builds objects that stay reachable through the env's global so a collection
/// cannot reclaim them, which is what forces the near-heap-limit refusal.
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

/// Scripts that request an enormous allocation but do not necessarily retain a
/// large object (V8 may throw, reclaim, or never materialize it). The only
/// guarantee for these is the one that matters: the process stays alive and
/// the result is deterministic (completed or a terminated context), never an
/// abort.
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

/// Off-heap backing stores are refused softly by the allocator today; they
/// must stay that way (a `RangeError`, no termination, no abort).
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

#[test]
fn retained_heap_bombs_terminate_only_their_context() {
    for (name, js) in RETAINING_BOMBS {
        let grants_before = heap_emergency_stats().grants;
        let ctx = NapiCtx::builder().total_memory_bytes(BUDGET).build();
        let statuses = run_script(&ctx, js);
        // The retained allocation cannot fit the budget, so the near-heap-limit
        // refusal terminated the context, either while the script ran or on the
        // settle collection right after it.
        assert!(
            statuses.terminated(),
            "{name}: expected the context to be terminated, got {statuses:?}"
        );
        let usage = statuses.in_flight;
        assert!(
            usage.v8_heap_emergency > 0,
            "{name}: the emergency headroom was not used: {usage:?}"
        );
        assert!(
            usage.v8_heap_emergency <= DEFAULT_HEAP_EMERGENCY_HEADROOM + 2048 * MIB,
            "{name}: implausible emergency exposure: {usage:?}"
        );
        assert!(heap_emergency_stats().grants > grants_before, "{name}");
        // Env teardown returned the exposure together with the ceiling.
        let released = ctx.budget().snapshot();
        assert_eq!(released.v8_heap_emergency, 0, "{name}: {released:?}");
        assert_eq!(released.v8_heap_reserved, 0, "{name}: {released:?}");
        assert_eq!(released.live_isolates, 0, "{name}: {released:?}");
    }
}

/// The weaker but universal guarantee for allocations that may or may not
/// retain a large object: the process survives and the outcome is one of the
/// two safe ones, never an abort. Any emergency headroom used is bounded and
/// released, and any context that used it is terminated.
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
            statuses.in_flight.v8_heap_emergency <= DEFAULT_HEAP_EMERGENCY_HEADROOM + 2048 * MIB,
            "{name}: implausible emergency exposure: {statuses:?}"
        );
        let released = ctx.budget().snapshot();
        assert_eq!(released.v8_heap_emergency, 0, "{name}: {released:?}");
        assert_eq!(released.live_isolates, 0, "{name}: {released:?}");
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
/// for blowing its heap.
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
            let mut statuses = Vec::new();
            for _ in 0..20 {
                statuses.push(run_script(
                    &ctx,
                    "var a=[];for(var i=0;i<1000;i++){a.push({i:i});}if(a.length!==1000)throw new Error('bad');",
                ));
            }
            statuses
        })
    };
    let bomb = bomb.join().unwrap();
    assert_eq!(bomb.after, NAPI_PENDING_EXCEPTION, "{bomb:?}");
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

/// Runs `js` in a *second* env of `ctx` (a worker isolate: own heap limit and
/// own emergency headroom, shared budget) while a first env stays idle, then
/// probes both envs with `1+1`. Returns the worker's statuses and the main
/// env's probe status.
fn run_script_in_worker(ctx: &NapiCtx, js: &str) -> (Statuses, i32) {
    assert!(!js.contains('\0'));
    let wat = format!(
        r#"(module
      (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
      (memory (export "memory") 1)
      (data (i32.const 256) "1+1\00")
      (data (i32.const 512) "{}\00")
      (func $run_at (param $env i32) (param $source i32) (result i32)
        (if (call $string (local.get $env) (local.get $source) (i32.const -1) (i32.const 20))
          (then (return (i32.const -2))))
        (call $script (local.get $env) (i32.load (i32.const 20)) (i32.const 24)))
      (func (export "run") (result i32)
        ;; main env -> mem[4]; worker env -> mem[40]
        (if (call $create (i32.const 0) (i32.const 4) (i32.const 12))
          (then (return (i32.const -1))))
        (if (call $create (i32.const 0) (i32.const 40) (i32.const 44))
          (then (return (i32.const -3))))
        (call $run_at (i32.load (i32.const 40)) (i32.const 512)))
      (func (export "after") (result i32)
        (call $run_at (i32.load (i32.const 40)) (i32.const 256)))
      (func (export "after_main") (result i32)
        (call $run_at (i32.load (i32.const 4)) (i32.const 256))))"#,
        wat_string(js)
    );
    let mut store = Store::default();
    let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
    let session = ctx.new_session(&module).unwrap();
    let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    session
        .configure_instance(&mut store.as_store_mut(), &instance, None)
        .unwrap();
    let call = |name: &str, store: &mut Store| {
        instance
            .exports
            .get_typed_function::<(), i32>(store, name)
            .unwrap()
            .call(store)
            .unwrap()
    };
    let script = call("run", &mut store);
    let after = call("after", &mut store);
    let after_main = call("after_main", &mut store);
    let in_flight = ctx.budget().snapshot();
    (
        Statuses {
            script,
            after,
            in_flight,
        },
        after_main,
    )
}

/// Two isolates share one budget (a main env and a worker); the bomb runs in
/// the worker. The worker's own near-heap-limit callback refuses, terminates
/// the worker and exposes headroom; the process survives; the shared budget
/// is released with the envs.
#[test]
fn a_heap_bomb_in_a_worker_isolate_terminates_only_that_tenant() {
    let grants_before = heap_emergency_stats().grants;
    // Room for two default isolates plus the guest heap.
    let ctx = NapiCtx::builder().total_memory_bytes(256 * MIB).build();
    let (worker, after_main) = run_script_in_worker(&ctx, "var a=new Array(30000000).fill(1.5);");
    assert!(
        worker.terminated(),
        "the worker isolate should be terminated, got {worker:?}"
    );
    assert_eq!(worker.in_flight.live_isolates, 2, "{worker:?}");
    assert!(worker.in_flight.v8_heap_emergency > 0, "{worker:?}");
    assert!(heap_emergency_stats().grants > grants_before);
    // The refusal is sticky for the whole context (the embedder kills the
    // instance), so the main env either still answers or reports the stop;
    // what it must not do is take the process with it.
    assert!(
        after_main == 0 || after_main == NAPI_PENDING_EXCEPTION,
        "unexpected main env status {after_main}"
    );
    let released = ctx.budget().snapshot();
    assert_eq!(released.v8_heap_emergency, 0, "{released:?}");
    assert_eq!(released.live_isolates, 0, "{released:?}");
}

/// Node-wide worst case: several tenants hit emergency at the same time. Each
/// is terminated with its own headroom; the process survives; a benign third
/// tenant is undisturbed; every exposure is released.
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
                let statuses = run_script(&ctx, "var a=new Array(30000000).fill(1.5);");
                (statuses, ctx.budget().snapshot())
            })
        })
        .collect();
    let benign = {
        let start = Arc::clone(&start);
        thread::spawn(move || {
            let ctx = NapiCtx::builder().total_memory_bytes(BUDGET).build();
            start.wait();
            (0..20)
                .map(|_| {
                    run_script(
                        &ctx,
                        "var a=[];for(var i=0;i<1000;i++){a.push({i:i});}if(a.length!==1000)throw new Error('bad');",
                    )
                })
                .collect::<Vec<_>>()
        })
    };
    for bomb in bombs {
        let (statuses, released) = bomb.join().unwrap();
        assert!(statuses.terminated(), "{statuses:?}");
        assert!(statuses.in_flight.v8_heap_emergency > 0, "{statuses:?}");
        assert_eq!(released.v8_heap_emergency, 0, "{released:?}");
        assert_eq!(released.live_isolates, 0, "{released:?}");
    }
    assert!(heap_emergency_stats().grants >= grants_before + 2);
    let benign = benign.join().unwrap();
    assert!(
        benign.iter().all(Statuses::completed),
        "the benign tenant was disturbed: {benign:?}"
    );
}
