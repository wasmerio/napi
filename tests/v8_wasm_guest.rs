#![cfg(feature = "cli")]

//! Metered V8 WebAssembly for WASIX guests in a managed context: the guest
//! sees WebAssembly in its root and `vm` contexts, refused memory growth
//! stays a recoverable JS error, and a context past its code budget is
//! stopped through the provider's stop path and reported to the embedder.
//!
//! These need the guest `.wasm` files, which `tests/build-test-wasix.sh`
//! builds with `wasixcc`.

use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicU64, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

use wasmer_napi::{
    ManagedV8Lane, NapiCtx, NapiLimitExceeded, NapiMemoryAccountant, NapiRuntimeHooks, Pool,
    WasmEngineLimits, WasmLimits, WasmPolicy, cli::run_wasix_main_capture_stdio_with_hooks,
};

mod common;
use common::build_wasix_test;

const MIB: u64 = 1024 * 1024;

/// An embedder accountant whose terminal charge has no side effects.
#[derive(Default)]
struct Accountant {
    limit: u64,
    charged: AtomicU64,
    soft_denials: Mutex<Vec<Pool>>,
    exceeded: Mutex<Vec<NapiLimitExceeded>>,
}

impl NapiMemoryAccountant for Accountant {
    fn memory_limit(&self) -> u64 {
        self.limit
    }
    fn memory_charged(&self) -> u64 {
        self.charged.load(Ordering::SeqCst)
    }
    fn try_charge(&self, bytes: u64) -> bool {
        self.charged
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |current| {
                current
                    .checked_add(bytes)
                    .filter(|total| *total <= self.limit)
            })
            .is_ok()
    }
    fn try_charge_soft_for(&self, pool: Pool, bytes: u64) -> bool {
        let granted = self.try_charge(bytes);
        if !granted {
            self.soft_denials.lock().unwrap().push(pool);
        }
        granted
    }
    fn uncharge(&self, bytes: u64) {
        self.charged.fetch_sub(bytes, Ordering::SeqCst);
    }
    fn limit_exceeded(&self, limit: NapiLimitExceeded) {
        self.exceeded.lock().unwrap().push(limit);
    }
}

/// Managed hooks with metered WebAssembly and one lazily started lane.
fn metered_hooks(accountant: Arc<Accountant>, limits: WasmLimits) -> NapiRuntimeHooks {
    wasmer_napi::configure_wasm_engine(&WasmEngineLimits::default()).unwrap();
    let lane_slot: Arc<Mutex<Option<Arc<ManagedV8Lane>>>> = Arc::new(Mutex::new(None));
    NapiCtx::builder()
        .memory_accountant(accountant)
        .webassembly(WasmPolicy::EnabledMetered(limits))
        .build_managed_imports(Arc::new(move || {
            let mut slot = lane_slot.lock().unwrap();
            if let Some(lane) = &*slot {
                return Ok(Arc::clone(lane));
            }
            let lane = ManagedV8Lane::new(256, Arc::new(|| Box::new(())), Arc::new(|| {}))?;
            let worker = Arc::clone(&lane);
            thread::Builder::new()
                .name("napi-test-lane".into())
                .spawn(move || worker.run())?;
            *slot = Some(Arc::clone(&lane));
            Ok(lane)
        }))
}

fn wait_for(what: &str, done: impl Fn() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(10);
    while !done() {
        assert!(Instant::now() < deadline, "timed out waiting for {what}");
        thread::sleep(Duration::from_millis(20));
    }
}

#[test]
fn metered_policy_exposes_webassembly_to_guests() {
    let wasm = build_wasix_test("test_js_wasm_unmetered");
    let accountant = Arc::new(Accountant {
        limit: 1024 * MIB,
        ..Default::default()
    });
    let hooks = metered_hooks(Arc::clone(&accountant), WasmLimits::default());
    let (exit_code, stdout, stderr) =
        run_wasix_main_capture_stdio_with_hooks(&hooks, &wasm, &[], &[]).unwrap();
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    // Root and vm contexts compile and grow memory; a vm context with wasm
    // code generation disabled and foreign realms still refuse.
    assert!(
        stdout.contains("JS_WASM_UNMETERED_OK"),
        "{stdout}\n{stderr}"
    );
}

#[test]
fn metered_guest_is_denied_softly_and_stopped_past_its_code_budget() {
    let wasm = build_wasix_test("test_js_wasm_metered");
    let accountant = Arc::new(Accountant {
        limit: 768 * MIB,
        ..Default::default()
    });
    let mut limits = WasmLimits::default();
    limits.code_budget_bytes = 512 * 1024;
    let hooks = metered_hooks(Arc::clone(&accountant), limits);
    let budget = hooks.budget();
    let (exit_code, stdout, stderr) =
        run_wasix_main_capture_stdio_with_hooks(&hooks, &wasm, &[], &[]).unwrap();
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("WASM_SOFT_DENIALS_OK"),
        "{stdout}\n{stderr}"
    );
    assert!(
        stdout.contains("CODE_BOMB_STATUS=10"),
        "the code bomb was not stopped: {stdout}\n{stderr}"
    );
    assert!(
        accountant
            .soft_denials
            .lock()
            .unwrap()
            .contains(&Pool::V8WasmMemory)
    );
    wait_for("the code budget report", || {
        !accountant.exceeded.lock().unwrap().is_empty()
    });
    let exceeded = accountant.exceeded.lock().unwrap().clone();
    assert!(
        matches!(
            exceeded.as_slice(),
            [NapiLimitExceeded::WasmCodeBudget { budget: 524288, committed }] if *committed > 524288
        ),
        "{exceeded:?}"
    );
    // The guest's environments are gone; so is every wasm charge.
    wait_for("isolate teardown", || budget.snapshot().live_isolates == 0);
    let usage = budget.snapshot();
    assert_eq!(
        (usage.v8_wasm_memory, usage.v8_wasm_code),
        (0, 0),
        "{usage:?}"
    );
}
