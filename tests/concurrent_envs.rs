//! Environments owned by different threads execute independently: one
//! environment running JavaScript must not stop another from making N-API
//! calls, and a thread-safe stop must reach an environment while it runs.
//!
//! Each guest is a WAT module in its own Wasmer store, so these exercise the
//! real import dispatch and the C++ bridge against real V8.

#![cfg(not(target_arch = "wasm32"))]

use std::{
    ffi::{c_char, c_void},
    sync::{Arc, Barrier, mpsc},
    thread,
    time::{Duration, Instant},
};

use wasmer::{Engine, Module};
use wasmer_napi::NapiCtx;

mod wat_guest;
use wat_guest::{Guest, NAPI_INVALID_ARG, NAPI_PENDING_EXCEPTION, compile};

/// Marks cell 1, then parks the script until another environment wakes it,
/// for up to 10 s. A bridge that serializes environments keeps the other
/// environment out until the wait times out. Some of these entry points do not
/// install a guest callback context, so readiness is reported through the
/// shared memory rather than through `signal()`.
const WAIT_SCRIPT: &str = "const cell = new Int32Array(shared); \
     Atomics.store(cell, 1, 1); \
     globalThis.woke = Atomics.wait(cell, 0, 0, 10000);";
const TIMEOUT: Duration = Duration::from_secs(10);

/// Bridge entry points that run JavaScript and that used to hold the bridge's
/// process-wide lock while doing so.
#[derive(Clone, Copy, Debug)]
enum Entry {
    ContextifyRunScript,
    ModuleEvaluate,
    MicrotaskCheckpoint,
    SerializerGetter,
}

