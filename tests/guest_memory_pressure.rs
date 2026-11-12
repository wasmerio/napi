//! A guest's `unofficial_napi_memory_pressure` import reaches its env through
//! real Wasmer dispatch and the C++ bridge, with the level it passed. The
//! engine side is covered by `tests/runners/test_72_unofficial_memory_pressure.cc`.

#![cfg(not(target_arch = "wasm32"))]

use std::sync::mpsc;

use wasmer_napi::{NapiCtx, callback_record_counts};

mod wat_guest;
use wat_guest::{Guest, NAPI_INVALID_ARG, compile};

#[test]
fn memory_pressure_reaches_the_env_through_the_guest_import() {
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();

    let mut guest = Guest::new(&ctx, &engine, &module, signal_tx);
    let before = callback_record_counts();
    guest.create();
    // The fixture creates native callbacks, then retains them on its global.
    let live = callback_record_counts();
    assert_eq!(live.envs, before.envs + 1);
    assert!(live.registrations > before.registrations);
    assert!(live.bindings > before.bindings);
    assert!(live.payloads > before.payloads);
    assert_eq!(live.made - before.made, live.bindings - before.bindings);
    assert_eq!(guest.transient_callback(), 0);
    let transient = callback_record_counts();
    assert_eq!(transient.registrations, live.registrations + 1);
    assert_eq!(transient.bindings, live.bindings + 1);
    assert_eq!(transient.payloads, live.payloads + 1);
    assert_eq!(guest.collect(), 0);
    let collected = callback_record_counts();
    assert_eq!(collected.registrations, live.registrations);
    assert_eq!(collected.bindings, live.bindings);
    assert_eq!(collected.payloads, live.payloads);
    assert_eq!(collected.made, live.made + 1);
    assert_eq!(collected.freed, live.freed + 1);
    assert_eq!(guest.number("globalThis.kept = 41; kept + 1"), 42);

    // none, moderate, critical, moderate, none
    for level in [0, 1, 2, 1, 0] {
        assert_eq!(guest.memory_pressure(level), 0, "level {level}");
        assert_eq!(guest.number("kept + 1"), 42, "level {level}");
    }
    assert_eq!(guest.memory_pressure(-1), NAPI_INVALID_ARG);
    // Success or retry on an archive with the optional hook. Stock V8
    // reports napi_generic_failure and leaves the isolate usable.
    let preserve_code = guest.memory_pressure(3);
    assert!(matches!(preserve_code, 0 | 9 | 15), "status {preserve_code}");
    assert_eq!(guest.memory_pressure(4), NAPI_INVALID_ARG);
    assert_eq!(guest.number("kept + 1"), 42);

    assert_eq!(guest.release(), 0);
    assert_eq!(callback_record_counts(), before);
}
