#![cfg(feature = "cli")]

//! A budgeted or managed application shares the process with others: the
//! host refuses tools that sample, snapshot or copy outside its budget, does
//! not consume code caches the guest supplies, and leaves WebAssembly out
//! unless the embedder enables it. A standalone application keeps all of it.

use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
};

use wasmer_napi::{
    NapiCtx, NapiMemoryAccountant, WasmPolicy, cli::run_wasix_main_capture_stdio_with_ctx,
};

mod common;
use common::build_wasix_test;

const MIB: u64 = 1024 * 1024;

fn run(ctx: &NapiCtx, args: &[&str]) -> (i32, String, String) {
    let wasm = build_wasix_test("test_resource_limit_rules");
    let args: Vec<String> = args.iter().map(|arg| arg.to_string()).collect();
    run_wasix_main_capture_stdio_with_ctx(ctx, &wasm, &args, &[])
        .unwrap_or_else(|error| panic!("the guest failed with {args:?}: {error:#}"))
}

/// A code cache a standalone run produced, which a limited run must reject.
fn standalone_cache() -> String {
    let (exit_code, stdout, stderr) = run(&NapiCtx::default(), &["standalone"]);
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    stdout
        .lines()
        .find_map(|line| line.strip_prefix("CACHE_HEX="))
        .expect("the standalone run printed no cache")
        .to_string()
}

#[test]
fn standalone_contexts_keep_profilers_code_caches_and_webassembly() {
    let (exit_code, stdout, stderr) = run(&NapiCtx::default(), &["standalone"]);
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("RESOURCE_LIMIT_RULES_OK"),
        "{stdout}\n{stderr}"
    );
}

#[test]
fn budgeted_contexts_refuse_them() {
    // Room for the isolate and the guest, not for a 200 MB serialization.
    let ctx = NapiCtx::builder().total_memory_bytes(256 * MIB).build();
    assert_eq!(ctx.webassembly_policy(), WasmPolicy::Restricted);
    let cache = standalone_cache();
    let (exit_code, stdout, stderr) = run(&ctx, &["limited", &cache]);
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("RESOURCE_LIMIT_RULES_OK"),
        "{stdout}\n{stderr}"
    );
}

#[test]
fn an_explicit_webassembly_policy_wins_over_the_default() {
    let ctx = NapiCtx::builder()
        .total_memory_bytes(512 * MIB)
        .webassembly(WasmPolicy::EnabledUnmetered)
        .build();
    assert_eq!(ctx.webassembly_policy(), WasmPolicy::EnabledUnmetered);
    assert_eq!(
        NapiCtx::builder()
            .webassembly(WasmPolicy::Restricted)
            .build()
            .webassembly_policy(),
        WasmPolicy::Restricted
    );
    assert_eq!(
        NapiCtx::default().webassembly_policy(),
        WasmPolicy::EnabledUnmetered
    );
}

/// An accountant makes the context budgeted for good, whatever limit it
/// reports when the context is built: one that starts unlimited may impose a
/// limit later, so the WebAssembly default does not depend on its number.
#[test]
fn an_accountant_makes_the_context_budgeted_whatever_its_limit() {
    struct Accountant(AtomicU64);
    impl NapiMemoryAccountant for Accountant {
        fn memory_limit(&self) -> u64 {
            u64::MAX
        }
        fn memory_charged(&self) -> u64 {
            self.0.load(Ordering::Acquire)
        }
        fn try_charge(&self, bytes: u64) -> bool {
            self.0.fetch_add(bytes, Ordering::AcqRel);
            true
        }
        fn uncharge(&self, bytes: u64) {
            self.0.fetch_sub(bytes, Ordering::AcqRel);
        }
    }
    let ctx = NapiCtx::builder()
        .memory_accountant(Arc::new(Accountant(AtomicU64::new(0))))
        .build();
    assert_eq!(ctx.webassembly_policy(), WasmPolicy::Restricted);
    assert!(ctx.budget().is_budgeted());
}
