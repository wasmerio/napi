//! Metered wasm code with V8's dynamic tiering (`liftoff_only: false`):
//! optimized code replaces baseline code, V8 frees the dead baseline code,
//! and the charge follows. Own binary: tiering is a process-wide setting.

use std::sync::Arc;

#[path = "common/v8_lane.rs"]
mod v8_lane;
use v8_lane::{Caps, Lane, MIB, configure_engine};

#[test]
fn code_freed_after_tier_up_is_uncharged() {
    let mut limits = wasmer_napi::WasmEngineLimits::default();
    limits.liftoff_only = false;
    configure_engine(&limits);
    let lane = Lane::new(512 * MIB, Some(Caps::default()));
    let acct = Arc::clone(&lane.accountant);
    let env = lane.env();
    env.load_wasmgen();
    let before = wasmer_napi::wasm_process_stats();
    // Hot functions tier up on the context's lane; the baseline code they
    // leave behind is collected by V8's wasm code GC and decommitted.
    env.eval(
        "globalThis.inst = new WebAssembly.Instance(new WebAssembly.Module(wasmgen.loads(128, 400)));
         for (let round = 0; round < 200; round++)
           for (let n = 0; n < 128; n++) inst.exports['f' + n](0);
         ''",
    );
    let peak = acct.code();
    // Let background tier-up finish and code GC run.
    let mut decommitted = 0;
    for _ in 0..50 {
        env.eval(
            "for (let round = 0; round < 20; round++)
               for (let n = 0; n < 128; n++) inst.exports['f' + n](0);
             ''",
        );
        env.gc_until(|| true);
        decommitted = wasmer_napi::wasm_process_stats().code_decommitted_bytes
            - before.code_decommitted_bytes;
        if decommitted > 0 {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
    let after = acct.code();
    println!("peak charge {peak}, decommitted {decommitted}, charge after churn {after}");
    assert!(decommitted > 0, "V8 never decommitted code after tier-up");
    assert_eq!(lane.usage().code_bytes, after);
    assert!(after > 0 && after <= peak + 4 * MIB);
    env.release();
    assert_eq!(acct.code(), 0, "tier-up churn leaked code charge");
    lane.finish();
}
