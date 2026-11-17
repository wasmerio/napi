#![cfg(feature = "cli")]

//! A budgeted or managed application shares the process with others: the
//! host refuses tools that sample, snapshot or copy outside its budget, does
//! not consume code caches the guest supplies, and leaves WebAssembly out
//! unless the embedder enables it. A standalone application keeps all of it.

use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
    mpsc,
};

use wasmer_napi::{
    NapiCtx, NapiMemoryAccountant, WasmPolicy, cli::run_wasix_main_capture_stdio_with_ctx,
};

mod common;
use common::build_wasix_test;
mod wat_guest;

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
    struct Accountant {
        limit: AtomicU64,
        charged: AtomicU64,
    }
    impl NapiMemoryAccountant for Accountant {
        fn memory_limit(&self) -> u64 {
            self.limit.load(Ordering::Acquire)
        }
        fn memory_charged(&self) -> u64 {
            self.charged.load(Ordering::Acquire)
        }
        fn try_charge(&self, bytes: u64) -> bool {
            self.charged
                .try_update(Ordering::AcqRel, Ordering::Acquire, |charged| {
                    charged
                        .checked_add(bytes)
                        .filter(|next| *next <= self.memory_limit())
                })
                .is_ok()
        }
        fn uncharge(&self, bytes: u64) {
            self.charged.fetch_sub(bytes, Ordering::AcqRel);
        }
    }
    let accountant = Arc::new(Accountant {
        limit: AtomicU64::new(u64::MAX),
        charged: AtomicU64::new(0),
    });
    let ctx = NapiCtx::builder()
        .memory_accountant(accountant.clone())
        .build();
    assert_eq!(ctx.webassembly_policy(), WasmPolicy::Restricted);
    assert!(ctx.budget().is_budgeted());

    let (engine, module) = wat_guest::compile();
    let (signal_tx, _signal_rx) = mpsc::channel();
    let mut guest = wat_guest::Guest::new(&ctx, &engine, &module, signal_tx);
    guest.create();
    assert_eq!(guest.install_serdes(), 0);

    // Leave room for the script text and a small serialization. The same
    // isolate must also refuse a larger serialization after the limit changes.
    accountant
        .limit
        .store(accountant.memory_charged() + 4096, Ordering::Release);
    assert_eq!(
        guest.number("const s = new serdes.Serializer(); s.writeValue(42) ? 1 : 0"),
        1
    );
    assert!(ctx.budget().snapshot().host_transient > 0);
    assert_eq!(
        guest.number(
            "(() => {
                const s = new serdes.Serializer();
                try {
                    s.writeValue('x'.repeat(32768));
                    return 0;
                } catch (error) {
                    return error.message.includes('out of memory') ? 1 : 2;
                }
            })()"
        ),
        1
    );
    assert_eq!(guest.release(), 0);
    assert_eq!(ctx.budget().snapshot().host_transient, 0);
}
