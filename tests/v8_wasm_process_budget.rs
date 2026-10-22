//! The process-wide budget for metered wasm code. Once the process's
//! committed wasm code reaches the soft budget, new compilations in metered
//! contexts are refused with a CompileError. Lazy compilation of modules
//! admitted earlier cannot be refused, so a context whose commit takes the
//! process past the hard limit (twice the soft budget) is stopped. Both keep
//! V8's own process-wide limit, whose breach aborts the process, out of
//! reach. Own binary with a single sequential test: the budget is
//! process-wide.

use std::{sync::Arc, thread};

#[path = "common/v8_lane.rs"]
mod v8_lane;
use v8_lane::{Caps, Lane, MIB, NAPI_PENDING_EXCEPTION, configure_engine, outcome};

const SOFT: u64 = 4 * MIB;
const HARD: u64 = 2 * SOFT;
/// `SNAPI_V8_WASM_CODE_LIMIT_PROCESS`.
const LIMIT_PROCESS: u32 = 3;
/// Baseline code of one `wasmgen.loads(_, 400)` function is about 13 KB.
const ONE_FUNCTION: u64 = 64 * 1024;

fn committed() -> u64 {
    wasmer_napi::wasm_process_stats().code_committed_bytes
}

fn metered() -> Lane {
    Lane::new(512 * MIB, Some(Caps::default()))
}

#[test]
fn process_code_budget_refuses_compiles_and_stops_lazy_compilation() {
    let mut limits = wasmer_napi::WasmEngineLimits::default();
    limits.process_code_budget_bytes = SOFT;
    configure_engine(&limits);
    // Applied when V8 initializes, with the first environment.
    assert_eq!(wasmer_napi::wasm_process_stats().code_budget_bytes, 0);

    soft_budget_refuses_new_compilations_gracefully();
    lazy_compilation_past_the_soft_budget_is_stopped_at_the_hard_limit();
    concurrent_contexts_stop_at_the_hard_limit();
}

fn soft_budget_refuses_new_compilations_gracefully() {
    // Two contexts, each far below its own code budget.
    let first = metered();
    let second = metered();
    let a = first.env();
    let b = second.env();
    let stats = wasmer_napi::wasm_process_stats();
    assert_eq!(
        (stats.code_budget_bytes, stats.code_hard_limit_bytes),
        (SOFT, HARD)
    );
    a.load_wasmgen();
    b.load_wasmgen();
    let fill = "globalThis.kept = globalThis.kept || [];
                { const i = new WebAssembly.Instance(new WebAssembly.Module(wasmgen.loads(32, 400)));
                  for (let n = 0; n < 32; n++) i.exports['f' + n](0);
                  kept.push(i); } ''";
    let denials = wasmer_napi::wasm_process_stats().codegen_denials;
    // Fill the soft budget from the first context.
    let mut compiled = 0;
    while committed() < SOFT {
        a.eval(fill);
        compiled += 1;
        assert!(compiled < 64, "the process budget never filled");
    }
    assert!(committed() < HARD);
    // Both contexts are refused new compilations, gracefully.
    for env in [&a, &b] {
        assert_eq!(
            env.eval(&outcome("new WebAssembly.Module(wasmgen.add())")),
            "CompileError"
        );
        assert_eq!(
            env.eval(
                "(() => { try { new WebAssembly.Module(wasmgen.add()); } catch (e) { return e.message; } })()"
            ),
            "WebAssembly.Module(): Wasm code generation disallowed by embedder"
        );
    }
    assert!(wasmer_napi::wasm_process_stats().codegen_denials >= denials + 4);
    // Nobody was stopped; existing code keeps running.
    assert!(first.accountant.code_limits.lock().unwrap().is_empty());
    assert_eq!(a.eval("String(kept[0].exports.f0(0))"), "0");

    // Releasing code reopens the gate.
    let acct = Arc::clone(&first.accountant);
    a.eval("kept = []; ''");
    assert!(a.gc_until(|| acct.code() == 0));
    assert_eq!(
        b.eval(&outcome("new WebAssembly.Module(wasmgen.add())")),
        "ok"
    );
    // Environments of one thread are entered and must be released LIFO.
    b.release();
    a.release();
    first.finish();
    second.finish();
}

