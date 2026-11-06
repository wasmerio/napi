//! `wasmer_napi::set_foreground_task_waker` reaches the V8 platform through
//! the bridge. A guest's `Atomics.waitAsync`, woken by another guest's
//! `Atomics.notify` on another thread, resolves only at the waiting guest's
//! next checkpoint, so the host's waker must run, on the notifying thread,
//! when the notify queues that task. The platform side is covered by
//! `tests/src/v8_foreground_task_waker_test.cc`.
//!
//! The waker is process-wide, so this test needs a process to itself, which
//! is what a separate integration test binary with one test gives it.

#![cfg(not(target_arch = "wasm32"))]

use std::{
    mem::ManuallyDrop,
    sync::{Arc, Mutex, mpsc},
    thread::{self, ThreadId},
    time::Duration,
};

use wasmer_napi::NapiCtx;

mod wat_guest;
use wat_guest::{Guest, compile};

const TIMEOUT: Duration = Duration::from_secs(10);
/// More checkpoints than an idle env needs to run what V8 queued for it.
const MAX_CHECKPOINTS: usize = 64;

/// The threads the installed waker ran on, in order.
type WakerLog = Arc<Mutex<Vec<ThreadId>>>;

fn calls(log: &WakerLog) -> usize {
    log.lock().unwrap().len()
}

fn calls_on(log: &WakerLog, thread: ThreadId) -> usize {
    log.lock()
        .unwrap()
        .iter()
        .filter(|&&id| id == thread)
        .count()
}

/// Starts an `Atomics.waitAsync` on the shared cell; `woken` becomes 1 once
/// its promise resolves.
fn wait_async(page: &mut Guest) {
    assert_eq!(
        page.eval(
            "globalThis.woken = 0; \
             Atomics.waitAsync(new Int32Array(shared), 0, 0).value.then(() => { woken = 1; });"
        ),
        0
    );
}

/// Runs checkpoints until the waitAsync promise has resolved.
fn checkpoint_until_woken(page: &mut Guest) -> bool {
    for _ in 0..MAX_CHECKPOINTS {
        assert_eq!(page.checkpoint(), 0);
        if page.number("woken") == 1 {
            return true;
        }
    }
    false
}

/// Records a waker call like the installed wakers do, and panics when
/// dropped.
struct PanicsOnDrop(WakerLog);

impl PanicsOnDrop {
    fn record(&self) {
        self.0.lock().unwrap().push(thread::current().id());
    }
}

impl Drop for PanicsOnDrop {
    fn drop(&mut self) {
        panic!("a waker's capture that panics when dropped");
    }
}

/// What the notifying guest saw: how many waiters `Atomics.notify` woke, and
/// how many times the waker ran on its thread meanwhile.
struct Notified {
    woke: i32,
    waker_calls: usize,
}

