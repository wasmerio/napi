#![cfg(feature = "cli")]

//! Memory N-API holds on the host for a guest is charged to the context's
//! budget: messages between its environments, the bridge's handle tables, and
//! host copies of guest memory.

use std::{
    thread,
    time::{Duration, Instant},
};

use wasmer_napi::{NapiCtx, ResourceUsage, cli::run_wasix_main_capture_stdio_with_ctx};

mod common;
use common::build_wasix_test;

const MIB: u64 = 1024 * 1024;
const GENEROUS_BUDGET: u64 = 1024 * MIB;

fn run(ctx: &NapiCtx, program: &str, args: &[String]) -> (i32, String, String) {
    let wasm = build_wasix_test(program);
    run_wasix_main_capture_stdio_with_ctx(ctx, &wasm, args, &[]).expect("the guest did not run")
}

/// The context's usage once the exited guests' isolates are released, which
/// finishes after the run returns.
fn usage_after_teardown(ctx: &NapiCtx) -> ResourceUsage {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let usage = ctx.budget().snapshot();
        if usage.live_isolates == 0 || Instant::now() >= deadline {
            return usage;
        }
        thread::sleep(Duration::from_millis(5));
    }
}

fn pending_message_id(stdout: &str) -> String {
    stdout
        .lines()
        .find_map(|line| line.strip_prefix("PENDING_MESSAGE="))
        .expect("the guest printed no message id")
        .to_string()
}

/// A queued message stays charged after its sender exits, until the context
/// stops: `terminate_all` frees it.
#[test]
fn guest_messages_remain_charged_until_instance_shutdown() {
    let ctx = NapiCtx::builder()
        .total_memory_bytes(GENEROUS_BUDGET)
        .build();
    let budget = ctx.budget();
    let (exit_code, stdout, stderr) = run(&ctx, "test_guest_message_budget", &[]);
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(stdout.contains("MESSAGE_BUDGET_OK"), "{stdout}\n{stderr}");
    assert!(
        budget.snapshot().serialized_message > 0,
        "queued message escaped memory accounting"
    );
    ctx.runtime_control().terminate_all();
    assert_eq!(budget.snapshot().serialized_message, 0);
    let usage = usage_after_teardown(&ctx);
    assert_eq!(usage.mem_charged, 0, "{usage:?}");
}

/// A guest cannot take or drop another context's message by its id.
#[test]
fn guests_cannot_take_another_contexts_messages() {
    let owner = NapiCtx::builder()
        .total_memory_bytes(GENEROUS_BUDGET)
        .build();
    let (exit_code, stdout, stderr) = run(&owner, "test_guest_message_budget", &[]);
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    let id = pending_message_id(&stdout);
    let charged = owner.budget().snapshot().serialized_message;
    assert!(charged > 0);

    let intruder = NapiCtx::default();
    let (exit_code, stdout, stderr) = run(&intruder, "test_foreign_message", &[id]);
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("FOREIGN_MESSAGE_REFUSED"),
        "{stdout}\n{stderr}"
    );
    // The message is still the owner's, charged, until the owner stops.
    assert_eq!(owner.budget().snapshot().serialized_message, charged);
    owner.runtime_control().terminate_all();
    assert_eq!(owner.budget().snapshot().serialized_message, 0);
}

/// A message larger than what the budget has left is refused as a clone
/// error the guest can catch, and nothing stays charged for it.
#[test]
fn a_message_over_the_budget_is_a_catchable_clone_error() {
    // Room for the isolate (88 MiB) and the guest's memory with its 40 MiB
    // ArrayBuffer (about 230 MiB together), not for a 40 MiB copy besides.
    let ctx = NapiCtx::builder().total_memory_bytes(240 * MIB).build();
    let budget = ctx.budget();
    let (exit_code, stdout, stderr) = run(&ctx, "test_message_over_budget", &[]);
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(
        stdout.contains("MESSAGE_OVER_BUDGET_OK"),
        "{stdout}\n{stderr}\n{:?}",
        budget.snapshot()
    );
    assert_eq!(budget.snapshot().serialized_message, 0);
}

/// The bridge's handle tables are charged: a guest that keeps creating
/// references is refused once the budget is spent, instead of growing host
/// memory past it. Everything is returned when the guest exits.
#[test]
fn reference_bookkeeping_is_charged_and_refused_at_the_budget() {
    let ctx = NapiCtx::builder().total_memory_bytes(160 * MIB).build();
    let budget = ctx.budget();
    let (exit_code, stdout, stderr) = run(&ctx, "test_ref_bookkeeping", &[]);
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(stdout.contains("REF_BOOKKEEPING_OK"), "{stdout}\n{stderr}");
    assert!(stdout.contains("REFUSED=1"), "{stdout}\n{stderr}");
    let usage = usage_after_teardown(&ctx);
    assert_eq!(usage.host_bookkeeping, 0, "{usage:?}");
    assert_eq!(usage.mem_charged, 0, "{usage:?}");

    // Without a budget nothing is refused.
    let (exit_code, stdout, stderr) = run(&NapiCtx::default(), "test_ref_bookkeeping", &[]);
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}");
    assert!(stdout.contains("REFUSED=0"), "{stdout}\n{stderr}");
}

/// A host copy of guest memory the budget cannot hold is refused.
#[test]
fn host_copies_of_guest_memory_are_charged() {
    // Room for the isolate (88 MiB) and the guest's memory with its 48 MiB
    // string (about 190 MiB together), not for a 48 MiB copy besides.
    let ctx = NapiCtx::builder().total_memory_bytes(210 * MIB).build();
    let budget = ctx.budget();
    let (exit_code, stdout, stderr) = run(&ctx, "test_transient_copy", &["48".to_string()]);
    assert_eq!(exit_code, 0, "{stdout}\n{stderr}\n{:?}", budget.snapshot());
    assert!(stdout.contains("TRANSIENT_COPY_OK"), "{stdout}\n{stderr}");
    assert_eq!(budget.snapshot().host_transient, 0);
}
