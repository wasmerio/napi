//! Exercise the N-API refill hook's managed and NoYield reservation paths.
#![cfg(all(feature = "wasix", not(target_arch = "wasm32")))]

use std::{
    collections::VecDeque,
    future::Future,
    pin::Pin,
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
    task::{Context, Poll},
};

use wasixv2_exec::{Abis, run_on_owning_thread};
use wasixv2_host::{LocalFuture, TokioHost};
use wasixv2_syscalls::{
    AtomicWait, AtomicWord, Caller, Control, CpuTime, Errno, ExitMm, GroupStop, GuestAction, Image,
    LoaderControl, MaybeFuture, SharedMapping, SharedMappings, SysResult, Syscalls, TaskId,
    VforkWait,
};
use wasmer::sys::{Cranelift, EngineBuilder, Features};
use wasmer::{Module, Store};
use wasmer_napi::{NapiCtx, NapiHook};

const MIB: u32 = 1024 * 1024;

const GUEST: &str = r#"(module
    (import "env" "memory" (memory 2 2048 shared))
    (import "wasixv2" "exit_group" (func $exit (param i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
        (func $configure (param i32) (result i32)))
    (import "napi" "napi_wasm_init_env" (func $init (result i32)))
    (import "napi" "napi_create_string_utf8"
        (func $string (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
    (import "napi" "napi_get_value_int32" (func $integer (param i32 i32 i32) (result i32)))
    (table (export "__indirect_function_table") 1 funcref)
    (global (export "__stack_pointer") (mut i32) (i32.const 65536))
    (global (export "__data_end") i32 (i32.const 65536))
    (global (export "__heap_base") i32 (i32.const 65536))
    (data (i32.const 1024) "(() => { try { globalThis.keep = new Uint8Array(33554432); return 41; } catch (_) { return 42; } })()\00")
    (data (i32.const 2048) "42\00")
    (global $env (mut i32) (i32.const 0))
    (func $check (param i32) local.get 0 if unreachable end)
    (func $eval (param $source i32)
        global.get $env local.get $source i32.const -1 i32.const 8 call $string call $check
        global.get $env i32.const 8 i32.load i32.const 12 call $script call $check
        global.get $env i32.const 12 i32.load i32.const 16 call $integer call $check
        i32.const 16 i32.load i32.const 42 i32.ne if unreachable end)
    (func (export "_start")
        i32.const 0 call $configure call $check
        call $init global.set $env
        global.get $env i32.eqz if unreachable end
        i32.const 1024 call $eval
        i32.const 2048 call $eval
        i32.const 0 call $exit))"#;

#[derive(Default)]
struct RefillCounts {
    calls: AtomicUsize,
    polls: AtomicUsize,
    resumed: AtomicUsize,
    small_resumed: AtomicUsize,
    dropped_after_one_poll: AtomicUsize,
    small_dropped_after_one_poll: AtomicUsize,
    large_requests: AtomicUsize,
    large_resumed: AtomicUsize,
    large_dropped_after_one_poll: AtomicUsize,
    task_releases: AtomicUsize,
    permanent_pending_polls: AtomicUsize,
    permanent_pending_drops: AtomicUsize,
    permanent_pending_first_poll: tokio::sync::Notify,
    permanent_pending_dropped: tokio::sync::Notify,
    task_released: tokio::sync::Notify,
}

struct DropCount {
    counts: Arc<RefillCounts>,
    large: bool,
    polls: usize,
}

impl Drop for DropCount {
    fn drop(&mut self) {
        if self.polls == 1 {
            self.counts
                .dropped_after_one_poll
                .fetch_add(1, Ordering::SeqCst);
            if self.large {
                self.counts
                    .large_dropped_after_one_poll
                    .fetch_add(1, Ordering::SeqCst);
            } else {
                self.counts
                    .small_dropped_after_one_poll
                    .fetch_add(1, Ordering::SeqCst);
            }
        }
    }
}

struct PendingThenNoMemory {
    counts: Arc<RefillCounts>,
    large: bool,
    polls: usize,
    _drop_count: DropCount,
}

impl Future for PendingThenNoMemory {
    type Output = SysResult<Box<dyn wasixv2_syscalls::HostMemoryLease>>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        self.counts.polls.fetch_add(1, Ordering::SeqCst);
        self.polls += 1;
        self._drop_count.polls = self.polls;
        if self.polls == 1 {
            if self.large {
                self.counts.large_requests.fetch_add(1, Ordering::SeqCst);
            }
            cx.waker().wake_by_ref();
            Poll::Pending
        } else {
            self.counts.resumed.fetch_add(1, Ordering::SeqCst);
            if self.large {
                self.counts.large_resumed.fetch_add(1, Ordering::SeqCst);
            } else {
                self.counts.small_resumed.fetch_add(1, Ordering::SeqCst);
            }
            Poll::Ready(Err(Errno::ENOMEM.into()))
        }
    }
}

struct PendingForever {
    counts: Arc<RefillCounts>,
    polls: usize,
}

impl Future for PendingForever {
    type Output = SysResult<Box<dyn wasixv2_syscalls::HostMemoryLease>>;

    fn poll(mut self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<Self::Output> {
        self.polls += 1;
        self.counts
            .permanent_pending_polls
            .fetch_add(1, Ordering::SeqCst);
        if self.polls == 1 {
            self.counts.permanent_pending_first_poll.notify_one();
        }
        // Deliberately never wake itself. Only caller cancellation can retire it.
        Poll::Pending
    }
}

impl Drop for PendingForever {
    fn drop(&mut self) {
        if self.polls > 0 {
            self.counts
                .permanent_pending_drops
                .fetch_add(1, Ordering::SeqCst);
            self.counts.permanent_pending_dropped.notify_one();
        }
    }
}

#[derive(Default)]
struct TestSyscalls {
    actions: Mutex<VecDeque<(TaskId, GuestAction)>>,
    refill: Arc<RefillCounts>,
    permanent_pending: bool,
}

impl Syscalls for TestSyscalls {
    fn exit_group<'a>(
        &'a self,
        caller: &'a Caller<'_>,
        status: i32,
    ) -> MaybeFuture<'a, SysResult<()>> {
        self.actions
            .lock()
            .unwrap()
            .push_back((caller.task(), GuestAction::Exit(status)));
        MaybeFuture::Ready(Ok(()))
    }
}

impl LoaderControl for TestSyscalls {
    fn loader_reserve_host<'a>(
        &'a self,
        _: &'a Caller<'_>,
        bytes: u32,
        _: u32,
    ) -> LocalFuture<'a, SysResult<Box<dyn wasixv2_syscalls::HostMemoryLease>>> {
        self.refill.calls.fetch_add(1, Ordering::SeqCst);
        if self.permanent_pending {
            return Box::pin(PendingForever {
                counts: self.refill.clone(),
                polls: 0,
            });
        }
        let large = bytes > 32 * MIB;
        let drop_count = DropCount {
            counts: self.refill.clone(),
            large,
            polls: 0,
        };
        Box::pin(PendingThenNoMemory {
            counts: self.refill.clone(),
            large,
            polls: 0,
            _drop_count: drop_count,
        })
    }
}

