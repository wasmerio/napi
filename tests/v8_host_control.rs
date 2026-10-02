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

use wasmer_napi::{NapiCtx, WasmPolicy};

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

/// Admits a guest and a default isolate with only a few MiB to spare, which a
/// guest holding tens of thousands of references exhausts.
const BUDGET_STARVING_BOOKKEEPING: u64 = 144 * 1024 * 1024;

#[test]
fn two_guest_isolates_run_sequentially() {
    let wasm = build_wasix_test("hello_napi_test");
    for _ in 0..2 {
        let (exit_code, stdout, stderr) =
            run_guest(&NapiCtx::default(), &wasm).expect("guest run failed");
        assert_eq!(exit_code, 0, "{stderr}");
        assert!(
            stdout.contains("HELLO_NAPI_TEST_OK=1"),
            "{stdout}\n{stderr}"
        );
    }
}

#[test]
fn standalone_unlimited_context_does_not_apply_managed_value_cap() {
    let wasm = build_wasix_test("test_unlimited_value_handles");
    let (exit_code, stdout, stderr) =
        run_guest(&NapiCtx::default(), &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("UNLIMITED_VALUE_HANDLES_OK"),
        "{stdout}\n{stderr}"
    );
}

#[test]
fn finite_budget_charges_value_handles_instead_of_capping_them() {
    let wasm = build_wasix_test("test_unlimited_value_handles");
    let ctx = NapiCtx::builder()
        .total_memory_bytes(GENEROUS_BUDGET)
        .build();
    let (exit_code, stdout, stderr) = run_guest(&ctx, &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("UNLIMITED_VALUE_HANDLES_OK"),
        "{stdout}\n{stderr}"
    );
}

/// Waits for WASIX's asynchronous final-store cleanup, which is what releases
/// an isolate's reservations, then returns the budget snapshot.
fn usage_after_quiescence(ctx: &NapiCtx) -> wasmer_napi::ResourceUsage {
    let deadline = Instant::now() + Duration::from_secs(10);
    while ctx.budget().snapshot().live_isolates != 0 && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(20));
    }
    ctx.budget().snapshot()
}

