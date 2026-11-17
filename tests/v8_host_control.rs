#![cfg(feature = "cli")]

//! Host-side control over live V8 isolates, exercised against real V8 rather
//! than in isolation: a WASIX guest runs JS through the N-API host imports,
//! and the host stops it or refuses it a budget.
//!
//! These need the guest `.wasm` files, which `tests/build-test-wasix.sh`
//! builds with `wasixcc`.

use std::{
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};

use wasmer_napi::{NapiCtx, ResourceUsage, WasmPolicy};

mod common;
use common::{build_wasix_test, run_guest};

/// Budget that comfortably fits a guest plus a default V8 isolate.
const GENEROUS_BUDGET: u64 = 512 * 1024 * 1024;

/// `napi_pending_exception`, which is what a terminated isolate reports.
const NAPI_PENDING_EXCEPTION: i32 = 10;

/// Budget below the fixed per-isolate floor (per-isolate overhead plus unwind
/// slack), so no isolate can be admitted no matter how far its old-space
/// ceiling is clamped.
const BUDGET_BELOW_ISOLATE_FLOOR: u64 = 20 * 1024 * 1024;

/// How long a guest's thread gets to drop its store after `run_guest` returns.
const GUEST_TEARDOWN_DEADLINE: Duration = Duration::from_secs(30);

/// The budget once the guest's V8 charges are gone, or as it stands at
/// [`GUEST_TEARDOWN_DEADLINE`].
///
/// `run_guest` returns as soon as WASIX has the guest's exit status. The
/// guest's own thread reports that status (`WasiEnv::blocking_on_exit`) and
/// only then drops the guest's store, and with it the N-API environments the
/// guest never released, so their isolates go away a moment after `run_guest`
/// returns. They are released on their owner thread as they should be; the
/// caller just has to wait for it. One that is still charged at the deadline
/// is a leak.
fn usage_after_guest_teardown(ctx: &NapiCtx) -> ResourceUsage {
    let deadline = Instant::now() + GUEST_TEARDOWN_DEADLINE;
    loop {
        let usage = ctx.budget().snapshot();
        let released =
            usage.live_isolates == 0 && usage.v8_heap_reserved == 0 && usage.v8_external == 0;
        if released || Instant::now() >= deadline {
            return usage;
        }
        thread::sleep(Duration::from_millis(5));
    }
}

/// The kill path Edge relies on: a JS loop that never returns on its own has
/// to stop when the host says so. Nothing in the guest cooperates here — the
/// isolate is executing JS when the request arrives.
#[test]
fn terminate_all_stops_running_js() {
    let wasm = build_wasix_test("test_js_infinite_loop");

    let ctx = NapiCtx::default();
    let control = ctx.runtime_control();

    let (finished_tx, finished_rx) = mpsc::channel();
    let guest = thread::spawn(move || {
        let result = run_guest(&ctx, &wasm);
        // Signal completion separately so a hang fails the test instead of
        // blocking it forever on `join`.
        let _ = finished_tx.send(());
        result
    });

    // Give the guest time to reach JS. The loop never ends by itself, so being
    // late only makes the test slower — it can't make it pass spuriously.
    thread::sleep(Duration::from_secs(2));

    let requested_at = Instant::now();
    control.terminate_all();

    finished_rx
        .recv_timeout(Duration::from_secs(60))
        .expect("terminate_all() did not stop the running JS");
    let elapsed = requested_at.elapsed();

    let (exit_code, stdout, stderr) = guest
        .join()
        .expect("guest thread panicked")
        .expect("guest run failed");

    assert!(
        stdout.contains("JS_LOOP_ENTERED"),
        "the guest never reached JS, so nothing was terminated\
         \n--- stdout ---\n{stdout}\n--- stderr ---\n{stderr}"
    );
    assert!(
        stdout.contains(&format!("JS_LOOP_LEFT status={NAPI_PENDING_EXCEPTION}")),
        "the JS loop didn't end in a terminated isolate\
         \n--- stdout ---\n{stdout}\n--- stderr ---\n{stderr}"
    );

    // The guest then tries to clear the termination and keep running, the way
    // a workload that doesn't want to be killed would. The host's stop is
    // sticky, so it must not get back in.
    assert!(
        !stdout.contains("RESUME_STATUS=0"),
        "the guest cancelled a host-requested termination and resumed JS\
         \n--- stdout ---\n{stdout}\n--- stderr ---\n{stderr}"
    );
    assert!(
        stdout.contains("RESUME_STATUS="),
        "the guest never reported whether it could resume\
         \n--- stdout ---\n{stdout}\n--- stderr ---\n{stderr}"
    );
    assert_eq!(exit_code, 0, "guest exited with {exit_code}\n{stderr}");

    eprintln!(
        "JS loop stopped {}ms after the request",
        elapsed.as_millis()
    );
}