impl AtomicWait for TestSyscalls {
    fn atomic_wait<'a>(
        &'a self,
        _: &'a Caller<'a>,
        _: u32,
        _: AtomicWord,
        _: Option<std::time::Duration>,
    ) -> LocalFuture<'a, SysResult<i32>> {
        Box::pin(async { Err(Errno::ENOSYS.into()) })
    }

    fn atomic_notify(&self, _: TaskId, _: u32, _: u32) -> u32 {
        0
    }
}

impl CpuTime for TestSyscalls {}

impl ExitMm for TestSyscalls {
    fn exit_mm<'a>(&'a self, _: &'a Caller<'a>) -> LocalFuture<'a, ()> {
        Box::pin(async {})
    }
}

impl GroupStop for TestSyscalls {
    fn stop_wait(&self, _: TaskId) -> LocalFuture<'_, ()> {
        Box::pin(async {})
    }
}

impl SharedMappings for TestSyscalls {
    fn shared_mappings(&self, _: TaskId) -> Vec<SharedMapping> {
        Vec::new()
    }

    fn wipe_on_fork(&self, _: TaskId) -> Vec<std::ops::Range<u64>> {
        Vec::new()
    }
}

impl VforkWait for TestSyscalls {
    fn vfork_wait(&self, _: TaskId) -> LocalFuture<'_, ()> {
        Box::pin(async {})
    }
}

