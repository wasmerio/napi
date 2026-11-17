#![cfg(feature = "cli")]

//! A budgeted or managed application shares the process with others: the
//! host refuses tools that sample, snapshot or copy outside its budget, does
//! not consume code caches the guest supplies, and leaves WebAssembly out
//! unless the embedder enables it. A standalone application keeps all of it.

use wasmer_napi::{NapiCtx, WasmPolicy, cli::run_wasix_main_capture_stdio_with_ctx};

mod common;
use common::build_wasix_test;

const MIB: u64 = 1024 * 1024;

fn run(ctx: &NapiCtx, mode: &str) -> (i32, String, String) {
    let wasm = build_wasix_test("test_resource_limit_rules");
    run_wasix_main_capture_stdio_with_ctx(ctx, &wasm, &[mode.to_string()], &[])
        .unwrap_or_else(|error| panic!("the {mode} guest failed: {error:#}"))
}

#[test]
fn standalone_contexts_keep_profilers_code_caches_and_webassembly() {
    let (exit_code, stdout, stderr) = run(&NapiCtx::default(), "standalone");
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("RESOURCE_LIMIT_RULES_OK"),
        "{stdout}\n{stderr}"
    );
}

#[test]
fn budgeted_contexts_refuse_them() {
    let ctx = NapiCtx::builder().total_memory_bytes(512 * MIB).build();
    assert_eq!(ctx.webassembly_policy(), WasmPolicy::Restricted);
    let (exit_code, stdout, stderr) = run(&ctx, "limited");
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
