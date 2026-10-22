//! Metered V8 WebAssembly (`WasmPolicy::EnabledMetered`): wasm memories are
//! charged softly and capped, wasm code is charged after the fact and
//! budgeted, and runaway wasm stays stoppable. Driven natively through the
//! bridge with an embedder lane per context, as a managed embedder does.
//!
//! Every test of this binary uses the default process-wide engine limits.

use std::{
    sync::{Arc, atomic::Ordering},
    thread,
    time::{Duration, Instant},
};

#[path = "common/v8_lane.rs"]
mod v8_lane;
use v8_lane::{
    Caps, Lane, MIB, NAPI_PENDING_EXCEPTION, POLICY_METERED, POLICY_RESTRICTED, configure_engine,
    outcome,
};

const WASM_PAGE: u64 = 64 * 1024;
/// `SNAPI_V8_WASM_CODE_LIMIT_*`.
const LIMIT_BUDGET: u32 = 1;
const LIMIT_MEMORY: u32 = 2;

unsafe extern "C" {
    fn snapi_v8_test_code_reserve(length: usize) -> *mut std::ffi::c_void;
    fn snapi_v8_test_code_commit(
        address: *mut std::ffi::c_void,
        length: usize,
        commit: bool,
    ) -> bool;
    fn snapi_v8_test_page_free(address: *mut std::ffi::c_void, length: usize) -> bool;
    fn snapi_bridge_unofficial_create_env(
        api_version: i32,
        guest_heap: *const std::ffi::c_void,
        webassembly_policy: u32,
        env_out: *mut *mut std::ffi::c_void,
    ) -> i32;
}

fn setup() {
    configure_engine(&wasmer_napi::WasmEngineLimits::default());
}

fn metered_lane(limit: u64, caps: Caps) -> Lane {
    setup();
    Lane::new(limit, Some(caps))
}

fn mapping_count() -> usize {
    std::fs::read_to_string("/proc/self/maps")
        .map(|maps| maps.lines().count())
        .unwrap_or(0)
}

#[test]
fn metered_environments_require_wasm_accounting_and_engine_limits() {
    setup();
    // Same limits again: accepted, also once V8 runs.
    configure_engine(&wasmer_napi::WasmEngineLimits::default());
    let mut different = wasmer_napi::WasmEngineLimits::default();
    different.max_functions = 10;
    let mut invalid = wasmer_napi::WasmEngineLimits::default();
    invalid.max_memory_pages = 65537;
    assert!(wasmer_napi::configure_wasm_engine(&invalid).is_err());

    // Without any lane bound (checked before the runtime is touched).
    let mut env = std::ptr::null_mut();
    assert_ne!(
        unsafe {
            snapi_bridge_unofficial_create_env(8, std::ptr::null(), POLICY_METERED, &mut env)
        },
        0
    );
    assert!(env.is_null());

    let unmetered = Lane::new(64 * MIB, None);
    assert!(
        unmetered.try_env(POLICY_METERED).is_err(),
        "metered WebAssembly without the lane's wasm accounting"
    );
    // The policy-independent native environment still works on that lane.
    unmetered.try_env(POLICY_RESTRICTED).unwrap().release();
    assert!(wasmer_napi::configure_wasm_engine(&different).is_err());

    let lane = metered_lane(64 * MIB, Caps::default());
    let env = lane.env();
    assert_eq!(env.eval("typeof WebAssembly"), "object");
    env.load_wasmgen();
    assert_eq!(
        env.eval(
            "String(new WebAssembly.Instance(new WebAssembly.Module(wasmgen.add())).exports.add(40, 2))"
        ),
        "42"
    );
    env.release();
    unmetered.finish();
    lane.finish();
}