impl Control for TestSyscalls {
    fn take_pending(&self, task: TaskId) -> Option<GuestAction> {
        let mut actions = self.actions.lock().unwrap();
        let index = actions.iter().position(|(pending, _)| *pending == task)?;
        actions.remove(index).map(|(_, action)| action)
    }

    fn take_image(&self, _: TaskId) -> Option<Image> {
        None
    }

    fn release(&self, _: TaskId) {
        self.refill.task_releases.fetch_add(1, Ordering::SeqCst);
        self.refill.task_released.notify_one();
    }
}

#[tokio::test]
async fn napi_hook_suspends_advisory_refill_and_drops_allocator_pending_in_no_yield() {
    const CHILD: &str = "WASIXV2_NAPI_PENDING_CHILD";
    if std::env::var_os(CHILD).is_none() {
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "napi_hook_suspends_advisory_refill_and_drops_allocator_pending_in_no_yield",
                "--nocapture",
            ])
            .env(CHILD, "1")
            .spawn()
            .unwrap();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(45);
        loop {
            if let Some(status) = child.try_wait().unwrap() {
                assert!(status.success(), "pending refill child failed: {status}");
                return;
            }
            if std::time::Instant::now() >= deadline {
                let _ = child.kill();
                let _ = child.wait();
                panic!("pending refill child did not finish");
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }

    tokio::task::LocalSet::new()
        .run_until(async {
            let context = NapiCtx::builder().max_sessions(1).max_envs(1).build();
            let mut features = Features::default();
            features.exceptions(true);
            let mut engine = EngineBuilder::new(Cranelift::default())
                .set_features(Some(features))
                .engine();
            engine.set_tunables(wasmer_napi::budgeted_tunables(context.budget()));
            let store = Store::new(engine.clone());
            let module = Module::new(&store, wat::parse_str(GUEST).unwrap()).unwrap();
            drop(store);

            let syscalls = Arc::new(TestSyscalls::default());
            let mut abis = Abis::<TestSyscalls>::wasixv2();
            abis.push_instantiation_hook(Arc::new(NapiHook::new(context.clone())));
            let result = run_on_owning_thread(
                engine,
                &module,
                syscalls.clone(),
                TaskId(1),
                Arc::new(TokioHost::default()),
                Arc::new(abis),
            )
            .await;

            assert_eq!(result.unwrap(), 0);
            assert!(syscalls.refill.calls.load(Ordering::SeqCst) >= 2);
            assert!(
                syscalls.refill.small_resumed.load(Ordering::SeqCst) >= 1,
                "the import-boundary advisory refill must cross a real managed Pending"
            );
            assert!(
                syscalls.refill.large_requests.load(Ordering::SeqCst) >= 1,
                "the guest must request a backing larger than the 32 MiB allocation"
            );
            assert_eq!(
                syscalls.refill.large_resumed.load(Ordering::SeqCst),
                0,
                "V8 allocator callbacks must refuse Pending without suspending"
            );
            assert_eq!(
                syscalls
                    .refill
                    .small_dropped_after_one_poll
                    .load(Ordering::SeqCst),
                0,
                "managed advisory reservations resume instead of being canceled"
            );
            assert!(
                syscalls
                    .refill
                    .large_dropped_after_one_poll
                    .load(Ordering::SeqCst)
                    >= 1,
                "the NoYield allocator must synchronously drop the pending reservation"
            );
            assert_eq!(context.active_sessions(), 0);
            let budget = context.budget().snapshot();
            assert_eq!(budget.live_isolates, 0);
            assert_eq!(budget.v8_heap_reserved, 0);
            assert_eq!(budget.v8_external, 0);
            assert_eq!(budget.wasm_linear, 0);
            assert_eq!(syscalls.refill.task_releases.load(Ordering::SeqCst), 1);
        })
        .await;
}

