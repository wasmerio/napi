//! A guest's `unofficial_napi_memory_pressure` import reaches its env through
//! real Wasmer dispatch and the C++ bridge, with the level it passed. The
//! engine side is covered by `tests/runners/test_72_unofficial_memory_pressure.cc`.

#![cfg(not(target_arch = "wasm32"))]

use std::sync::mpsc;

use wasmer_napi::NapiCtx;

mod wat_guest;
use wat_guest::{Guest, NAPI_INVALID_ARG, compile};

#[test]
fn memory_pressure_reaches_the_env_through_the_guest_import() {
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();

    let mut guest = Guest::new(&ctx, &engine, &module, signal_tx);
    guest.create();
    assert_eq!(guest.number("globalThis.kept = 41; kept + 1"), 42);

    // none, moderate, critical, moderate, none
    for level in [0, 1, 2, 1, 0] {
        assert_eq!(guest.memory_pressure(level), 0, "level {level}");
        assert_eq!(guest.number("kept + 1"), 42, "level {level}");
    }
    assert_eq!(guest.memory_pressure(-1), NAPI_INVALID_ARG);
    assert_eq!(guest.memory_pressure(3), NAPI_INVALID_ARG);
    assert_eq!(guest.number("kept + 1"), 42);

    assert_eq!(guest.release(), 0);
}