#[test]
fn wasm_memory_is_charged_softly_and_bounded() {
    let lane = metered_lane(64 * MIB, Caps::default());
    let acct = Arc::clone(&lane.accountant);
    let env = lane.env();
    env.load_wasmgen();

    // A memory without a maximum reserves the engine's per-memory maximum
    // (16384 pages), not V8's 4 GiB default; only commits are charged.
    env.eval("globalThis.m = new WebAssembly.Memory({ initial: 1 }); ''");
    assert_eq!(acct.memory(), WASM_PAGE);
    let usage = lane.usage();
    assert_eq!(
        (usage.memories, usage.reserved_bytes),
        (1, 16384 * WASM_PAGE)
    );
    assert_eq!(acct.buffers(), 0, "wasm memory charged as a plain buffer");
    let stats = wasmer_napi::wasm_process_stats();
    assert!(stats.memories >= 1 && stats.memory_committed_bytes >= WASM_PAGE);

    assert_eq!(env.eval("String(m.grow(15))"), "1");
    assert_eq!(acct.memory(), 16 * WASM_PAGE);
    // Over the limit: a RangeError, the memory and context survive.
    assert_eq!(env.eval(&outcome("m.grow(2000)")), "RangeError");
    assert_eq!(
        env.eval("String(m.buffer.byteLength)"),
        (16 * WASM_PAGE).to_string()
    );
    assert_eq!(acct.memory(), 16 * WASM_PAGE);

    // memory.grow bomb inside wasm: grows until refused, memory.grow
    // returns -1, the charge stays within the limit.
    let pages: u64 = env
        .eval(
            "const i = new WebAssembly.Instance(new WebAssembly.Module(wasmgen.grower()));
             globalThis.grower = i.exports; String(i.exports.bomb())",
        )
        .parse()
        .unwrap();
    assert!(pages > 16, "the bomb never grew");
    assert!(acct.charged() <= 64 * MIB);
    assert_eq!(acct.memory(), (16 + pages) * WASM_PAGE);
    // Single pages may still fit; then growth is refused for good.
    env.eval("while (grower.grow(1) !== -1) {} ''");
    assert_eq!(env.eval("String(grower.grow(1))"), "-1");
    assert!(acct.charged() <= 64 * MIB);
    assert!(acct.denied.load(Ordering::SeqCst) > 0);

    // Huge allocations: over the limit, and over the engine's per-memory
    // maximum. Both RangeErrors, nothing charged.
    let before = acct.memory();
    assert_eq!(
        env.eval(&outcome("new WebAssembly.Memory({ initial: 16384 })")),
        "RangeError"
    );
    assert_eq!(
        env.eval(&outcome("new WebAssembly.Memory({ initial: 16385 })")),
        "RangeError"
    );
    assert_eq!(acct.memory(), before);
    assert_eq!(env.eval("'still running'"), "still running");

    env.release();
    assert_eq!(acct.memory(), 0);
    assert_eq!(lane.usage().memories, 0);
    lane.finish();
}

#[test]
fn memory_count_and_reservation_caps_bound_mappings() {
    let caps = Caps {
        max_memories: 64,
        ..Caps::default()
    };
    let lane = metered_lane(256 * MIB, caps);
    let env = lane.env();
    let denials = wasmer_napi::wasm_process_stats().memory_cap_denials;
    let maps_before = mapping_count();
    let created: u64 = env
        .eval(
            "globalThis.mems = [];
             try { for (let i = 0; i < 1024; i++) mems.push(new WebAssembly.Memory({ initial: 1, maximum: 1 })); }
             catch (e) { if (!(e instanceof RangeError)) throw e; }
             String(mems.length)",
        )
        .parse()
        .unwrap();
    assert_eq!(created, 64);
    assert_eq!(lane.usage().memories, 64);
    let added = mapping_count().saturating_sub(maps_before);
    assert!(added <= 64 * 3 + 64, "{added} new mappings for 64 memories");
    assert!(wasmer_napi::wasm_process_stats().memory_cap_denials > denials);
    env.eval("mems = null; ''");
    assert!(env.gc_until(|| lane.usage().memories == 0));
    env.release();
    lane.finish();

    // Reserved address space: V8 retries a refused reservation with smaller
    // maxima, so a memory without a maximum still fits.
    let lane = metered_lane(
        256 * MIB,
        Caps {
            max_reserved_bytes: 64 * MIB,
            ..Caps::default()
        },
    );
    let acct = Arc::clone(&lane.accountant);
    let env = lane.env();
    env.eval("globalThis.m = new WebAssembly.Memory({ initial: 1 }); ''");
    assert!(lane.usage().reserved_bytes <= 64 * MIB);
    assert_eq!(
        env.eval(&outcome("new WebAssembly.Memory({ initial: 2000 })")),
        "RangeError"
    );
    // Growing past the reduced reservation copies into a new one.
    assert_eq!(env.eval("String(m.grow(10))"), "1");
    assert!(env.gc_until(|| acct.memory() == 11 * WASM_PAGE));
    assert!(lane.usage().reserved_bytes <= 64 * MIB);
    env.release();
    lane.finish();
}

