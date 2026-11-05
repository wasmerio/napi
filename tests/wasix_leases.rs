//! Real V8 backing stores remain protected kernel leases through guest access and GC.
#![cfg(all(feature = "wasix", not(target_arch = "wasm32")))]

use std::sync::{
    Arc, Mutex,
    atomic::{AtomicUsize, Ordering},
};
use wasixv2::{
    ExecEnv, InstanceAttachment, InstantiationHook, Machine, MachineSyscalls, ProcessConfig, Stdio,
};
use wasmer::sys::{Cranelift, EngineBuilder, Features, HostMemoryAccess, NativeEngineExt};
use wasmer::{Engine, FunctionEnv, Imports, Instance, Memory, Module, RuntimeError, StoreMut};
use wasmer_napi::{NapiCtx, NapiHook, budgeted_tunables};

// Match wasix_facade's source-built 32 MiB allocator refill and custom_bindings'
// protected-reservation checks. All memory operations use the actual N-API pointer.
const GUEST: &str = r#"(module
    (import "env" "memory" (memory 2 2048 shared))
    (import "wasixv2" "exit_group" (func $exit (param i32)))
    (import "wasixv2" "mprotect" (func $protect (param i32 i32 i32) (result i32)))
    (import "wasixv2" "munmap" (func $unmap (param i32 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
        (func $configure (param i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_collect_garbage"
        (func $gc (param i32) (result i32)))
    (import "napi" "napi_wasm_init_env" (func $init (result i32)))
    (import "napi" "napi_create_string_utf8"
        (func $string (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
    (import "napi" "napi_get_value_int32" (func $integer (param i32 i32 i32) (result i32)))
    (import "napi" "napi_get_arraybuffer_info"
        (func $info (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_strict_equals"
        (func $same (param i32 i32 i32 i32) (result i32)))
    (table (export "__indirect_function_table") 1 funcref)
    (global (export "__stack_pointer") (mut i32) (i32.const 65536))
    (global (export "__data_end") i32 (i32.const 65536))
    (global (export "__heap_base") i32 (i32.const 65536))
    (global $env (mut i32) (i32.const 0))
    (data (i32.const 1024) "globalThis.keep = new Uint8Array(33554432); keep[0] = 41; keep[33554431] = 1; keep.buffer\00")
    (data (i32.const 2048) "keep.buffer\00")
    (data (i32.const 3072) "keep[0] + keep[33554431]\00")
    (func $check (param i32) local.get 0 if unreachable end)
    (func $equal (param i32 i32)
        local.get 0 local.get 1 i32.ne if unreachable end)
    (func $evaluate (param $source i32) (result i32)
        global.get $env local.get $source i32.const -1 i32.const 8 call $string call $check
        global.get $env i32.const 8 i32.load i32.const 12 call $script call $check
        i32.const 12 i32.load)
    (func $sum (param $expected i32)
        global.get $env i32.const 3072 call $evaluate i32.const 16 call $integer call $check
        i32.const 16 i32.load local.get $expected call $equal)
    (func $busy (param $status i32)
        ;; A protection regression must exit before any later payload access.
        local.get $status i32.const -16 i32.ne
        if i32.const 77 call $exit unreachable end)
    (func $protected (param $data i32) (local $page i32)
        local.get $data i32.const -65536 i32.and local.set $page
        ;; PROT_READ first: unexpected success cannot precede a guest write.
        local.get $page i32.const 65536 i32.const 1 call $protect call $busy
        local.get $page i32.const 65536 call $unmap call $busy)
    (func (export "_start") (local $buffer i32) (local $data i32) (local $after i32)
        i32.const 0 call $configure call $check
        call $init global.set $env
        global.get $env i32.eqz if unreachable end
        ;; Larger than the initial 16 MiB fund: a successful actual V8 refill.
        i32.const 1024 call $evaluate local.set $buffer
        global.get $env local.get $buffer i32.const 32 i32.const 36 call $info call $check
        i32.const 36 i32.load i32.const 33554432 call $equal
        i32.const 32 i32.load local.tee $data i32.eqz if unreachable end
        local.get $data call $protected
        local.get $data i32.load8_u i32.const 41 call $equal
        local.get $data i32.const 33554431 i32.add i32.load8_u i32.const 1 call $equal
        local.get $data i32.const 40 i32.store8
        i32.const 41 call $sum
        global.get $env call $gc call $check
        i32.const 2048 call $evaluate local.set $after
        global.get $env local.get $after i32.const 40 i32.const 44 call $info call $check
        i32.const 40 i32.load local.get $data call $equal
        i32.const 44 i32.load i32.const 33554432 call $equal
        global.get $env local.get $buffer local.get $after i32.const 48 call $same call $check
        i32.const 48 i32.load8_u i32.const 1 call $equal
        local.get $data call $protected
        local.get $data i32.load8_u i32.const 40 call $equal
        local.get $data i32.const 33554431 i32.add i32.load8_u i32.const 1 call $equal
        local.get $data i32.const 33554431 i32.add i32.const 2 i32.store8
        i32.const 42 call $sum
        i32.const 0 call $exit unreachable))"#;

#[derive(Default)]
struct Observation {
    // Only detached allocation/range/accounting custody crosses the run boundary.
    memory: Mutex<Option<HostMemoryAccess>>,
    configured: AtomicUsize,
    deactivated: AtomicUsize,
}

struct ObserverHook(Arc<Observation>);
struct ObserverAttachment(Arc<Observation>);

impl InstantiationHook<MachineSyscalls> for ObserverHook {
    fn prepare_imports(
        &self,
        _: &Module,
        _: &mut StoreMut<'_>,
        _: &FunctionEnv<ExecEnv<MachineSyscalls>>,
        _: &mut Imports,
    ) -> Result<Option<Box<dyn InstanceAttachment>>, RuntimeError> {
        Ok(Some(Box::new(ObserverAttachment(self.0.clone()))))
    }
}

impl InstanceAttachment for ObserverAttachment {
    fn configure(
        &mut self,
        _: &Module,
        store: &mut StoreMut<'_>,
        _: &Instance,
        memory: &Memory,
    ) -> Result<(), RuntimeError> {
        let access = memory
            .host_memory_access(&*store)
            .map_err(|error| RuntimeError::new(error.to_string()))?;
        assert!(
            access.allocation_owner_count() > 1,
            "the observer must own the allocation before runtime teardown"
        );
        let mut retained = self.0.memory.lock().unwrap();
        assert!(retained.is_none(), "this fixture has one actual instance");
        *retained = Some(access);
        self.0.configured.fetch_add(1, Ordering::SeqCst);
        Ok(())
    }

    fn deactivate(&mut self, _: &mut StoreMut<'_>) {
        self.0.deactivated.fetch_add(1, Ordering::SeqCst);
    }
}

#[tokio::test]
async fn native_arraybuffer_backing_is_an_exact_protected_kernel_lease() {
    const CHILD: &str = "WASIXV2_NAPI_PROTECTED_LEASE_CHILD";
    if std::env::var_os(CHILD).is_none() {
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "native_arraybuffer_backing_is_an_exact_protected_kernel_lease",
                "--nocapture",
            ])
            .env(CHILD, "1")
            .spawn()
            .unwrap();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(45);
        loop {
            if let Some(status) = child.try_wait().unwrap() {
                assert!(status.success(), "protected-lease child failed: {status}");
                return;
            }
            if std::time::Instant::now() >= deadline {
                let _ = child.kill();
                let _ = child.wait();
                panic!("protected-lease child did not finish");
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }

    tokio::task::LocalSet::new()
        .run_until(async {
            for shared in [false, true] {
                let context = NapiCtx::builder().max_sessions(1).max_envs(1).build();
                let observation = Arc::new(Observation::default());
                let mut features = Features::default();
                features.exceptions(true);
                let mut engine: Engine = EngineBuilder::new(Cranelift::default())
                    .set_features(Some(features))
                    .engine()
                    .into();
                engine.set_tunables(budgeted_tunables(context.budget()));
                let source = if shared {
                    GUEST.to_owned()
                } else {
                    GUEST.replace("(memory 2 2048 shared)", "(memory 2 2048)")
                };
                let module = wasixv2::load(&engine, &wat::parse_str(source).unwrap()).unwrap();
                let machine = Machine::builder()
                    .instantiation_hook(Arc::new(NapiHook::new(context.clone())))
                    .instantiation_hook(Arc::new(ObserverHook(observation.clone())))
                    .build();
                let status = machine
                    .run_module(
                        &engine,
                        &module,
                        ProcessConfig {
                            args: vec!["napi-protected-lease".into()],
                            env: Vec::new(),
                            stdio: Stdio::from_fns(|_| {}, |_| {}),
                        },
                    )
                    .await;
                machine.shutdown().await;
                drop(machine);
                drop(module);
                drop(engine);
                assert_eq!(status.unwrap(), 0, "shared memory: {shared}");
                assert_eq!(observation.configured.load(Ordering::SeqCst), 1);
                assert_eq!(observation.deactivated.load(Ordering::SeqCst), 1);
                assert_eq!(context.active_sessions(), 0);
                let usage = context.budget().snapshot();
                assert_eq!(usage.live_isolates, 0);
                assert_eq!(usage.v8_heap_reserved, 0);
                assert_eq!(usage.v8_external, 0);

                let retained = observation.memory.lock().unwrap().take().unwrap();
                assert_eq!(
                    retained.allocation_owner_count(),
                    1,
                    "only the detached observer may retain the actual backing"
                );
                let committed = retained.host_data().unwrap().1;
                assert!(committed >= 33554432);
                assert_eq!(usage.wasm_linear, committed);
                assert_eq!(usage.mem_charged, committed);
                drop(retained);
                assert_eq!(context.budget().snapshot().wasm_linear, 0);
                assert_eq!(context.budget().snapshot().mem_charged, 0);
            }
        })
        .await;
}