/// A workload holding a host handle per short-lived object used to fail past
/// a fixed handle count, because the tiny JS objects never made V8 collect the
/// handles' owners. The bookkeeping charge now asks for that collection.
#[test]
fn many_short_lived_refs_are_collected_under_a_finite_budget() {
    let wasm = build_wasix_test("test_ref_churn");
    let ctx = NapiCtx::builder()
        .total_memory_bytes(GENEROUS_BUDGET)
        .build();
    let (exit_code, stdout, stderr) = run_guest(&ctx, &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(stdout.contains("REF_CHURN_OK"), "{stdout}\n{stderr}");
    let usage = usage_after_quiescence(&ctx);
    assert_eq!(
        usage.host_bookkeeping, 0,
        "bookkeeping outlived the guest: {usage:?}"
    );
}

/// Bookkeeping the budget cannot cover is an out-of-memory condition: the env
/// is stopped like one whose heap growth was refused, not left half-working.
#[test]
fn bookkeeping_exhaustion_stops_the_env() {
    let wasm = build_wasix_test("test_ref_hold");
    let ctx = NapiCtx::builder()
        .total_memory_bytes(BUDGET_STARVING_BOOKKEEPING)
        .build();
    let (exit_code, stdout, stderr) = run_guest(&ctx, &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(stdout.contains("REF_HOLD_REFUSED"), "{stdout}\n{stderr}");
    assert!(
        stdout.contains(&format!("JS_AFTER_REFUSAL status={NAPI_PENDING_EXCEPTION}")),
        "the refusal did not terminate the isolate: {stdout}\n{stderr}"
    );
    let usage = usage_after_quiescence(&ctx);
    assert_eq!(
        usage.host_bookkeeping, 0,
        "bookkeeping outlived the guest: {usage:?}"
    );
}

#[test]
fn guest_fatal_error_exits_only_its_workload() {
    let fatal_wasm = build_wasix_test("test_guest_fatal_error");
    let hello_wasm = build_wasix_test("hello_napi_test");

    let (exit_code, stdout, stderr) =
        run_guest(&NapiCtx::default(), &fatal_wasm).expect("fatal guest run failed");
    assert_ne!(
        exit_code, 0,
        "fatal guest unexpectedly survived: {stdout}\n{stderr}"
    );

    let (exit_code, stdout, stderr) =
        run_guest(&NapiCtx::default(), &hello_wasm).expect("next guest run failed");
    assert_eq!(exit_code, 0, "{stderr}");
    assert!(
        stdout.contains("HELLO_NAPI_TEST_OK=1"),
        "{stdout}\n{stderr}"
    );
}

#[test]
fn guest_cannot_set_process_wide_v8_flags_with_embedded_nul() {
    let wasm = build_wasix_test("test_guest_runtime_flags");
    let (exit_code, stdout, stderr) =
        run_guest(&NapiCtx::default(), &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(stdout.contains("FLAGS_REJECTED"), "{stdout}\n{stderr}");
}

#[test]
fn guest_property_descriptor_count_is_bounded_before_read() {
    let wasm = build_wasix_test("test_property_descriptor_cap");
    let (exit_code, stdout, stderr) =
        run_guest(&NapiCtx::default(), &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("PROPERTY_CAP_REJECTED"),
        "{stdout}\n{stderr}"
    );
}

#[test]
fn explicit_length_names_preserve_embedded_nul_without_native_overread() {
    let wasm = build_wasix_test("test_embedded_nul_names");
    let (exit_code, stdout, stderr) =
        run_guest(&NapiCtx::default(), &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("EMBEDDED_NUL_NAMES_OK"),
        "{stdout}\n{stderr}"
    );
}

#[test]
fn guest_cannot_allocate_unmetered_v8_webassembly_memory() {
    let wasm = build_wasix_test("test_js_wasm_disabled");
    let (exit_code, stdout, stderr) =
        run_guest(&NapiCtx::default(), &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(stdout.contains("JS_WASM_DISABLED_OK"), "{stdout}\n{stderr}");
}

/// asm.js would otherwise be translated to WebAssembly and compiled into
/// V8's wasm code space, bypassing the WebAssembly restriction above.
#[test]
fn asm_js_runs_as_plain_javascript() {
    let wasm = build_wasix_test("test_js_asmjs_plain");
    let (exit_code, stdout, stderr) =
        run_guest(&NapiCtx::default(), &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("ASMJS_RUNS_AS_PLAIN_JS_OK"),
        "{stdout}\n{stderr}"
    );
}

#[test]
fn explicitly_restricted_webassembly_policy_keeps_webassembly_unavailable() {
    let wasm = build_wasix_test("test_js_wasm_disabled");
    let ctx = NapiCtx::builder()
        .webassembly(WasmPolicy::Restricted)
        .total_memory_bytes(GENEROUS_BUDGET)
        .build();
    assert_eq!(ctx.webassembly_policy(), WasmPolicy::Restricted);
    let (exit_code, stdout, stderr) = run_guest(&ctx, &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(stdout.contains("JS_WASM_DISABLED_OK"), "{stdout}\n{stderr}");
}

/// The embedder opt-in exposes V8 WebAssembly in the root and `vm` contexts;
/// per-context code-generation settings still apply.
#[test]
fn unmetered_webassembly_policy_exposes_webassembly() {
    let wasm = build_wasix_test("test_js_wasm_unmetered");
    let ctx = NapiCtx::builder()
        .webassembly(WasmPolicy::EnabledUnmetered)
        .build();
    assert_eq!(ctx.webassembly_policy(), WasmPolicy::EnabledUnmetered);
    let (exit_code, stdout, stderr) = run_guest(&ctx, &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("JS_WASM_UNMETERED_OK"),
        "{stdout}\n{stderr}"
    );

    // The policy belongs to one context: the next default context is
    // restricted again in the same process.
    let wasm = build_wasix_test("test_js_wasm_disabled");
    let (exit_code, stdout, stderr) =
        run_guest(&NapiCtx::default(), &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(stdout.contains("JS_WASM_DISABLED_OK"), "{stdout}\n{stderr}");
}

#[test]
fn guest_cannot_start_unmanaged_v8_workers_or_profiler() {
    let wasm = build_wasix_test("test_guest_unmanaged_controls");
    let (exit_code, stdout, stderr) =
        run_guest(&NapiCtx::default(), &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("UNMANAGED_CONTROLS_REJECTED"),
        "{stdout}\n{stderr}"
    );
}

#[test]
fn guest_messages_remain_charged_until_instance_shutdown() {
    let wasm = build_wasix_test("test_guest_message_budget");
    let ctx = NapiCtx::builder()
        .total_memory_bytes(GENEROUS_BUDGET)
        .build();
    let budget = ctx.budget();
    let (exit_code, stdout, stderr) = run_guest(&ctx, &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(stdout.contains("MESSAGE_BUDGET_OK"), "{stdout}\n{stderr}");
    assert!(
        budget.snapshot().serialized_message > 0,
        "queued message escaped memory accounting"
    );
    ctx.runtime_control().terminate_all();
    assert_eq!(budget.snapshot().serialized_message, 0);
    drop(ctx);
}

#[test]
fn named_properties_and_throw_errors_use_charged_guest_strings() {
    let wasm = build_wasix_test("test_charged_cstring_bridge");
    let (exit_code, stdout, stderr) =
        run_guest(&NapiCtx::default(), &wasm).expect("guest run failed");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("CHARGED_CSTRING_BRIDGE_OK"),
        "{stdout}\n{stderr}"
    );
}

#[test]
fn two_guest_isolates_can_coexist_while_one_sleeps() {
    let sleeping_wasm = build_wasix_test("test_env_sleep");
    let hello_wasm = build_wasix_test("hello_napi_test");
    let sleeping = thread::spawn(move || run_guest(&NapiCtx::default(), &sleeping_wasm));
    thread::sleep(Duration::from_secs(1));
    let (exit_code, stdout, stderr) =
        run_guest(&NapiCtx::default(), &hello_wasm).expect("second guest failed");
    assert_eq!(exit_code, 0, "{stderr}");
    assert!(
        stdout.contains("HELLO_NAPI_TEST_OK=1"),
        "{stdout}\n{stderr}"
    );
    let (exit_code, stdout, stderr) = sleeping.join().unwrap().unwrap();
    assert_eq!(exit_code, 0, "{stderr}");
    assert!(stdout.contains("ENV_READY"), "{stdout}\n{stderr}");
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

/// A busy isolate cannot hold the bridge's process-wide registry lock while
/// another workload creates its isolate and requests GC.
#[test]
fn another_guest_can_collect_gc_while_js_is_spinning() {
    let spinning_wasm = build_wasix_test("test_js_infinite_loop");
    let gc_wasm = build_wasix_test("test_gc_once");
    let busy_ctx = NapiCtx::default();
    let control = busy_ctx.runtime_control();
    let (busy_tx, busy_rx) = mpsc::channel();
    let busy = thread::spawn(move || {
        let result = run_guest(&busy_ctx, &spinning_wasm);
        let _ = busy_tx.send(());
        result
    });

    // The guest prints its marker before entering the loop. The captured
    // stdout is only returned at completion, so allow startup time here.
    thread::sleep(Duration::from_secs(2));

    let (gc_tx, gc_rx) = mpsc::channel();
    let gc_guest = thread::spawn(move || {
        let result = run_guest(&NapiCtx::default(), &gc_wasm);
        let _ = gc_tx.send(());
        result
    });
    let gc_finished = gc_rx.recv_timeout(Duration::from_secs(15));
    control.terminate_all();
    busy_rx
        .recv_timeout(Duration::from_secs(60))
        .expect("spinning guest survived termination");

    gc_finished.expect("second guest stalled behind busy isolate");
    let (exit_code, stdout, stderr) = gc_guest.join().unwrap().unwrap();
    assert_eq!(exit_code, 0, "{stderr}");
    assert!(stdout.contains("GC_DONE"), "{stdout}\n{stderr}");
    let (_, busy_stdout, busy_stderr) = busy.join().unwrap().unwrap();
    assert!(
        busy_stdout.contains("JS_LOOP_ENTERED"),
        "busy guest never entered JS: {busy_stdout}\n{busy_stderr}"
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

    let usage = usage_after_quiescence(&ctx);
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
