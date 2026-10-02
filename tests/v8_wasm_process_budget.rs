//! The process-wide soft budget for metered wasm code: once the process's
//! committed wasm code reaches it, new compilations in metered contexts are
//! refused with a CompileError instead of letting V8 reach its own
//! process-wide limit, whose breach aborts the process. Own binary: the
//! budget is process-wide.

use std::sync::Arc;

#[path = "common/v8_lane.rs"]
mod v8_lane;
use v8_lane::{Caps, Lane, MIB, configure_engine, outcome};

#[test]
fn exhausted_process_code_budget_refuses_compiles_gracefully() {
    let mut limits = wasmer_napi::WasmEngineLimits::default();
    limits.process_code_budget_bytes = MIB;
    configure_engine(&limits);
    // Applied when V8 initializes, with the first environment.
    assert_eq!(wasmer_napi::wasm_process_stats().code_budget_bytes, 0);

    // Two contexts, each far below its own code budget.
    let first = Lane::new(256 * MIB, Some(Caps::default()));
    let second = Lane::new(256 * MIB, Some(Caps::default()));
    let a = first.env();
    let b = second.env();
    assert_eq!(wasmer_napi::wasm_process_stats().code_budget_bytes, MIB);
    a.load_wasmgen();
    b.load_wasmgen();
    let fill = "globalThis.kept = globalThis.kept || [];
                { const i = new WebAssembly.Instance(new WebAssembly.Module(wasmgen.loads(32, 400)));
                  for (let n = 0; n < 32; n++) i.exports['f' + n](0);
                  kept.push(i); } ''";
    let denials = wasmer_napi::wasm_process_stats().codegen_denials;
    // Fill the process budget from the first context.
    let mut compiled = 0;
    while wasmer_napi::wasm_process_stats().code_committed_bytes < MIB {
        a.eval(fill);
        compiled += 1;
        assert!(compiled < 64, "the process budget never filled");
    }
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
    a.release();
    b.release();
    first.finish();
    second.finish();
}