#[test]
fn shared_memory_grown_from_another_context_is_charged_once() {
    let lane = metered_lane(64 * MIB, Caps::default());
    let acct = Arc::clone(&lane.accountant);
    let first = lane.env();
    first.eval(
        "globalThis.m = new WebAssembly.Memory({ initial: 1, maximum: 256, shared: true }); ''",
    );
    assert_eq!(acct.memory(), WASM_PAGE);
    let message = first.message("m");
    let second = thread::scope(|scope| {
        scope
            .spawn(|| {
                let second = lane.env();
                second.receive(message, c"shared");
                assert_eq!(second.eval("String(shared.grow(15))"), "1");
                second
            })
            .join()
            .unwrap()
    });
    assert_eq!(
        acct.memory(),
        16 * WASM_PAGE,
        "a shared commit was charged twice"
    );
    assert_eq!(
        first.eval("String(m.buffer.byteLength)"),
        (16 * WASM_PAGE).to_string()
    );
    first.release();
    assert_eq!(acct.memory(), 16 * WASM_PAGE);
    second.release();
    assert_eq!(acct.memory(), 0);
    lane.finish();
}

/// Runs `call` (a JS expression that never returns) and terminates the
/// environment from another thread, as the embedder's kill path does.
fn assert_terminable(env: &v8_lane::Env, call: &str) {
    let started = Instant::now();
    let result = thread::scope(|scope| {
        scope.spawn(|| {
            thread::sleep(Duration::from_millis(200));
            env.terminate();
        });
        env.try_eval(&format!("{call}; 'returned'"))
    });
    assert_eq!(result, Err(NAPI_PENDING_EXCEPTION), "{call}");
    assert!(started.elapsed() < Duration::from_secs(10));
    env.cancel_terminate();
}

#[test]
fn runaway_wasm_is_terminable() {
    let lane = metered_lane(64 * MIB, Caps::default());
    let env = lane.env();
    env.load_wasmgen();
    env.eval(
        "globalThis.spin = new WebAssembly.Instance(new WebAssembly.Module(wasmgen.spin())).exports.spin;
         globalThis.wait = new WebAssembly.Instance(new WebAssembly.Module(wasmgen.wait())).exports.wait; ''",
    );
    assert_terminable(&env, "spin()");
    assert_terminable(&env, "wait()");
    assert_eq!(env.eval("'alive'"), "alive");
    env.release();
    lane.finish();
}

#[test]
fn code_is_charged_per_context_and_not_shared() {
    let first = metered_lane(256 * MIB, Caps::default());
    let second = metered_lane(256 * MIB, Caps::default());
    let run = "const i = new WebAssembly.Instance(new WebAssembly.Module(wasmgen.loads(32, 400)));
               for (const f of Object.values(i.exports)) if (typeof f === 'function') f(0);
               globalThis.keep = i; ''";
    let a = first.env();
    a.load_wasmgen();
    let process_before = wasmer_napi::wasm_process_stats().code_committed_bytes;
    a.eval(run);
    let code = first.accountant.code();
    println!("32 functions x 400 loads: {code} code bytes charged");
    assert!(code > 32 * 400 * 4, "only {code} code bytes charged");
    assert_eq!(first.usage().code_bytes, code);
    assert!(wasmer_napi::wasm_process_stats().code_committed_bytes >= process_before + code);

    // The same module in another context compiles (and is charged) again:
    // no compiled code is shared between contexts.
    let b = second.env();
    b.load_wasmgen();
    b.eval(run);
    assert!(second.accountant.code() >= code / 2);
    assert_eq!(first.accountant.code(), code);

    // Environments of one thread are entered and must be released LIFO.
    b.release();
    assert_eq!(first.accountant.code(), code);
    a.release();
    assert_eq!(first.accountant.code(), 0);
    first.finish();
    second.finish();
}

