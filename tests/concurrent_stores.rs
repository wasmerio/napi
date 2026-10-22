//! Environment ids are allocated per application rather than per Wasmer
//! store. A guest in one store (for example a WASIX worker thread) can stop or
//! interrupt an environment owned by another store of the same application;
//! every other N-API call still resolves only through the caller's own store.
//!
//! Each guest is a WAT module in its own store, so these exercise the real
//! import dispatch and the C++ bridge against real V8.

#![cfg(not(target_arch = "wasm32"))]

use std::{
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};

use wasmer_napi::NapiCtx;

mod wat_guest;
use wat_guest::{Guest, INTERRUPT_DATA, NAPI_INVALID_ARG, NAPI_PENDING_EXCEPTION, compile};

const TIMEOUT: Duration = Duration::from_secs(10);

/// Ids name environments and owner scopes across all stores of one
/// application, are not reused, and mean nothing to another application.
#[test]
fn env_ids_are_unique_per_application() {
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let mut first = Guest::new(&ctx, &engine, &module, signal_tx.clone());
    let mut second = Guest::new(&ctx, &engine, &module, signal_tx.clone());
    let first_id = first.create();
    let second_id = second.create();
    assert!(first_id > 0 && second_id > 0);
    assert_ne!(first_id, second_id, "two stores got the same env id");

    // A scope token from another store must not alias the caller's own.
    let second_scope = second.scope();
    assert_ne!(first.scope(), second_scope);
    assert_eq!(first.release_scope(second_scope), NAPI_INVALID_ARG);
    assert_eq!(first.number("6 * 7"), 42);

    assert_eq!(second.release(), 0);
    let mut third = Guest::new(&ctx, &engine, &module, signal_tx.clone());
    let third_id = third.create();
    assert!(third_id > second_id, "a retired env id was reused");

    // Another application keeps its own ids and cannot reach these envs.
    let other_ctx = NapiCtx::default();
    let mut other = Guest::new(&other_ctx, &engine, &module, signal_tx);
    let other_id = other.create();
    assert_ne!(other_id, third_id);
    assert_eq!(other.terminate(third_id), NAPI_INVALID_ARG);
    assert_eq!(
        third.number("6 * 7"),
        42,
        "another application stopped this env"
    );

    for guest in [&mut first, &mut third, &mut other] {
        assert_eq!(guest.release(), 0);
    }
}

/// A worker store's environment, running JavaScript on its own thread, can be
/// interrupted and terminated from the main store, which cannot otherwise use,
/// cancel or release it.
#[test]
fn another_store_can_interrupt_and_terminate_but_not_use_an_env() {
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let mut main = Guest::new(&ctx, &engine, &module, signal_tx.clone());
    let main_id = main.create();
    assert_eq!(
        main.eval("globalThis.shared = new SharedArrayBuffer(8); shared"),
        0
    );
    assert_eq!(main.message(), 0);
    let shared_token = main.token();

    let (ids_tx, ids_rx) = mpsc::channel();
    let (to_main, from_worker) = mpsc::channel();
    let (to_worker, from_main) = mpsc::channel();
    let worker = {
        let (ctx, engine, module) = (ctx.clone(), engine.clone(), module.clone());
        thread::spawn(move || {
            let mut worker = Guest::new(&ctx, &engine, &module, signal_tx);
            let id = worker.create();
            ids_tx.send((id, worker.scope())).unwrap();
            let token = from_main.recv_timeout(TIMEOUT).unwrap();
            assert_eq!(worker.take(token), 0);
            assert_eq!(worker.install(), 0);
            // Cell 0 tells the main store where this script is.
            let waited = worker.number(
                "const cell = new Int32Array(shared); Atomics.store(cell, 0, 1); \
                 Atomics.wait(cell, 1, 0, 10000) === 'ok' ? 42 : 0",
            );
            to_main.send(waited).unwrap();
            to_main.send(worker.interrupted()).unwrap();
            let spun = worker.eval(
                "Atomics.store(new Int32Array(shared), 0, 2); \
                 const end = Date.now() + 30000; while (Date.now() < end) {}",
            );
            to_main.send(spun).unwrap();
            // Only the owner may clear its termination and keep going.
            assert_eq!(worker.cancel(id), 0);
            assert_eq!(worker.number("6 * 7"), 42);
            assert_eq!(worker.release(), 0);
        })
    };

    let (worker_id, worker_scope) = ids_rx.recv_timeout(TIMEOUT).unwrap();
    assert!(worker_id > 0 && worker_id != main_id, "env ids collide");
    assert_eq!(main.undefined_in(worker_id), NAPI_INVALID_ARG);
    assert_eq!(main.cancel(worker_id), NAPI_INVALID_ARG);
    assert_eq!(main.release_scope(worker_scope), NAPI_INVALID_ARG);

    to_worker.send(shared_token).unwrap();
    let wait_until = |main: &mut Guest, value: i32| {
        let deadline = Instant::now() + TIMEOUT;
        while main.number("Atomics.load(new Int32Array(shared), 0)") != value {
            assert!(
                Instant::now() < deadline,
                "worker never reached step {value}"
            );
            thread::sleep(Duration::from_millis(1));
        }
    };
    wait_until(&mut main, 1);
    // The worker is inside JavaScript; this store keeps working.
    assert_eq!(main.number("40 + 2"), 42);
    assert_eq!(main.eval("({ answer: 42 })"), 0);
    assert_eq!(main.message(), 0);
    let other_token = main.token();
    assert_ne!(other_token, shared_token);
    main.drop_message(other_token);
    // The worker's interrupt callback must run on the worker, in the worker's
    // store, while its script is still waiting.
    assert_eq!(main.interrupt(worker_id), 0);
    let deadline = Instant::now() + TIMEOUT;
    while main.number("Atomics.notify(new Int32Array(shared), 1)") != 1 {
        assert!(Instant::now() < deadline, "could not wake the worker");
        thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(from_worker.recv_timeout(TIMEOUT).unwrap(), 42);
    assert_eq!(
        from_worker.recv_timeout(TIMEOUT).unwrap(),
        INTERRUPT_DATA,
        "the interrupt did not run in the worker's own store"
    );

    wait_until(&mut main, 2);
    assert_eq!(main.terminate(worker_id), 0);
    assert_eq!(
        from_worker.recv_timeout(TIMEOUT).unwrap(),
        NAPI_PENDING_EXCEPTION,
        "the worker's script was not terminated"
    );
    worker.join().expect("worker thread panicked");

    // Retired ids are rejected; the main env was never stopped.
    assert_eq!(main.terminate(worker_id), NAPI_INVALID_ARG);
    assert_eq!(main.interrupt(worker_id), NAPI_INVALID_ARG);
    assert_eq!(main.number("40 + 2"), 42);
    assert_eq!(main.take(shared_token), NAPI_INVALID_ARG);
    assert_eq!(main.release(), 0);
}