/// Every byte an isolate reserves has to come back when it goes away,
/// otherwise a long-lived app leaks budget until it can't start an isolate at
/// all. Charging is by reservation, so this covers creation and teardown of a
/// real isolate rather than the accountant's own bookkeeping.
#[test]
fn isolate_reservations_are_released_when_the_guest_exits() {
    let wasm = build_wasix_test("run_script_test");

    let ctx = NapiCtx::builder()
        .total_memory_bytes(GENEROUS_BUDGET)
        .build();
    let (exit_code, stdout, stderr) = run_guest(&ctx, &wasm).expect("guest run failed");

    assert_eq!(exit_code, 0, "guest exited with {exit_code}\n{stderr}");
    assert!(
        stdout.contains("RUN_SCRIPT_TEST_OK=1"),
        "guest did not report success\n--- stdout ---\n{stdout}"
    );

    let usage = usage_after_guest_teardown(&ctx);
    assert_eq!(
        usage.live_isolates, 0,
        "an isolate outlived the guest: {usage:?}"
    );
    assert_eq!(
        usage.v8_heap_reserved, 0,
        "V8 heap reservations were not released: {usage:?}"
    );
    assert_eq!(
        usage.v8_external, 0,
        "V8 external memory was not released: {usage:?}"
    );
}

/// The budget has to be able to say no. Below the per-isolate floor there is
/// no ceiling small enough to admit an isolate, so env creation must be
/// refused rather than quietly allocating outside the budget.
#[test]
fn isolate_creation_is_refused_below_the_budget_floor() {
    let wasm = build_wasix_test("hello_napi_test");

    let ctx = NapiCtx::builder()
        .total_memory_bytes(BUDGET_BELOW_ISOLATE_FLOOR)
        .build();
    let outcome = run_guest(&ctx, &wasm);

    // Either the run fails outright or the guest reports failure; what must
    // not happen is a successful run, which would mean the isolate was
    // created outside the budget.
    if let Ok((exit_code, stdout, stderr)) = outcome {
        assert!(
            !stdout.contains("HELLO_NAPI_TEST_OK=1"),
            "the guest created a V8 isolate that the budget can't cover\
             \n--- stdout ---\n{stdout}\n--- stderr ---\n{stderr}"
        );
        assert_ne!(
            exit_code, 0,
            "guest reported success under a budget that cannot hold an isolate\n{stdout}"
        );
    }

    let usage = ctx.budget().snapshot();
    assert_eq!(
        usage.live_isolates, 0,
        "a refused isolate still holds a slot: {usage:?}"
    );
    assert_eq!(
        usage.v8_heap_reserved, 0,
        "a refused isolate still holds a heap reservation: {usage:?}"
    );
}

/// Runs `program` under `policy` and checks that it printed `marker` and
/// exited cleanly.
fn assert_guest_passes(program: &str, policy: WasmPolicy, marker: &str) {
    let wasm = build_wasix_test(program);
    let ctx = NapiCtx::builder().webassembly(policy).build();
    assert_eq!(ctx.webassembly_policy(), policy);
    let (exit_code, stdout, stderr) = run_guest(&ctx, &wasm).expect("guest run failed");
    assert!(
        stdout.contains(marker) && exit_code == 0,
        "{program} under {policy:?} exited with {exit_code}\
         \n--- stdout ---\n{stdout}\n--- stderr ---\n{stderr}"
    );
}

/// `WasmPolicy::Restricted` takes `WebAssembly` out of the env's realm and
/// out of every realm made in it, also realms that allow wasm code
/// generation.
#[test]
fn restricted_webassembly_is_absent_from_every_realm() {
    assert_guest_passes(
        "test_js_wasm_restricted",
        WasmPolicy::Restricted,
        "JS_WASM_RESTRICTED_OK",
    );
}

/// The default policy keeps V8's WebAssembly, and a realm's
/// `allow_code_gen_wasm` (vm's `codeGeneration.wasm`) now decides whether it
/// compiles.
#[test]
fn default_webassembly_honors_each_realms_code_generation_option() {
    assert_eq!(WasmPolicy::default(), WasmPolicy::EnabledUnmetered);
    assert_eq!(
        NapiCtx::default().webassembly_policy(),
        WasmPolicy::EnabledUnmetered
    );
    assert_guest_passes(
        "test_js_wasm_enabled",
        WasmPolicy::EnabledUnmetered,
        "JS_WASM_ENABLED_OK",
    );
}

/// The policy belongs to the context whose guest created the env, not to
/// the process: a restricted guest and an unrestricted one run in the same
/// process, in either order.
#[test]
fn webassembly_policy_is_per_context() {
    for policy in [
        WasmPolicy::Restricted,
        WasmPolicy::EnabledUnmetered,
        WasmPolicy::Restricted,
    ] {
        match policy {
            WasmPolicy::Restricted => {
                assert_guest_passes("test_js_wasm_restricted", policy, "JS_WASM_RESTRICTED_OK")
            }
            _ => assert_guest_passes("test_js_wasm_enabled", policy, "JS_WASM_ENABLED_OK"),
        }
    }
}

/// asm.js runs as ordinary JavaScript under every policy: V8 15.2 and later
/// no longer translate it to wasm, so it cannot reach wasm code generation
/// in a restricted env.
#[test]
fn asmjs_runs_as_plain_javascript_under_every_policy() {
    for policy in [WasmPolicy::Restricted, WasmPolicy::EnabledUnmetered] {
        assert_guest_passes("test_js_asmjs_plain", policy, "ASMJS_RUNS_AS_PLAIN_JS_OK");
    }
}
