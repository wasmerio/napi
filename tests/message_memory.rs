#![cfg(not(target_arch = "wasm32"))]

//! A `SharedArrayBuffer` V8 allocates for a guest lives in that guest's
//! memory, and a message that carries one points into it. Only an
//! environment on the same memory may receive it: another memory's guest
//! could not address the bytes, and the sender's store owns the mapping. The
//! queue leases a shared memory while the message waits, so the bytes stay
//! mapped after the sender's store is gone.

use std::sync::mpsc;

use wasmer_napi::NapiCtx;

mod wat_guest;
use wat_guest::{Guest, NAPI_INVALID_ARG, NAPI_PENDING_EXCEPTION, compile, compile_shared};

#[test]
fn taking_a_copied_arraybuffer_grows_the_receivers_memory() {
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let mut sender = Guest::new(&ctx, &engine, &module, signal_tx.clone());
    sender.create();
    // A fresh receiver's prefunded arena is 16 MiB.
    assert_eq!(
        sender.eval(
            "const bytes = new Uint8Array(32 << 20);
             bytes[0] = 17;
             bytes[bytes.length - 1] = 83;
             bytes.buffer"
        ),
        0
    );
    assert_eq!(sender.message(), 0);

    let mut receiver = Guest::new(&ctx, &engine, &module, signal_tx);
    receiver.create();
    let before = receiver.memory_size();
    assert_eq!(receiver.take(sender.token()), 0);
    assert!(receiver.memory_size() > before);
    assert_eq!(receiver.install(), 0);
    assert_eq!(receiver.number("shared.byteLength"), 32 << 20);
    assert_eq!(receiver.number("new Uint8Array(shared)[0]"), 17);
    assert_eq!(receiver.number("new Uint8Array(shared).at(-1)"), 83);
    assert_eq!(receiver.release(), 0);
    assert_eq!(sender.release(), 0);
}

/// A guest on another memory cannot take it; the refusal consumes it.
#[test]
fn a_shared_buffer_in_another_memory_is_refused() {
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let mut sender = Guest::new(&ctx, &engine, &module, signal_tx.clone());
    sender.create();
    assert_eq!(
        sender.eval("globalThis.shared = new SharedArrayBuffer(8); shared"),
        0
    );
    assert_eq!(sender.message(), 0);
    let token = sender.token();

    let mut receiver = Guest::new(&ctx, &engine, &module, signal_tx);
    receiver.create();
    assert_eq!(receiver.take(token), NAPI_PENDING_EXCEPTION);
    // Consumed: nobody can take it any more.
    assert_eq!(receiver.take(token), NAPI_INVALID_ARG);
    assert_eq!(sender.take(token), NAPI_INVALID_ARG);

    // A plain value, and a growable buffer (V8 keeps its bytes in its own
    // pages), still cross memories.
    assert_eq!(
        sender.eval("({ answer: 42, grow: new SharedArrayBuffer(8, { maxByteLength: 16 }) })"),
        0
    );
    assert_eq!(sender.message(), 0);
    let token = sender.token();
    assert_eq!(receiver.take(token), 0);
    assert_eq!(receiver.install(), 0);
    assert_eq!(receiver.number("shared.answer"), 42);

    assert_eq!(receiver.release(), 0);
    assert_eq!(sender.release(), 0);
}

/// Envs on one shared memory (an instance and its WASIX threads) share the
/// buffer, also after the sender's store is gone.
#[test]
fn a_shared_buffer_reaches_a_receiver_on_the_same_memory() {
    let (engine, module) = compile_shared();
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let mut sender = Guest::new_sharing(&ctx, &engine, &module, signal_tx.clone(), None);
    let memory = sender.shared_memory();
    sender.create();
    assert_eq!(
        sender.eval(
            "globalThis.shared = new SharedArrayBuffer(8); \
             Atomics.store(new Int32Array(shared), 0, 41); shared"
        ),
        0
    );
    assert_eq!(sender.message(), 0);
    let token = sender.token();
    // The sender's env and store go away; the queued message keeps the
    // memory mapped.
    assert_eq!(sender.release(), 0);
    drop(sender);

    let mut receiver = Guest::new_sharing(&ctx, &engine, &module, signal_tx, Some(memory));
    receiver.create();
    assert_eq!(receiver.take(token), 0);
    assert_eq!(receiver.install(), 0);
    assert_eq!(
        receiver.number("Atomics.add(new Int32Array(shared), 0, 1) + 1"),
        42
    );
    assert_eq!(receiver.release(), 0);
}

/// A context that stops frees its queued messages, leases included, and the
/// budget gets their bytes back.
#[test]
fn stopping_the_context_frees_a_queued_shared_buffer() {
    let (engine, module) = compile_shared();
    let ctx = NapiCtx::builder()
        .total_memory_bytes(1024 * 1024 * 1024)
        .build();
    let budget = ctx.budget();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let mut sender = Guest::new_sharing(&ctx, &engine, &module, signal_tx.clone(), None);
    // Held past the sender, as a WASIX thread would.
    let memory = sender.shared_memory();
    sender.create();
    assert_eq!(sender.eval("new SharedArrayBuffer(64)"), 0);
    assert_eq!(sender.message(), 0);
    let token = sender.token();
    assert_eq!(sender.release(), 0);
    drop(sender);
    assert!(
        budget.snapshot().serialized_message > 0,
        "a queued message is not charged"
    );
    ctx.runtime_control().terminate_all();
    assert_eq!(budget.snapshot().serialized_message, 0);
    drop((memory, token, signal_tx));
}