#[test]
fn waker_runs_when_another_thread_queues_a_task_for_a_waiting_guest() {
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let log: WakerLog = Arc::default();

    // Dropped only once the test has passed. A regression can leave the
    // notifying guest stuck inside V8 (a waker that deadlocks, say), and
    // dropping this guest while the test fails would then wait on it forever.
    let mut page = ManuallyDrop::new(Guest::new(&ctx, &engine, &module, signal_tx.clone()));
    page.create();
    assert_eq!(
        page.eval("globalThis.shared = new SharedArrayBuffer(8); shared"),
        0
    );
    assert_eq!(page.message(), 0);
    let token = page.token();

    // A guest on another thread that notifies the shared cell on request.
    let (notify_tx, notify_rx) = mpsc::channel::<()>();
    let (notified_tx, notified_rx) = mpsc::channel::<Notified>();
    let notifier = {
        let (ctx, engine, module, log) = (ctx.clone(), engine.clone(), module.clone(), log.clone());
        thread::spawn(move || {
            let mut guest = Guest::new(&ctx, &engine, &module, signal_tx);
            guest.create();
            assert_eq!(guest.take(token), 0);
            assert_eq!(guest.install(), 0);
            let me = thread::current().id();
            while notify_rx.recv().is_ok() {
                let before = calls_on(&log, me);
                let woke = guest.number("Atomics.notify(new Int32Array(shared), 0)");
                let waker_calls = calls_on(&log, me) - before;
                if notified_tx.send(Notified { woke, waker_calls }).is_err() {
                    break;
                }
            }
            assert_eq!(guest.release(), 0);
        })
    };
    let notify = || {
        notify_tx.send(()).expect("the notifying guest is gone");
        notified_rx
            .recv_timeout(TIMEOUT)
            .expect("the notifying guest did not answer")
    };

    // Run what V8 queued while the env started, before the waker is set.
    for _ in 0..MAX_CHECKPOINTS {
        assert_eq!(page.checkpoint(), 0);
    }

    {
        let log = log.clone();
        wasmer_napi::set_foreground_task_waker(Some(Arc::new(move || {
            log.lock().unwrap().push(thread::current().id());
        })));
    }
    wait_async(&mut page);
    let notified = notify();
    assert_eq!(notified.woke, 1, "the waitAsync was not waiting");
    assert!(
        notified.waker_calls >= 1,
        "the waker did not run on the notifying thread when the notify queued \
         the waiting guest's task"
    );
    // Only a checkpoint runs the task that resolves the promise.
    assert_eq!(page.number("woken"), 0);
    assert!(
        checkpoint_until_woken(&mut page),
        "the waitAsync promise did not resolve"
    );

    // Without a waker nothing is called, and a checkpoint still runs the task.
    wasmer_napi::set_foreground_task_waker(None);
    let before = calls(&log);
    wait_async(&mut page);
    assert_eq!(notify().woke, 1);
    assert_eq!(calls(&log), before, "a removed waker ran");
    assert!(
        checkpoint_until_woken(&mut page),
        "the waitAsync promise did not resolve without a waker"
    );

    // A waker that panics: the panic stays out of V8, and the task still runs.
    {
        let log = log.clone();
        wasmer_napi::set_foreground_task_waker(Some(Arc::new(move || {
            log.lock().unwrap().push(thread::current().id());
            panic!("a waker that panics");
        })));
    }
    wait_async(&mut page);
    let notified = notify();
    assert_eq!(notified.woke, 1);
    assert!(notified.waker_calls >= 1, "the panicking waker did not run");
    assert!(
        checkpoint_until_woken(&mut page),
        "the waitAsync promise did not resolve after the waker panicked"
    );

    // A waker that removes itself and then panics, holding a capture that
    // panics when dropped. It runs with no lock held, so it may call the
    // setter. Once removed, the trampoline holds the last reference to it,
    // and drops it after the call: if that drop came while the waker's panic
    // unwound, the capture's panic would abort the process. Whichever thread
    // calls it first removes it, so its calls are counted on every thread.
    let before = calls(&log);
    {
        let capture = PanicsOnDrop(log.clone());
        wasmer_napi::set_foreground_task_waker(Some(Arc::new(move || {
            capture.record();
            wasmer_napi::set_foreground_task_waker(None);
            panic!("a waker that removed itself");
        })));
    }
    wait_async(&mut page);
    assert_eq!(notify().woke, 1);
    assert!(
        calls(&log) > before,
        "the waker that removes itself did not run"
    );
    assert!(
        checkpoint_until_woken(&mut page),
        "the waitAsync promise did not resolve after the waker removed itself"
    );
    let before = calls(&log);
    wait_async(&mut page);
    assert_eq!(notify().woke, 1);
    assert_eq!(calls(&log), before, "the waker did not remove itself");
    assert!(
        checkpoint_until_woken(&mut page),
        "the waitAsync promise did not resolve once the waker removed itself"
    );

    drop(notify_tx);
    notifier.join().expect("the notifying guest panicked");
    assert_eq!(page.release(), 0);
    drop(ManuallyDrop::into_inner(page));
}