#[test]
fn code_budget_stops_the_context_and_refuses_new_compiles() {
    const BUDGET: u64 = 512 * 1024;
    let lane = metered_lane(
        256 * MIB,
        Caps {
            code_budget_bytes: BUDGET,
            ..Caps::default()
        },
    );
    let acct = Arc::clone(&lane.accountant);
    let env = lane.env();
    env.load_wasmgen();
    let stops = wasmer_napi::wasm_process_stats().code_limit_stops;
    let result = env.try_eval(
        "const i = new WebAssembly.Instance(new WebAssembly.Module(wasmgen.loads(512, 400)));
         for (const f of Object.values(i.exports)) if (typeof f === 'function') f(0);
         'never stopped'",
    );
    assert_eq!(
        result,
        Err(NAPI_PENDING_EXCEPTION),
        "the code budget did not stop JS"
    );
    let limits = acct.code_limits.lock().unwrap().clone();
    assert_eq!(limits.len(), 1, "{limits:?}");
    assert_eq!((limits[0].0, limits[0].2), (LIMIT_BUDGET, BUDGET));
    assert!(wasmer_napi::wasm_process_stats().code_limit_stops > stops);
    // Overshoot is bounded by what compiles before termination lands.
    let peak = acct.peak_code.load(Ordering::SeqCst);
    println!("code budget {BUDGET}, peak charge {peak}");
    assert!(peak > BUDGET && peak < BUDGET + 2 * MIB, "peak {peak}");

    // Even if the guest cancels termination, it cannot compile again.
    env.cancel_terminate();
    assert_eq!(
        env.eval(&outcome("new WebAssembly.Module(wasmgen.add())")),
        "CompileError"
    );
    env.release();
    lane.finish();
}

#[test]
fn refused_code_charge_stops_the_context_without_leaking() {
    // Room for the context's wasm memory but not its code.
    let lane = metered_lane(4 * MIB, Caps::default());
    let acct = Arc::clone(&lane.accountant);
    let env = lane.env();
    env.load_wasmgen();
    env.eval("globalThis.m = new WebAssembly.Memory({ initial: 60 }); ''");
    let result = env.try_eval(
        "const i = new WebAssembly.Instance(new WebAssembly.Module(wasmgen.loads(512, 400)));
         for (const f of Object.values(i.exports)) if (typeof f === 'function') f(0);
         'never stopped'",
    );
    assert_eq!(result, Err(NAPI_PENDING_EXCEPTION));
    let limits = acct.code_limits.lock().unwrap().clone();
    assert_eq!(limits.first().map(|limit| limit.0), Some(LIMIT_MEMORY));
    assert!(acct.charged() <= 4 * MIB);
    env.release();
    lane.finish();
}

#[test]
fn module_size_and_function_count_are_capped() {
    let lane = metered_lane(256 * MIB, Caps::default());
    let env = lane.env();
    env.load_wasmgen();
    let size_error = "(() => { try { new WebAssembly.Module(new Uint8Array(SIZE)); return 'ok'; }
                       catch (e) { return e.constructor.name + ': ' + e.message; } })()";
    let over = env.eval(&size_error.replace("SIZE", "(16 << 20) + 1"));
    assert!(
        over.contains("buffer source exceeds maximum size of 16777216"),
        "{over}"
    );
    // At the cap the bytes reach the decoder (and fail as a module).
    let at = env.eval(&size_error.replace("SIZE", "16 << 20"));
    assert!(
        at.starts_with("CompileError: ") && !at.contains("maximum size"),
        "{at}"
    );
    assert_eq!(
        env.eval(&outcome(
            "new WebAssembly.Module(wasmgen.functions(100001))"
        )),
        "CompileError"
    );
    assert_eq!(
        env.eval(&outcome(
            "globalThis.big = new WebAssembly.Module(wasmgen.functions(100000))"
        )),
        "ok"
    );
    // Tables live on the V8 heap: capped at 1M entries.
    for (expr, expected) in [
        (
            "new WebAssembly.Table({ initial: 1000001, element: 'anyfunc' })",
            "RangeError",
        ),
        (
            "new WebAssembly.Table({ initial: 1, element: 'anyfunc' }).grow(1000000)",
            "RangeError",
        ),
        (
            "new WebAssembly.Table({ initial: 1000000, element: 'anyfunc' })",
            "ok",
        ),
    ] {
        assert_eq!(env.eval(&outcome(expr)), expected, "{expr}");
    }
    env.release();
    lane.finish();
}

#[test]
fn dead_modules_return_their_code_charge() {
    let lane = metered_lane(256 * MIB, Caps::default());
    let acct = Arc::clone(&lane.accountant);
    let env = lane.env();
    env.load_wasmgen();
    env.eval(
        "globalThis.keep = new WebAssembly.Instance(new WebAssembly.Module(wasmgen.loads(32, 400)));
         for (let n = 0; n < 32; n++) keep.exports['f' + n](0); ''",
    );
    let charged = acct.code();
    assert!(charged > 0);
    env.eval("keep = null; ''");
    assert!(
        env.gc_until(|| acct.code() == 0),
        "a collected module kept {} code bytes charged",
        acct.code()
    );
    env.release();
    lane.finish();
}