#[tokio::test]
async fn dropping_the_caller_cancels_a_permanently_pending_napi_refill() {
    const CHILD: &str = "WASIXV2_NAPI_PENDING_CANCEL_CHILD";
    if std::env::var_os(CHILD).is_none() {
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "dropping_the_caller_cancels_a_permanently_pending_napi_refill",
                "--nocapture",
            ])
            .env(CHILD, "1")
            .spawn()
            .unwrap();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(45);
        loop {
            if let Some(status) = child.try_wait().unwrap() {
                assert!(
                    status.success(),
                    "pending cancellation child failed: {status}"
                );
                return;
            }
            if std::time::Instant::now() >= deadline {
                let _ = child.kill();
                let _ = child.wait();
                panic!("pending cancellation child did not finish");
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }

    tokio::task::LocalSet::new()
        .run_until(async {
            let context = NapiCtx::builder().max_sessions(1).max_envs(1).build();
            let mut features = Features::default();
            features.exceptions(true);
            let mut engine = EngineBuilder::new(Cranelift::default())
                .set_features(Some(features))
                .engine();
            engine.set_tunables(wasmer_napi::budgeted_tunables(context.budget()));
            let store = Store::new(engine.clone());
            let module = Module::new(&store, wat::parse_str(GUEST).unwrap()).unwrap();
            drop(store);

            let syscalls = Arc::new(TestSyscalls {
                permanent_pending: true,
                ..TestSyscalls::default()
            });
            let mut abis = Abis::<TestSyscalls>::wasixv2();
            abis.push_instantiation_hook(Arc::new(NapiHook::new(context.clone())));
            let mut run = Box::pin(run_on_owning_thread(
                engine,
                &module,
                syscalls.clone(),
                TaskId(2),
                Arc::new(TokioHost::default()),
                Arc::new(abis),
            ));

            let first_poll = syscalls.refill.permanent_pending_first_poll.notified();
            tokio::pin!(first_poll);
            let reached_pending = tokio::time::timeout(std::time::Duration::from_secs(10), async {
                tokio::select! {
                    _ = &mut first_poll => (),
                    result = &mut run => panic!("guest completed before refill became pending: {result:?}"),
                }
            })
            .await;
            assert!(reached_pending.is_ok(), "the N-API advisory refill did not become pending");
            assert!(
                syscalls
                    .refill
                    .permanent_pending_polls
                    .load(Ordering::SeqCst)
                    >= 1,
                "the cancellation probe must reach a real pending loader reservation"
            );
            assert_eq!(context.active_sessions(), 1);

            let refill_dropped = syscalls.refill.permanent_pending_dropped.notified();
            let task_released = syscalls.refill.task_released.notified();
            tokio::pin!(refill_dropped);
            tokio::pin!(task_released);
            drop(run);

            let retired = tokio::time::timeout(std::time::Duration::from_secs(3), async {
                tokio::join!(&mut refill_dropped, &mut task_released);
            })
            .await;
            assert!(
                retired.is_ok(),
                "dropping the caller must drop the pending refill and release its task; observed refill_drops={}, task_releases={}, active_sessions={}",
                syscalls
                    .refill
                    .permanent_pending_drops
                    .load(Ordering::SeqCst),
                syscalls.refill.task_releases.load(Ordering::SeqCst),
                context.active_sessions(),
            );
            assert_eq!(
                syscalls
                    .refill
                    .permanent_pending_drops
                    .load(Ordering::SeqCst),
                1
            );
            assert_eq!(syscalls.refill.task_releases.load(Ordering::SeqCst), 1);
            assert_eq!(context.active_sessions(), 0);
            let budget = context.budget().snapshot();
            assert_eq!(budget.live_isolates, 0);
            assert_eq!(budget.v8_heap_reserved, 0);
            assert_eq!(budget.v8_external, 0);
            assert_eq!(budget.wasm_linear, 0);
            assert_eq!(budget.mem_charged, 0);
        })
        .await;
}
