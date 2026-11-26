#![cfg(not(target_arch = "wasm32"))]

//! Copying an ArrayBuffer can grow the guest heap. A transfer detaches its
//! source, and a queued message owns its serialized bytes until delivery or
//! drop. SharedArrayBuffers additionally lease the sender's memory and can
//! only be received by a guest that can address that memory.

use std::sync::mpsc;

use wasmer_napi::NapiCtx;

mod wat_guest;
use wat_guest::{Guest, NAPI_INVALID_ARG, NAPI_PENDING_EXCEPTION, compile, compile_shared};

// Even a completely unused arena cannot hold a buffer larger than the guest's
// entire current linear memory. Keep the payload at least as large as in #156.
fn buffer_size(guest: &Guest) -> i32 {
    i32::try_from(guest.memory_size() + 1)
        .unwrap()
        .max(32 << 20)
}

fn make_buffer(guest: &mut Guest, size: i32) {
    assert_eq!(
        guest.eval(&format!(
            "const bytes = new Uint8Array({size});
         bytes[0] = 17;
         bytes[bytes.length - 1] = 83;
         bytes.buffer"
        )),
        0
    );
}

fn check_buffer(guest: &mut Guest, size: i32) {
    assert_eq!(guest.install(), 0);
    assert_eq!(guest.number("shared.byteLength"), size);
    assert_eq!(guest.number("new Uint8Array(shared)[0]"), 17);
    assert_eq!(guest.number("new Uint8Array(shared).at(-1)"), 83);
}

#[test]
fn taking_a_copied_arraybuffer_grows_the_receivers_memory() {
    deliver_buffer(false);
}

#[test]
fn taking_a_transferred_arraybuffer_grows_the_receivers_memory() {
    deliver_buffer(true);
}

fn deliver_buffer(transfer: bool) {
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let mut sender = Guest::new(&ctx, &engine, &module, signal_tx.clone());
    let mut receiver = Guest::new(&ctx, &engine, &module, signal_tx);
    sender.create();
    receiver.create();
    let size = buffer_size(&receiver);
    make_buffer(&mut sender, size);
    if transfer {
        // Edge.js applies the transfer list before it creates the queue message.
        assert_eq!(sender.clone_buffer(true), 0);
        assert_eq!(sender.install(), 0);
        assert_eq!(sender.number("bytes.byteLength"), 0);
        assert_eq!(sender.eval("shared"), 0);
    }
    assert_eq!(sender.message(), 0);
    let before = receiver.memory_size();
    assert_eq!(receiver.take(sender.token()), 0);
    assert!(receiver.memory_size() > before);
    check_buffer(&mut receiver, size);
    assert_eq!(receiver.release(), 0);
    assert_eq!(sender.release(), 0);
}

#[test]
fn structured_clone_can_grow_the_guest_heap() {
    let (engine, module) = compile();
    let (signal, _) = mpsc::channel();
    let mut guest = Guest::new(&NapiCtx::default(), &engine, &module, signal);
    guest.create();
    let size = buffer_size(&guest);
    make_buffer(&mut guest, size);
    let before = guest.memory_size();
    assert_eq!(guest.clone_buffer(false), 0);
    assert!(guest.memory_size() > before);
    check_buffer(&mut guest, size);
    assert_eq!(guest.number("bytes[0] = 0; new Uint8Array(shared)[0]"), 17);
    assert_eq!(guest.release(), 0);
}

#[test]
fn a_serialization_getter_can_queue_a_second_message() {
    let (engine, module) = compile();
    let (signal, _) = mpsc::channel();
    let mut guest = Guest::new(&NapiCtx::default(), &engine, &module, signal);
    guest.create();
    assert_eq!(guest.install_post(), 0);
    assert_eq!(guest.eval("({get answer() { post(17); return 83; }})"), 0);
    assert_eq!(guest.message(), 0);
    let outer = guest.token();
    let inner = guest.nested_token();
    assert_eq!(guest.take(inner), 0);
    assert_eq!(guest.install(), 0);
    assert_eq!(guest.number("shared"), 17);
    assert_eq!(guest.take(outer), 0);
    assert_eq!(guest.install(), 0);
    assert_eq!(guest.number("shared.answer"), 83);
    assert_eq!(guest.release(), 0);
}

#[test]
fn a_guest_exit_in_a_getter_releases_the_unqueued_message() {
    let (engine, module) = compile();
    let ctx = NapiCtx::builder().total_memory_bytes(1 << 30).build();
    let mut guest = Guest::with_signal_hook(&ctx, &engine, &module, || {
        Err(wat_guest::GuestExit::raise(7))
    });
    guest.create();
    // An ordinary getter with the same result establishes that serialization
    // creates a charged handle and can finish when signal() returns undefined.
    assert_eq!(guest.eval("({get answer() { return undefined; }})"), 0);
    assert_eq!(guest.message(), 0);
    let token = guest.token();
    assert!(ctx.budget().snapshot().serialized_message > 0);
    guest.drop_message(token);
    assert_eq!(ctx.budget().snapshot().serialized_message, 0);

    assert_eq!(guest.eval("({get answer() { return signal(); }})"), 0);
    let exit = guest.try_message().unwrap_err();
    assert_eq!(wat_guest::GuestExit::status(&exit), Some(7));
    assert_eq!(ctx.budget().snapshot().serialized_message, 0);
    assert_eq!(guest.release(), 0);
}

#[test]
fn stopping_the_context_in_a_getter_releases_queued_messages() {
    let (engine, module) = compile();
    let ctx = NapiCtx::builder().total_memory_bytes(1 << 30).build();
    let control = ctx.runtime_control();
    let mut guest = Guest::with_signal_hook(&ctx, &engine, &module, move || {
        control.terminate_all();
        Ok(())
    });
    guest.create();
    assert_eq!(guest.eval("17"), 0);
    assert_eq!(guest.message(), 0);
    let token = guest.token();
    assert!(ctx.budget().snapshot().serialized_message > 0);
    assert_eq!(guest.eval("({get answer() { signal(); return 83; }})"), 0);
    assert_ne!(guest.message(), 0);
    assert_eq!(ctx.budget().snapshot().serialized_message, 0);
    assert_eq!(guest.take(token), NAPI_INVALID_ARG);
    assert_eq!(guest.release(), 0);
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