/// Code commits are charged to the context running when V8 commits them
/// (an import-wrapper space is shared), decommits return exactly those
/// charges, and a context's teardown returns what it still holds in spaces
/// that outlive it.
#[test]
fn code_spans_follow_the_committing_context() {
    const PAGE: usize = 64 * 1024;
    let a = metered_lane(64 * MIB, Caps::default());
    let b = metered_lane(64 * MIB, Caps::default());
    a.env().release();
    let decommitted = wasmer_napi::wasm_process_stats().code_decommitted_bytes;

    let region = {
        let _bound = a.bind();
        unsafe { snapi_v8_test_code_reserve(16 * PAGE) }
    };
    assert!(!region.is_null());
    let at = |offset: usize| unsafe { region.cast::<u8>().add(offset) }.cast();
    {
        let _bound = b.bind();
        assert!(unsafe { snapi_v8_test_code_commit(at(0), 2 * PAGE, true) });
    }
    assert_eq!(
        (a.accountant.code(), b.accountant.code()),
        (0, 2 * PAGE as u64)
    );
    // Without a metered lane bound, the reserving context pays.
    assert!(unsafe { snapi_v8_test_code_commit(at(2 * PAGE), 2 * PAGE, true) });
    assert_eq!(a.accountant.code(), 2 * PAGE as u64);
    // Recommitting committed pages charges nothing more.
    {
        let _bound = a.bind();
        assert!(unsafe { snapi_v8_test_code_commit(at(0), 4 * PAGE, true) });
    }
    assert_eq!(
        a.accountant.charged() + b.accountant.charged(),
        4 * PAGE as u64
    );
    // A decommit across both owners returns each one's share.
    assert!(unsafe { snapi_v8_test_code_commit(at(PAGE), 2 * PAGE, false) });
    assert_eq!(
        (a.accountant.code(), b.accountant.code()),
        (PAGE as u64, PAGE as u64)
    );
    assert_eq!(
        wasmer_napi::wasm_process_stats().code_decommitted_bytes - decommitted,
        2 * PAGE as u64
    );
    // B's teardown returns what B still holds in the surviving space.
    b.finish();
    assert_eq!(a.accountant.code(), PAGE as u64);
    assert!(unsafe { snapi_v8_test_page_free(region, 16 * PAGE) });
    a.finish();
}

/// Non-wasm JavaScript in a metering process: neither the page-allocator
/// hot path nor V8's code range sees wasm bookkeeping. Prints the cost of a
/// buffer-heavy JS loop with and without metered WebAssembly in the same
/// process. Run with `--ignored --nocapture`.
#[test]
#[ignore]
fn non_wasm_javascript_cost() {
    const SCRIPT: &str = "(() => {
        let n = 0;
        for (let i = 0; i < 2000; i++) {
          const o = JSON.parse(JSON.stringify({ a: i, b: [i, i + 1], c: 'x'.repeat(64) }));
          const b = new ArrayBuffer(0, { maxByteLength: 1 << 20 });
          b.resize(4096); new Uint8Array(b)[0] = o.a;
          n += o.b[1] + new Uint8Array(b)[0];
        }
        return String(n);
      })()";
    let metered = metered_lane(256 * MIB, Caps::default());
    let plain = Lane::new(256 * MIB, None);
    let metered_env = metered.env();
    let plain_env = plain.try_env(POLICY_RESTRICTED).unwrap();
    let code_before = wasmer_napi::wasm_process_stats().code_committed_bytes;
    let time = |env: &v8_lane::Env| {
        let start = Instant::now();
        for _ in 0..20 {
            env.eval(SCRIPT);
        }
        start.elapsed().as_secs_f64() * 1e3 / 20.0
    };
    for round in 0..5 {
        let (p, m) = (time(&plain_env), time(&metered_env));
        println!(
            "round {round}: plain {p:.3} ms/run, metered {m:.3} ms/run, delta {:+.2}%",
            (m / p - 1.0) * 100.0
        );
    }
    assert_eq!(
        wasmer_napi::wasm_process_stats().code_committed_bytes,
        code_before,
        "non-wasm JavaScript committed metered wasm code"
    );
    assert_eq!(metered.accountant.code(), 0);
    plain_env.release();
    metered_env.release();
    metered.finish();
    plain.finish();
}