/// Runs `WAIT_SCRIPT` in a guest on another thread through `entry`, and wakes
/// it from a guest on this thread while it is still inside JavaScript.
fn wait_in_one_env_and_notify_from_another(engine: &Engine, module: &Module, entry: Entry) {
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let mut notifier = Guest::new(&ctx, engine, module, signal_tx.clone());
    notifier.create();
    assert_eq!(
        notifier.eval("globalThis.shared = new SharedArrayBuffer(8); shared"),
        0
    );
    assert_eq!(notifier.message(), 0);
    let token = notifier.token();

    let waiter = {
        let (ctx, engine, module) = (ctx.clone(), engine.clone(), module.clone());
        thread::spawn(move || {
            let mut guest = Guest::new(&ctx, &engine, &module, signal_tx);
            guest.create();
            assert_eq!(guest.take(token), 0);
            assert_eq!(guest.install(), 0);
            match entry {
                Entry::ContextifyRunScript => assert_eq!(guest.eval_contextify(WAIT_SCRIPT), 0),
                Entry::ModuleEvaluate => assert_eq!(guest.eval_module(WAIT_SCRIPT), 0),
                Entry::MicrotaskCheckpoint => {
                    let queued = format!("Promise.resolve().then(() => {{ {WAIT_SCRIPT} }})");
                    assert_eq!(guest.eval(&queued), 0);
                    assert_eq!(guest.checkpoint(), 0);
                }
                Entry::SerializerGetter => {
                    let getter = format!("({{ get value() {{ {WAIT_SCRIPT} return 42; }} }})");
                    assert_eq!(guest.eval(&getter), 0);
                    assert_eq!(guest.message(), 0);
                    let clone = guest.token();
                    assert_eq!(guest.take(clone), 0);
                    assert_eq!(guest.install(), 0);
                    assert_eq!(guest.number("shared.value"), 42, "getter result lost");
                }
            }
            let woke = guest.number("woke === 'ok' ? 1 : 0");
            assert_eq!(guest.release(), 0);
            woke
        })
    };

    // Every poll is a script in this environment, run while the other one is
    // inside JavaScript.
    let deadline = Instant::now() + TIMEOUT;
    while notifier.number("Atomics.load(new Int32Array(shared), 1)") != 1 {
        assert!(
            Instant::now() < deadline,
            "{entry:?}: the waiting guest never reached JavaScript"
        );
        thread::sleep(Duration::from_millis(1));
    }
    // Keep notifying until the waiter is actually parked.
    loop {
        if notifier.number("Atomics.notify(new Int32Array(shared), 0)") == 1 {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "{entry:?}: could not wake the other environment; did its \
             JavaScript block this one?"
        );
        thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(
        waiter.join().expect("waiting guest panicked"),
        1,
        "{entry:?}: the waiting script timed out instead of being woken"
    );
    assert_eq!(notifier.release(), 0);
}

#[test]
fn a_contextified_script_does_not_block_other_envs() {
    let (engine, module) = compile();
    wait_in_one_env_and_notify_from_another(&engine, &module, Entry::ContextifyRunScript);
}

#[test]
fn a_module_evaluation_does_not_block_other_envs() {
    let (engine, module) = compile();
    wait_in_one_env_and_notify_from_another(&engine, &module, Entry::ModuleEvaluate);
}

#[test]
fn a_microtask_checkpoint_does_not_block_other_envs() {
    let (engine, module) = compile();
    wait_in_one_env_and_notify_from_another(&engine, &module, Entry::MicrotaskCheckpoint);
}

#[test]
fn a_serializer_getter_does_not_block_other_envs() {
    let (engine, module) = compile();
    wait_in_one_env_and_notify_from_another(&engine, &module, Entry::SerializerGetter);
}

/// The host stop (`NapiRuntimeControl::terminate_all`) runs on a host thread,
/// not on the thread that owns the environment, and must not wait for the
/// running script to give anything up.
#[test]
fn host_terminate_reaches_an_env_inside_a_contextified_script() {
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let control = ctx.runtime_control();
    let (signal_tx, signal_rx) = mpsc::channel();
    let (status_tx, status_rx) = mpsc::channel();
    let guest = thread::spawn(move || {
        let mut guest = Guest::new(&ctx, &engine, &module, signal_tx);
        guest.create();
        // Bounded, so a bridge that cannot stop it fails the test instead of
        // spinning forever with every other environment locked out.
        let status = guest.eval_contextify(
            "signal(); const end = Date.now() + 30000; while (Date.now() < end) {}",
        );
        let _ = status_tx.send(status);
        assert_eq!(guest.release(), 0);
    });

    signal_rx
        .recv_timeout(TIMEOUT)
        .expect("the guest never reached JavaScript");
    let (stopped_tx, stopped_rx) = mpsc::channel();
    thread::spawn(move || {
        control.terminate_all();
        let _ = stopped_tx.send(());
    });
    stopped_rx
        .recv_timeout(TIMEOUT)
        .expect("terminate_all() blocked while the environment ran JavaScript");
    assert_eq!(
        status_rx
            .recv_timeout(TIMEOUT)
            .expect("the running script was not terminated"),
        NAPI_PENDING_EXCEPTION
    );
    guest.join().expect("guest thread panicked");
}

/// Release runs the env's finalizers, and a guest finalizer can run
/// JavaScript. A host stop issued meanwhile must still reach it.
#[test]
fn host_terminate_reaches_javascript_a_finalizer_runs_during_release() {
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let control = ctx.runtime_control();
    let (signal_tx, signal_rx) = mpsc::channel();
    let (released_tx, released_rx) = mpsc::channel();
    let guest = thread::spawn(move || {
        let mut guest = Guest::new(&ctx, &engine, &module, signal_tx);
        guest.create();
        assert_eq!(guest.arm_spinning_finalizer(), 0);
        let _ = released_tx.send(guest.release());
    });

    signal_rx
        .recv_timeout(TIMEOUT)
        .expect("the finalizer never ran");
    let (stopped_tx, stopped_rx) = mpsc::channel();
    thread::spawn(move || {
        control.terminate_all();
        let _ = stopped_tx.send(());
    });
    stopped_rx
        .recv_timeout(TIMEOUT)
        .expect("terminate_all() blocked while the env was being released");
    assert_eq!(
        released_rx
            .recv_timeout(TIMEOUT)
            .expect("the finalizer's JavaScript kept running after terminate_all()"),
        0
    );
    guest.join().expect("guest thread panicked");
}

/// A store dropped on a thread other than the one that created its env cannot
/// release the env there. The isolate must then stay charged to the budget,
/// not leak unaccounted.
///
/// `wasmer::Store` is not `Send`; embedders that move stores anyway do so
/// unsafely, and this test does the same to reach that path.
#[test]
fn an_env_whose_store_is_dropped_on_another_thread_stays_charged() {
    struct MovedGuest(#[allow(dead_code)] Guest);
    // SAFETY: the guest is only dropped on the other thread, never used there.
    unsafe impl Send for MovedGuest {}

    let (engine, module) = compile();
    let ctx = NapiCtx::builder()
        .total_memory_bytes(512 * 1024 * 1024)
        .build();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let mut guest = Guest::new(&ctx, &engine, &module, signal_tx);
    guest.create();
    assert_eq!(ctx.budget().snapshot().live_isolates, 1);
    let moved = MovedGuest(guest);
    thread::spawn(move || drop(moved))
        .join()
        .expect("dropping the store panicked");
    assert_eq!(
        ctx.budget().snapshot().live_isolates,
        1,
        "an isolate that was never released was returned to the budget"
    );
}

/// Envs created, used and released on several threads at once: module records
/// live in provider state shared by all environments, and isolates are
/// created and disposed while others start up. A failure here is a crash or
/// a hang, so the test bounds the whole run.
#[test]
fn envs_on_several_threads_come_and_go_while_loading_modules() {
    const THREADS: usize = 4;
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let (done_tx, done_rx) = mpsc::channel();
    let start = Arc::new(Barrier::new(THREADS));
    for _ in 0..THREADS {
        let (ctx, engine, module) = (ctx.clone(), engine.clone(), module.clone());
        let (start, signal_tx, done_tx) = (Arc::clone(&start), signal_tx.clone(), done_tx.clone());
        thread::spawn(move || {
            start.wait();
            for _ in 0..25 {
                let mut guest = Guest::new(&ctx, &engine, &module, signal_tx.clone());
                guest.create();
                for _ in 0..4 {
                    assert_eq!(
                        guest.eval_module("export const answer = 42; globalThis.answer = answer;"),
                        0
                    );
                }
                assert_eq!(guest.number("answer"), 42);
                assert_eq!(guest.release(), 0);
            }
            done_tx.send(()).unwrap();
        });
    }
    for _ in 0..THREADS {
        done_rx
            .recv_timeout(Duration::from_secs(120))
            .expect("a thread crashed or hung while creating, using or releasing envs");
    }
}

/// One queued message, two environments racing to take it: the narrower
/// registry lock must still hand it to exactly one of them.
#[test]
fn a_message_has_exactly_one_consumer() {
    let (engine, module) = compile();
    let ctx = NapiCtx::default();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let mut source = Guest::new(&ctx, &engine, &module, signal_tx.clone());
    source.create();
    assert_eq!(source.eval("({ answer: 42 })"), 0);
    assert_eq!(source.message(), 0);
    let token = source.token();

    let start = Arc::new(Barrier::new(2));
    let consumers: Vec<_> = (0..2)
        .map(|_| {
            let (ctx, engine, module) = (ctx.clone(), engine.clone(), module.clone());
            let (start, signal_tx) = (Arc::clone(&start), signal_tx.clone());
            thread::spawn(move || {
                let mut guest = Guest::new(&ctx, &engine, &module, signal_tx);
                guest.create();
                start.wait();
                let status = guest.take(token);
                if status == 0 {
                    assert_eq!(guest.install(), 0);
                    assert_eq!(guest.number("shared.answer"), 42);
                }
                assert_eq!(guest.release(), 0);
                status
            })
        })
        .collect();
    let mut statuses: Vec<i32> = consumers
        .into_iter()
        .map(|consumer| consumer.join().expect("consumer panicked"))
        .collect();
    statuses.sort();
    assert_eq!(statuses, [0, NAPI_INVALID_ARG]);
    assert_eq!(source.release(), 0);
}

unsafe extern "C" {
    fn snapi_bridge_unofficial_configure_runtime(flags: *const c_char, length: u32) -> i32;
    fn snapi_bridge_unofficial_create_env(
        module_api_version: i32,
        guest_heap_ctx: *const c_void,
        env_out: *mut *mut c_void,
    ) -> i32;
    fn snapi_bridge_unofficial_release_env(env: *mut c_void) -> i32;
    fn snapi_bridge_get_global(env: *mut c_void, out_id: *mut u32) -> i32;
    fn snapi_bridge_alloc_cb_reg_id(env: *mut c_void) -> u32;
    fn snapi_bridge_unofficial_terminate_execution(env: *mut c_void) -> i32;
    fn snapi_bridge_unofficial_cancel_terminate_execution(env: *mut c_void) -> i32;
    fn snapi_bridge_dispose();
    fn snapi_bridge_create_int32(env: *mut c_void, value: i32, out_id: *mut u32) -> i32;
    fn snapi_bridge_get_value_int32(env: *mut c_void, id: u32, out: *mut i32) -> i32;
}

/// Not a correctness test: bridge calls per second with 1, 2 and 4 envs, one
/// per thread, all calling at once. Run with
/// `cargo test --test concurrent_envs -- --ignored --nocapture bridge_call_throughput`
/// (an optimized build of this crate gives meaningful numbers).
#[test]
#[ignore]
fn bridge_call_throughput() {
    const CALLS: u32 = 2_000_000;
    unsafe {
        assert_eq!(
            snapi_bridge_unofficial_configure_runtime(std::ptr::null(), 0),
            0
        );
    }
    for threads in [1usize, 2, 4] {
        let start = Arc::new(Barrier::new(threads + 1));
        let workers: Vec<_> = (0..threads)
            .map(|_| {
                let start = Arc::clone(&start);
                thread::spawn(move || unsafe {
                    let mut env: *mut c_void = std::ptr::null_mut();
                    assert_eq!(
                        snapi_bridge_unofficial_create_env(8, std::ptr::null(), &mut env),
                        0
                    );
                    let mut id = 0u32;
                    assert_eq!(snapi_bridge_create_int32(env, 42, &mut id), 0);
                    start.wait();
                    let began = Instant::now();
                    let mut value = 0i32;
                    for _ in 0..CALLS {
                        assert_eq!(snapi_bridge_get_value_int32(env, id, &mut value), 0);
                    }
                    let elapsed = began.elapsed();
                    assert_eq!(value, 42);
                    assert_eq!(snapi_bridge_unofficial_release_env(env), 0);
                    elapsed
                })
            })
            .collect();
        start.wait();
        let slowest = workers
            .into_iter()
            .map(|worker| worker.join().expect("benchmark thread panicked"))
            .max()
            .unwrap();
        println!(
            "THROUGHPUT threads={threads} ns_per_call={:.1} total_calls_per_sec={:.0}",
            slowest.as_nanos() as f64 / f64::from(CALLS),
            f64::from(CALLS) * threads as f64 / slowest.as_secs_f64()
        );
    }
}

/// An isolate and its persistent scopes belong to the thread that created
/// them. Another thread holding the raw bridge handle may stop it, but every
/// ordinary call, release and dispose has to be refused rather than touching
/// V8 from the wrong thread.
#[test]
fn foreign_threads_may_only_terminate_an_env() {
    unsafe {
        assert_eq!(
            snapi_bridge_unofficial_configure_runtime(std::ptr::null(), 0),
            0
        );
        let mut env: *mut c_void = std::ptr::null_mut();
        assert_eq!(
            snapi_bridge_unofficial_create_env(8, std::ptr::null(), &mut env),
            0
        );
        let mut global = 0u32;
        assert_eq!(snapi_bridge_get_global(env, &mut global), 0);

        let handle = env as usize;
        thread::scope(|scope| {
            scope.spawn(|| {
                let env = handle as *mut c_void;
                let mut global = 0u32;
                assert_eq!(
                    snapi_bridge_get_global(env, &mut global),
                    NAPI_INVALID_ARG,
                    "a foreign thread used another thread's environment"
                );
                assert_eq!(snapi_bridge_alloc_cb_reg_id(env), 0);
                assert_eq!(
                    snapi_bridge_unofficial_cancel_terminate_execution(env),
                    NAPI_INVALID_ARG
                );
                assert_eq!(
                    snapi_bridge_unofficial_release_env(env),
                    NAPI_INVALID_ARG,
                    "a foreign thread released another thread's environment"
                );
                // Owns nothing on this thread, so it must leave `env` alone.
                snapi_bridge_dispose();
                assert_eq!(snapi_bridge_unofficial_terminate_execution(env), 0);
            });
        });

        assert_eq!(snapi_bridge_unofficial_cancel_terminate_execution(env), 0);
        assert_eq!(snapi_bridge_get_global(env, &mut global), 0);
        assert_eq!(snapi_bridge_unofficial_release_env(env), 0);
    }
}