/// Regression: a module admitted below the soft budget compiles its
/// functions lazily, past the soft budget, with no gate in between. The
/// committing context must be stopped at the hard limit instead.
fn lazy_compilation_past_the_soft_budget_is_stopped_at_the_hard_limit() {
    let lane = metered();
    let acct = Arc::clone(&lane.accountant);
    let env = lane.env();
    env.load_wasmgen();
    // 3000 functions of about 13 KB of baseline code: 39 MB if all compile.
    env.eval(
        "globalThis.e = new WebAssembly.Instance(new WebAssembly.Module(wasmgen.loads(3000, 400))).exports; ''",
    );
    let mut peak = committed();
    assert!(peak < SOFT, "the module was admitted below the soft budget");
    let mut stopped_after = None;
    for n in 0..3000 {
        let result = env.try_eval(&format!("e.f{n}(0); ''"));
        peak = peak.max(committed());
        if result.is_err() {
            assert_eq!(result, Err(NAPI_PENDING_EXCEPTION));
            stopped_after = Some(n);
            break;
        }
    }
    let limits = acct.code_limits.lock().unwrap().clone();
    println!("lazy compilation stopped after {stopped_after:?} functions, peak {peak}, {limits:?}");
    assert!(
        stopped_after.is_some(),
        "lazy compilation ran to {peak} bytes past the {HARD}-byte hard limit"
    );
    assert_eq!(limits.len(), 1, "{limits:?}");
    assert_eq!((limits[0].0, limits[0].2), (LIMIT_PROCESS, HARD));
    assert!(limits[0].1 > HARD);
    assert!(peak <= HARD + ONE_FUNCTION, "peak {peak}");
    // The stopped context cannot compile again even if it cancels the stop.
    env.cancel_terminate();
    assert_eq!(
        env.eval(&outcome("new WebAssembly.Module(wasmgen.add())")),
        "CompileError"
    );
    env.release();
    assert!(committed() < SOFT, "teardown returned the code");
    lane.finish();
}

/// Two contexts lazily compiling toward the hard limit at once: whoever
/// crosses it is stopped, the process stays bounded and alive.
fn concurrent_contexts_stop_at_the_hard_limit() {
    let lanes = [metered(), metered()];
    let results: Vec<(Option<usize>, u64)> = thread::scope(|scope| {
        let workers: Vec<_> = lanes
            .iter()
            .map(|lane| {
                scope.spawn(move || {
                    let env = lane.env();
                    env.load_wasmgen();
                    // About 5.2 MB each: together past the hard limit.
                    env.eval(
                        "globalThis.e = new WebAssembly.Instance(new WebAssembly.Module(wasmgen.loads(400, 400))).exports; ''",
                    );
                    let mut peak = 0;
                    let mut stopped_after = None;
                    for n in 0..400 {
                        let result = env.try_eval(&format!("e.f{n}(0); ''"));
                        peak = peak.max(committed());
                        if result.is_err() {
                            stopped_after = Some(n);
                            break;
                        }
                    }
                    env.release();
                    (stopped_after, peak)
                })
            })
            .collect();
        workers.into_iter().map(|w| w.join().unwrap()).collect()
    });
    println!("concurrent contexts: {results:?}");
    assert!(
        results.iter().any(|(stopped, _)| stopped.is_some()),
        "neither context was stopped"
    );
    for (_, peak) in &results {
        assert!(*peak <= HARD + 2 * ONE_FUNCTION, "peak {peak}");
    }
    for lane in &lanes {
        for (reason, _, limit) in lane.accountant.code_limits.lock().unwrap().iter() {
            assert_eq!((*reason, *limit), (LIMIT_PROCESS, HARD));
        }
    }
    let [first, second] = lanes;
    first.finish();
    second.finish();
}
