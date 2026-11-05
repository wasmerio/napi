//! Native bindings run through the real facade and protected kernel allocator.
#![cfg(all(feature = "wasix", not(target_arch = "wasm32")))]

use std::{path::PathBuf, sync::Arc};
use wasixv2::{Machine, Stdio};
use wasmer_napi::{NapiCtx, NapiHook, NapiMemoryAccountant, NativeGuestChunk, NativeGuestRefill};

const GUEST: &str = r#"(module
    (import "env" "memory" (memory 2 2048 shared))
    (import "wasix_32v1" "proc_exit" (func $exit (param i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
        (func $configure (param i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_collect_garbage"
        (func $gc (param i32) (result i32)))
    (import "napi" "napi_wasm_init_env" (func $init (result i32)))
    (import "napi" "napi_create_string_utf8"
        (func $string (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
    (import "napi" "napi_get_value_int32" (func $integer (param i32 i32 i32) (result i32)))
    (table (export "__indirect_function_table") 1 funcref)
    (global (export "__stack_pointer") (mut i32) (i32.const 65536))
    (global (export "__data_end") i32 (i32.const 65536))
    (global (export "__heap_base") i32 (i32.const 65536))
    (data (i32.const 1024) "globalThis.keep = new Uint8Array(33554432); keep[0] = 41; keep[33554431] = 1; keep[0] + keep[33554431]\00")
    (data (i32.const 2048) "keep[0] + keep[33554431]\00")
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
        ;; Larger than the initial 16 MiB fund: actual V8 allocator refill.
        i32.const 1024 call $eval
        global.get $env call $gc call $check
        i32.const 2048 call $eval
        i32.const 0 call $exit unreachable))"#;

#[tokio::test]
async fn facade_native_napi_refills_leases_and_retires_session() {
    const CHILD: &str = "WASIXV2_NAPI_FACADE_CHILD";
    if std::env::var_os(CHILD).is_none() {
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "facade_native_napi_refills_leases_and_retires_session",
                "--nocapture",
            ])
            .env(CHILD, "1")
            .spawn()
            .unwrap();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(45);
        loop {
            if let Some(status) = child.try_wait().unwrap() {
                assert!(status.success(), "native facade child failed: {status}");
                return;
            }
            if std::time::Instant::now() >= deadline {
                let _ = child.kill();
                let _ = child.wait();
                panic!("native facade child did not finish");
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }
    tokio::task::LocalSet::new()
        .run_until(async {
            let context = NapiCtx::builder().max_sessions(1).max_envs(1).build();
            for source in [
                GUEST.to_owned(),
                GUEST.replace("(memory 2 2048 shared)", "(memory 2 2048)"),
            ] {
                let machine = Machine::builder()
                    .instantiation_hook(Arc::new(NapiHook::new(context.clone())))
                    .build();
                let wasm = wat::parse_str(&source).unwrap();
                assert_eq!(
                    machine
                        .run(&wasm, Stdio::from_fns(|_| {}, |_| {}))
                        .await
                        .unwrap(),
                    0
                );
                assert_eq!(context.active_sessions(), 0);
                let budget = context.budget().snapshot();
                assert_eq!(budget.live_isolates, 0);
                assert_eq!(budget.v8_heap_reserved, 0);
                assert_eq!(budget.v8_external, 0);
                assert_eq!(budget.wasm_linear, 0);
                machine.shutdown().await;
            }
        })
        .await;
}

#[tokio::test]
async fn facade_native_default_machine_denies_unaccounted_refill_and_accountant_charge() {
    const CHILD: &str = "WASIXV2_NAPI_LIMITED_REFILL_CHILD";
    if std::env::var_os(CHILD).is_none() {
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "facade_native_default_machine_denies_unaccounted_refill_and_accountant_charge",
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
                    "limited-refill facade child failed: {status}"
                );
                return;
            }
            if std::time::Instant::now() >= deadline {
                let _ = child.kill();
                let _ = child.wait();
                panic!("limited-refill facade child did not finish");
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }

    const MIB: u64 = 1024 * 1024;
    let source = GUEST
        .replace(
            "globalThis.keep = new Uint8Array(33554432); keep[0] = 41; keep[33554431] = 1; keep[0] + keep[33554431]",
            "(() => { try { globalThis.keep = new Uint8Array(33554432); keep[0] = 41; keep[33554431] = 1; return 0; } catch (_) { return 42; } })()",
        )
        .replace("keep[0] + keep[33554431]\\00", "42\\00");
    assert_ne!(
        source, GUEST,
        "the denied-allocation fixture must match its source"
    );
    assert!(source.contains("(data (i32.const 2048) \"42\\00\")"));
    let wasm = wat::parse_str(&source).unwrap();

    tokio::task::LocalSet::new()
        .run_until(async {
            let context = NapiCtx::builder()
                .max_sessions(1)
                .max_envs(1)
                .total_memory_bytes(64 * MIB)
                .build();
            let machine = Machine::builder()
                .instantiation_hook(Arc::new(NapiHook::new(context.clone())))
                .build();
            assert_eq!(
                machine
                    .run(&wasm, Stdio::from_fns(|_| {}, |_| {}))
                    .await
                    .unwrap(),
                0,
                "the guest should catch the denied ArrayBuffer allocation"
            );
            machine.shutdown().await;
            assert_eq!(context.active_sessions(), 0);
            let usage = context.budget().snapshot();
            assert_eq!(usage.live_isolates, 0);
            assert_eq!(usage.wasm_linear, 0);
            assert_eq!(usage.v8_heap_reserved, 0);
            assert_eq!(usage.v8_external, 0);
            assert_eq!(usage.mem_charged, 0);

            struct Accountant {
                limit: u64,
                charged: std::sync::atomic::AtomicU64,
                denied: std::sync::atomic::AtomicUsize,
            }
            impl NapiMemoryAccountant for Accountant {
                fn memory_limit(&self) -> u64 {
                    self.limit
                }
                fn memory_charged(&self) -> u64 {
                    self.charged.load(std::sync::atomic::Ordering::SeqCst)
                }
                fn try_charge(&self, bytes: u64) -> bool {
                    use std::sync::atomic::Ordering;
                    let mut charged = self.charged.load(Ordering::SeqCst);
                    loop {
                        let Some(next) = charged
                            .checked_add(bytes)
                            .filter(|next| *next <= self.limit)
                        else {
                            self.denied.fetch_add(1, Ordering::SeqCst);
                            return false;
                        };
                        match self.charged.compare_exchange_weak(
                            charged,
                            next,
                            Ordering::SeqCst,
                            Ordering::SeqCst,
                        ) {
                            Ok(_) => return true,
                            Err(observed) => charged = observed,
                        }
                    }
                }
                fn uncharge(&self, bytes: u64) {
                    self.charged
                        .fetch_sub(bytes, std::sync::atomic::Ordering::SeqCst);
                }
            }

            let accountant = Arc::new(Accountant {
                limit: 64 * MIB,
                charged: std::sync::atomic::AtomicU64::new(0),
                denied: std::sync::atomic::AtomicUsize::new(0),
            });
            let context = NapiCtx::builder()
                .max_sessions(1)
                .max_envs(1)
                .memory_accountant(accountant.clone())
                .build();
            let machine = Machine::builder()
                .instantiation_hook(Arc::new(NapiHook::new(context.clone())))
                .build();
            assert_eq!(
                machine
                    .run(&wasm, Stdio::from_fns(|_| {}, |_| {}))
                    .await
                    .unwrap(),
                0,
                "the custom accountant should deny the unbacked refill"
            );
            machine.shutdown().await;
            assert!(
                accountant.denied.load(std::sync::atomic::Ordering::SeqCst) > 0,
                "the default Machine's actual refill must consult the custom accountant"
            );
            assert_eq!(accountant.memory_charged(), 0);
            assert_eq!(context.active_sessions(), 0);
            let usage = context.budget().snapshot();
            assert_eq!(usage.live_isolates, 0);
            assert_eq!(usage.wasm_linear, 0);
            assert_eq!(usage.v8_heap_reserved, 0);
            assert_eq!(usage.v8_external, 0);
            assert_eq!(usage.mem_charged, 0);
        })
        .await;
}

const EMPTY_REFILL_GUEST: &str = r#"(module
    (import "env" "memory" (memory 2 2048 shared))
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
    (data (i32.const 1024) "40 + 2\00")
    (global $env (mut i32) (i32.const 0))
    (func $check (param i32) local.get 0 if unreachable end)
    (func (export "run") (result i32)
        i32.const 0 call $configure call $check
        call $init global.set $env
        global.get $env i32.eqz if unreachable end
        global.get $env i32.const 1024 i32.const -1 i32.const 8 call $string call $check
        global.get $env i32.const 8 i32.load i32.const 12 call $script call $check
        global.get $env i32.const 12 i32.load i32.const 16 call $integer call $check
        i32.const 16 i32.load))"#;

#[test]
fn leased_heap_bootstraps_when_refill_is_unavailable() {
    const CHILD: &str = "WASIXV2_NAPI_EMPTY_REFILL_CHILD";
    if std::env::var_os(CHILD).is_none() {
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "leased_heap_bootstraps_when_refill_is_unavailable",
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
                    "no-refill bootstrap child failed: {status}"
                );
                return;
            }
            if std::time::Instant::now() >= deadline {
                let _ = child.kill();
                let _ = child.wait();
                panic!("no-refill bootstrap child did not finish");
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }

    struct NoRefill(std::sync::Arc<std::sync::atomic::AtomicUsize>);
    // SAFETY: this fixture never reserves or owns a Store.
    unsafe impl NativeGuestRefill for NoRefill {
        fn reserve(
            &self,
            _: &mut wasmer::StoreMut<'_>,
            _: u32,
        ) -> Result<Option<NativeGuestChunk>, wasmer::RuntimeError> {
            self.0.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            Ok(None)
        }
    }

    use wasmer::{AsStoreMut, Imports, Instance, Memory, MemoryType, Module, Store};
    for (source, shared) in [
        (EMPTY_REFILL_GUEST.to_owned(), true),
        (
            EMPTY_REFILL_GUEST.replace("(memory 2 2048 shared)", "(memory 2 2048)"),
            false,
        ),
    ] {
        let context = NapiCtx::default();
        let mut store = Store::default();
        store
            .engine_mut()
            .set_tunables(wasmer_napi::budgeted_tunables(context.budget()));
        let module = Module::new(&store, &source).unwrap();
        let memory = Memory::new(&mut store, MemoryType::new(2, Some(2048), shared)).unwrap();
        let initial_pages = memory.size(&store).0;
        let initial_budget = context.budget().snapshot().wasm_linear;
        let mut imports = Imports::new();
        imports.define("env", "memory", memory.clone());
        let hooks = context.runtime_hooks();
        let state = hooks
            .add_imports(&module, &mut store.as_store_mut(), &mut imports)
            .unwrap();
        let session = state.session().unwrap().clone();
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        let arena = context.native_guest_arena(&store, &memory).unwrap();
        assert!(arena.alloc(16, false).is_none());
        let refill_calls = Arc::new(std::sync::atomic::AtomicUsize::new(0));

        session
            .configure_leased_instance(
                &mut store.as_store_mut(),
                &instance,
                &memory,
                arena.clone(),
                Arc::new(NoRefill(refill_calls.clone())),
            )
            .unwrap();
        assert_eq!(
            instance
                .exports
                .get_typed_function::<(), i32>(&store, "run")
                .unwrap()
                .call(&mut store)
                .unwrap(),
            42
        );
        assert!(
            refill_calls.load(std::sync::atomic::Ordering::SeqCst) > 0,
            "provider creation must attempt the advisory lease and tolerate no backing"
        );
        assert!(arena.alloc(16, false).is_none());
        assert_eq!(memory.size(&store).0, initial_pages);
        assert_eq!(context.budget().snapshot().wasm_linear, initial_budget);
        assert_eq!(context.budget().snapshot().live_isolates, 1);

        unsafe { session.deactivate(&mut store.as_store_mut()) };
        drop(instance);
        drop(imports);
        drop(state);
        drop(session);
        drop(arena);
        drop(memory);
        drop(store);
        let usage = context.budget().snapshot();
        assert_eq!(context.active_sessions(), 0);
        assert_eq!(usage.live_isolates, 0);
        assert_eq!(usage.v8_heap_reserved, 0);
        assert_eq!(usage.v8_external, 0);
        assert_eq!(usage.wasm_linear, 0);
    }
}

const THREAD_GUEST: &str = r#"(module
    (import "env" "memory" (memory 2 2048 shared))
    (import "wasix_32v1" "thread_spawn_v2"
        (func $spawn (param i32 i32) (result i32)))
    (import "wasix_32v1" "thread_join" (func $join (param i32) (result i32)))
    (import "wasix_32v1" "proc_exit" (func $exit (param i32)))
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
    (data (i32.const 1024) "40 + 2\00")
    (global $env (mut i32) (i32.const 0))
    (func $check (param i32) local.get 0 if unreachable end)
    (func $eval (param $result_addr i32) (local $local_env i32)
        i32.const 0 call $configure call $check
        call $init local.tee $local_env i32.eqz if unreachable end
        local.get $local_env i32.const 1024 i32.const -1 i32.const 8 call $string call $check
        local.get $local_env i32.const 8 i32.load i32.const 12 call $script call $check
        local.get $local_env i32.const 12 i32.load i32.const 16 call $integer call $check
        i32.const 16 i32.load i32.const 42 i32.ne if unreachable end
        local.get $result_addr i32.const 42 i32.store)
    (func (export "wasi_thread_start") (param i32) (param $start i32)
        local.get $start i32.const 12 i32.add i32.load call $eval)
    (func (export "_start") (local $tid i32)
        ;; The main and thread attachments each initialize their own N-API env.
        i32.const 200 call $eval
        i32.const 100 i32.const 131072 i32.store
        i32.const 104 i32.const 0 i32.store
        i32.const 108 i32.const 0 i32.store
        i32.const 112 i32.const 204 i32.store
        i32.const 156 i32.const 65536 i32.store
        i32.const 160 i32.const 0 i32.store
        i32.const 100 i32.const 164 call $spawn call $check
        i32.const 164 i32.load local.tee $tid call $join call $check
        i32.const 200 i32.load i32.const 42 i32.ne if unreachable end
        i32.const 204 i32.load i32.const 42 i32.ne if unreachable end
        i32.const 0 call $exit))"#;

const EXEC_PARENT_GUEST: &str = r#"(module
    (import "env" "memory" (memory 2 2048 shared))
    (import "wasix_32v1" "proc_fork_env" (func $vfork (param i32) (result i32)))
    (import "wasix_32v1" "proc_exec3"
        (func $exec (param i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
    (import "wasix_32v1" "proc_join" (func $join (param i32 i32 i32) (result i32)))
    (import "wasix_32v1" "proc_exit2" (func $exit (param i32)))
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
    (data (i32.const 1024) "6 * 7\00")
    (data (i32.const 2000) "/prog/child.wasm")
    (data (i32.const 2032) "child\n")
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
        ;; The vfork child replaces its image; the parent resumes with its N-API env.
        i32.const 100 call $vfork call $check
        i32.const 2000 i32.const 16 i32.const 2032 i32.const 6
        i32.const 0 i32.const 0 i32.const 0 i32.const 0 i32.const 0
        call $exec call $check
        i32.const 1024 call $eval
        ;; Match the existing vfork/exec fixture's child status protocol.
        i32.const 200 i32.const 1 i32.store8
        i32.const 204 i32.const 100 i32.load i32.store
        i32.const 200 i32.const 0 i32.const 220 call $join call $check
        i32.const 222 i32.load16_u i32.const 3 i32.ne if unreachable end
        i32.const 0 call $exit unreachable))"#;

const EXEC_CHILD_GUEST: &str = r#"(module
    (import "env" "memory" (memory 2 2048 shared))
    (import "wasix_32v1" "proc_exit2" (func $exit (param i32)))
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
    (data (i32.const 1024) "40 + 2\00")
    (global $env (mut i32) (i32.const 0))
    (func $check (param i32) local.get 0 if unreachable end)
    (func (export "_start")
        i32.const 0 call $configure call $check
        call $init global.set $env
        global.get $env i32.eqz if unreachable end
        global.get $env i32.const 1024 i32.const -1 i32.const 8 call $string call $check
        global.get $env i32.const 8 i32.load i32.const 12 call $script call $check
        global.get $env i32.const 12 i32.load i32.const 16 call $integer call $check
        i32.const 16 i32.load i32.const 42 i32.ne if unreachable end
        i32.const 0 call $exit unreachable))"#;

struct ExecPrograms(PathBuf);

impl ExecPrograms {
    fn new(child: &[u8]) -> Self {
        let path = std::env::temp_dir().join(format!("wasixv2-napi-exec-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        let child_path = path.join("child.wasm");
        std::fs::write(&child_path, child).unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&child_path, std::fs::Permissions::from_mode(0o755)).unwrap();
        }
        Self(path)
    }
}

impl Drop for ExecPrograms {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

#[tokio::test]
async fn facade_native_vfork_exec_creates_a_new_napi_session() {
    const CHILD: &str = "WASIXV2_NAPI_EXEC_CHILD";
    if std::env::var_os(CHILD).is_none() {
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "facade_native_vfork_exec_creates_a_new_napi_session",
                "--nocapture",
            ])
            .env(CHILD, "1")
            .spawn()
            .unwrap();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(45);
        loop {
            if let Some(status) = child.try_wait().unwrap() {
                assert!(status.success(), "vfork/exec facade child failed: {status}");
                return;
            }
            if std::time::Instant::now() >= deadline {
                let _ = child.kill();
                let _ = child.wait();
                panic!("vfork/exec facade child did not finish");
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }

    tokio::task::LocalSet::new()
        .run_until(async {
            let programs = ExecPrograms::new(&wat::parse_str(EXEC_CHILD_GUEST).unwrap());
            let context = NapiCtx::builder().max_sessions(2).max_envs(2).build();
            let machine = Machine::builder()
                .instantiation_hook(Arc::new(NapiHook::new(context.clone())))
                .build();
            machine
                .mount_host_dir("/prog", &programs.0, false)
                .await
                .unwrap();
            let main = wat::parse_str(EXEC_PARENT_GUEST).unwrap();
            assert_eq!(
                machine
                    .run_process(
                        &main,
                        wasixv2::ProcessConfig {
                            args: vec!["napi-vfork-exec".into()],
                            env: Vec::new(),
                            stdio: Stdio::from_fns(|_| {}, |_| {}),
                        },
                    )
                    .await
                    .unwrap(),
                0
            );
            assert_eq!(context.active_sessions(), 0);
            let budget = context.budget().snapshot();
            assert_eq!(budget.live_isolates, 0);
            assert_eq!(budget.v8_heap_reserved, 0);
            assert_eq!(budget.v8_external, 0);
            assert_eq!(budget.wasm_linear, 0);
            machine.shutdown().await;
        })
        .await;
}

#[tokio::test]
async fn facade_native_shared_thread_gets_its_own_napi_session() {
    const CHILD: &str = "WASIXV2_NAPI_THREAD_CHILD";
    if std::env::var_os(CHILD).is_none() {
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "facade_native_shared_thread_gets_its_own_napi_session",
                "--nocapture",
            ])
            .env(CHILD, "1")
            .spawn()
            .unwrap();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(45);
        loop {
            if let Some(status) = child.try_wait().unwrap() {
                assert!(status.success(), "thread facade child failed: {status}");
                return;
            }
            if std::time::Instant::now() >= deadline {
                let _ = child.kill();
                let _ = child.wait();
                panic!("thread facade child did not finish");
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }

    tokio::task::LocalSet::new()
        .run_until(async {
            let context = NapiCtx::builder().max_sessions(2).max_envs(2).build();
            let machine = Machine::builder()
                .instantiation_hook(Arc::new(NapiHook::new(context.clone())))
                .build();
            let wasm = wat::parse_str(THREAD_GUEST).unwrap();
            assert_eq!(
                machine
                    .run(&wasm, Stdio::from_fns(|_| {}, |_| {}))
                    .await
                    .unwrap(),
                0
            );
            assert_eq!(context.active_sessions(), 0);
            let budget = context.budget().snapshot();
            assert_eq!(budget.live_isolates, 0);
            assert_eq!(budget.v8_heap_reserved, 0);
            assert_eq!(budget.v8_external, 0);
            assert_eq!(budget.wasm_linear, 0);
            machine.shutdown().await;
        })
        .await;
}

fn leb(mut value: u32, out: &mut Vec<u8>) {
    loop {
        let byte = (value & 127) as u8;
        value >>= 7;
        out.push(byte | if value == 0 { 0 } else { 128 });
        if value == 0 {
            return;
        }
    }
}

fn pic(wat: &str, needed: &[&str]) -> Vec<u8> {
    let mut wasm = wat::parse_str(wat).unwrap();
    let mut section = vec![8];
    section.extend_from_slice(b"dylink.0");
    let mut info = Vec::new();
    for value in [512, 0, 0, 0] {
        leb(value, &mut info);
    }
    section.push(1);
    leb(info.len() as u32, &mut section);
    section.extend(info);
    if !needed.is_empty() {
        let mut names = Vec::new();
        leb(needed.len() as u32, &mut names);
        for name in needed {
            leb(name.len() as u32, &mut names);
            names.extend_from_slice(name.as_bytes());
        }
        section.push(2);
        leb(names.len() as u32, &mut section);
        section.extend(names);
    }
    wasm.push(0);
    leb(section.len() as u32, &mut wasm);
    wasm.extend(section);
    wasm
}

const DSO_MAIN: &str = r#"(module
    (import "env" "memory" (memory 1 65536 shared))
    (import "env" "__indirect_function_table" (table 3 funcref))
    (import "env" "__stack_pointer" (global (mut i32)))
    (import "env" "__memory_base" (global $base i32))
    (import "env" "__table_base" (global i32))
    (import "wasix_32v1" "dlopen"
        (func $open (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
    (import "wasix_32v1" "dlsym"
        (func $sym (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "wasix_32v1" "call_dynamic"
        (func $invoke (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "wasix_32v1" "proc_exit" (func $exit (param i32)))
    (export "__indirect_function_table" (table 0))
    (data (global.get $base) "/prog/side.so\00run_napi\00")
    (func $at (param i32) (result i32)
        global.get $base local.get 0 i32.add)
    (func $check (param i32 i32 i32)
        local.get 0 local.get 1 i32.ne if local.get 2 call $exit end)
    (func $load (result i32)
        i32.const 0 call $at i32.const 13 i32.const -1
        i32.const 256 call $at i32.const 64 i32.const 53 call $at
        i32.const 5 i32.const 192 call $at call $open
        i32.const 0 i32.const 1 call $check
        i32.const 192 call $at i32.load)
    (func $lookup (param $handle i32) (result i32)
        local.get $handle i32.const 14 call $at i32.const 8
        i32.const 256 call $at i32.const 64 i32.const 196 call $at
        call $sym i32.const 0 i32.const 2 call $check
        i32.const 196 call $at i32.load)
    (func (export "_start") (local $function i32)
        call $load call $lookup local.set $function
        local.get $function i32.const 0 i32.const 0 i32.const 256 call $at
        i32.const 4 i32.const 0 call $invoke
        i32.const 0 i32.const 3 call $check
        i32.const 256 call $at i32.load i32.const 42 i32.const 4 call $check
        i32.const 0 call $exit))"#;

const DSO_SIDE: &str = r#"(module
    (import "env" "memory" (memory 1 65536 shared))
    (import "env" "__indirect_function_table" (table 0 funcref))
    (import "env" "__memory_base" (global $base i32))
    (import "env" "__table_base" (global i32))
    (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
        (func $configure (param i32) (result i32)))
    (import "napi" "napi_wasm_init_env" (func $init (result i32)))
    (import "napi" "napi_create_string_utf8"
        (func $string (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
    (import "napi" "napi_get_value_int32" (func $integer (param i32 i32 i32) (result i32)))
    (data (global.get $base) "40 + 2\00")
    (func $check (param i32) local.get 0 if unreachable end)
    (func (export "run_napi") (result i32) (local $env i32)
        i32.const 0 call $configure call $check
        call $init local.tee $env i32.eqz if unreachable end
        local.get $env global.get $base i32.const -1
        global.get $base i32.const 8 i32.add call $string call $check
        local.get $env global.get $base i32.const 8 i32.add i32.load
        global.get $base i32.const 12 i32.add call $script call $check
        local.get $env global.get $base i32.const 12 i32.add i32.load
        global.get $base i32.const 16 i32.add call $integer call $check
        global.get $base i32.const 16 i32.add i32.load))"#;

struct DsoPrograms(PathBuf);

impl DsoPrograms {
    fn new(main: &[u8], side: &[u8]) -> Self {
        let path = std::env::temp_dir().join(format!("wasixv2-napi-dso-{}", std::process::id()));
        std::fs::create_dir_all(&path).unwrap();
        std::fs::write(path.join("main.wasm"), main).unwrap();
        std::fs::write(path.join("side.so"), side).unwrap();
        Self(path)
    }
}

impl Drop for DsoPrograms {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

#[tokio::test]
async fn facade_native_dso_import_gets_its_own_napi_session() {
    const CHILD: &str = "WASIXV2_NAPI_DSO_CHILD";
    if std::env::var_os(CHILD).is_none() {
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "facade_native_dso_import_gets_its_own_napi_session",
                "--nocapture",
            ])
            .env(CHILD, "1")
            .spawn()
            .unwrap();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(45);
        loop {
            if let Some(status) = child.try_wait().unwrap() {
                assert!(status.success(), "DSO facade child failed: {status}");
                return;
            }
            if std::time::Instant::now() >= deadline {
                let _ = child.kill();
                let _ = child.wait();
                panic!("DSO facade child did not finish");
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }

    tokio::task::LocalSet::new()
        .run_until(async {
            let main = pic(DSO_MAIN, &["side.so"]);
            let programs = DsoPrograms::new(&main, &pic(DSO_SIDE, &[]));
            let context = NapiCtx::builder().max_sessions(1).max_envs(1).build();
            let machine = Machine::builder()
                .instantiation_hook(Arc::new(NapiHook::new(context.clone())))
                .build();
            machine
                .mount_host_dir("/prog", &programs.0, false)
                .await
                .unwrap();
            let status = machine
                .run_process(
                    &main,
                    wasixv2::ProcessConfig {
                        args: vec!["napi-dso".into()],
                        env: vec!["LD_LIBRARY_PATH=/prog".into()],
                        stdio: Stdio::from_fns(|_| {}, |_| {}),
                    },
                )
                .await
                .unwrap();
            assert_eq!(status, 0);
            assert_eq!(context.active_sessions(), 0);
            let budget = context.budget().snapshot();
            assert_eq!(budget.live_isolates, 0);
            assert_eq!(budget.v8_heap_reserved, 0);
            assert_eq!(budget.v8_external, 0);
            assert_eq!(budget.wasm_linear, 0);
            machine.shutdown().await;
        })
        .await;
}
