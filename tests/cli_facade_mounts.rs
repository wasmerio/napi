//! The public N-API runner keeps v2 root overlays and writable host mounts.
#![cfg(all(feature = "cli", not(target_arch = "wasm32")))]

use std::path::{Path, PathBuf};
use wasmer_napi::{
    NapiCtx, NapiMemoryAccountant,
    cli::{GuestMount, run_wasix_main_capture_stdio_with_ctx},
};

const MIB: u64 = 1024 * 1024;

const ROOT_LAYER_GUEST: &str = r#"(module
    (import "env" "memory" (memory 2 2048 shared))
    (import "wasix_32v1" "path_open"
        (func $open (param i32 i32 i32 i32 i32 i64 i64 i32 i32) (result i32)))
    (import "wasix_32v1" "fd_read"
        (func $read (param i32 i32 i32 i32) (result i32)))
    (import "wasix_32v1" "fd_write"
        (func $write (param i32 i32 i32 i32) (result i32)))
    (import "wasix_32v1" "fd_seek"
        (func $seek (param i32 i64 i32 i32) (result i32)))
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
    (data (i32.const 2048) "value.txt")
    (data (i32.const 2064) "mutate")
    (global $env (mut i32) (i32.const 0))
    (func $check (param i32) local.get 0 if unreachable end)
    (func (export "_start") (local $fd i32)
        i32.const 0 call $configure call $check
        call $init global.set $env
        global.get $env i32.eqz if unreachable end
        global.get $env i32.const 1024 i32.const -1 i32.const 8 call $string call $check
        global.get $env i32.const 8 i32.load i32.const 12 call $script call $check
        global.get $env i32.const 12 i32.load i32.const 16 call $integer call $check
        i32.const 16 i32.load i32.const 42 i32.ne if unreachable end
        ;; The first root layer contains `value.txt`; read it, then mutate its COW view.
        i32.const 3 i32.const 1 i32.const 2048 i32.const 9
        i32.const 0 i64.const 70 i64.const 0 i32.const 0 i32.const 32
        call $open call $check
        i32.const 32 i32.load local.set $fd
        i32.const 0 i32.const 4096 i32.store
        i32.const 4 i32.const 6 i32.store
        local.get $fd i32.const 0 i32.const 1 i32.const 36 call $read call $check
        i32.const 1 i32.const 0 i32.const 1 i32.const 40 call $write call $check
        local.get $fd i64.const 0 i32.const 0 i32.const 44 call $seek call $check
        i32.const 0 i32.const 2064 i32.store
        i32.const 4 i32.const 6 i32.store
        local.get $fd i32.const 0 i32.const 1 i32.const 48 call $write call $check
        local.get $fd i64.const 0 i32.const 0 i32.const 44 call $seek call $check
        i32.const 0 i32.const 4096 i32.store
        i32.const 4 i32.const 6 i32.store
        local.get $fd i32.const 0 i32.const 1 i32.const 36 call $read call $check
        i32.const 1 i32.const 0 i32.const 1 i32.const 40 call $write call $check
        i32.const 0 call $exit))"#;

const WRITABLE_MOUNT_GUEST: &str = r#"(module
    (import "env" "memory" (memory 2 2048 shared))
    (import "wasix_32v1" "path_open"
        (func $open (param i32 i32 i32 i32 i32 i64 i64 i32 i32) (result i32)))
    (import "wasix_32v1" "fd_write"
        (func $write (param i32 i32 i32 i32) (result i32)))
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
    (data (i32.const 2048) "data/entry.txt")
    (data (i32.const 2064) "mounted")
    (global $env (mut i32) (i32.const 0))
    (func $check (param i32) local.get 0 if unreachable end)
    (func (export "_start") (local $fd i32)
        i32.const 0 call $configure call $check
        call $init global.set $env
        global.get $env i32.eqz if unreachable end
        global.get $env i32.const 1024 i32.const -1 i32.const 8 call $string call $check
        global.get $env i32.const 8 i32.load i32.const 12 call $script call $check
        global.get $env i32.const 12 i32.load i32.const 16 call $integer call $check
        i32.const 16 i32.load i32.const 42 i32.ne if unreachable end
        ;; Create and write through the writable /data host mount.
        i32.const 3 i32.const 1 i32.const 2048 i32.const 14
        i32.const 9 i64.const 66 i64.const 0 i32.const 0 i32.const 32
        call $open call $check
        i32.const 32 i32.load local.set $fd
        i32.const 0 i32.const 2064 i32.store
        i32.const 4 i32.const 7 i32.store
        local.get $fd i32.const 0 i32.const 1 i32.const 36 call $write call $check
        i32.const 0 call $exit))"#;

struct TempDir(PathBuf);

impl TempDir {
    fn new(name: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let path = std::env::temp_dir().join(format!(
            "wasmer-napi-cli-{name}-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&path).unwrap();
        Self(path)
    }

    fn guest(&self, name: &str, wat: &str) -> PathBuf {
        let path = self.0.join(name);
        std::fs::write(&path, wat::parse_str(wat).unwrap()).unwrap();
        path
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn assert_released(context: &NapiCtx) {
    assert_eq!(context.active_sessions(), 0);
    let budget = context.budget().snapshot();
    assert_eq!(budget.live_isolates, 0);
    assert_eq!(budget.v8_heap_reserved, 0);
    assert_eq!(budget.v8_external, 0);
    assert_eq!(budget.wasm_linear, 0);
}

fn run_in_watched_child(test_name: &str, child_key: &str) -> bool {
    if std::env::var_os(child_key).is_some() {
        return false;
    }

    let mut child = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["--exact", test_name, "--nocapture"])
        .env(child_key, "1")
        .spawn()
        .unwrap();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(45);
    loop {
        if let Some(status) = child.try_wait().unwrap() {
            assert!(status.success(), "CLI facade child failed: {status}");
            return true;
        }
        if std::time::Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            panic!("CLI facade child did not finish: {test_name}");
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
}

const REFILL_GUEST: &str = r#"(module
    (import "env" "memory" (memory 2 2048 shared))
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
    (data (i32.const 1024) "globalThis.keep = new Uint8Array(33554432); keep[0] = 41; keep[33554431] = 1; keep[0] + keep[33554431]\00")
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

#[test]
fn public_cli_runner_charges_a_refilled_arraybuffer_once() {
    if run_in_watched_child(
        "public_cli_runner_charges_a_refilled_arraybuffer_once",
        "WASIXV2_NAPI_CLI_REFILL_CHILD",
    ) {
        return;
    }

    struct Accountant {
        current: std::sync::atomic::AtomicU64,
        peak: std::sync::atomic::AtomicU64,
    }

    impl NapiMemoryAccountant for Accountant {
        fn memory_limit(&self) -> u64 {
            192 * MIB
        }

        fn memory_charged(&self) -> u64 {
            self.current.load(std::sync::atomic::Ordering::SeqCst)
        }

        fn try_charge(&self, bytes: u64) -> bool {
            use std::sync::atomic::Ordering::SeqCst;

            let mut current = self.current.load(SeqCst);
            loop {
                let Some(next) = current.checked_add(bytes) else {
                    return false;
                };
                if next > self.memory_limit() {
                    return false;
                }
                match self
                    .current
                    .compare_exchange_weak(current, next, SeqCst, SeqCst)
                {
                    Ok(_) => {
                        self.peak.fetch_max(next, SeqCst);
                        return true;
                    }
                    Err(observed) => current = observed,
                }
            }
        }

        fn uncharge(&self, bytes: u64) {
            self.current
                .fetch_sub(bytes, std::sync::atomic::Ordering::SeqCst);
        }
    }

    let temp = TempDir::new("budgeted-refill");
    let guest = temp.guest("refill.wasm", REFILL_GUEST);
    // The imported linear-memory mapping includes its 1/8 address-space gap,
    // so the actual combined reservation is about 158 MiB. Charging the
    // leased backing a second time would push it above this 192 MiB cap.
    let accountant = std::sync::Arc::new(Accountant {
        current: std::sync::atomic::AtomicU64::new(0),
        peak: std::sync::atomic::AtomicU64::new(0),
    });
    let context = NapiCtx::builder()
        .max_sessions(1)
        .max_envs(1)
        .memory_accountant(accountant.clone())
        .build();
    let (status, stdout, stderr) =
        run_wasix_main_capture_stdio_with_ctx(&context, &guest, &[], &[]).unwrap();
    assert_eq!(status, 0, "refill guest failed: {stderr}");
    assert!(stdout.is_empty());
    let peak = accountant.peak.load(std::sync::atomic::Ordering::SeqCst);
    assert!(
        peak > 128 * MIB,
        "unexpectedly low observed peak: {peak} bytes"
    );
    assert!(
        peak <= 192 * MIB,
        "unexpectedly high observed peak: {peak} bytes"
    );
    assert_eq!(accountant.memory_charged(), 0);
    assert_released(&context);
}

#[test]
fn public_runner_preserves_root_layer_precedence_cow_and_writable_mounts() {
    if run_in_watched_child(
        "public_runner_preserves_root_layer_precedence_cow_and_writable_mounts",
        "WASIXV2_NAPI_CLI_MOUNTS_CHILD",
    ) {
        return;
    }

    let temp = TempDir::new("mounts");
    let first_root = temp.0.join("first-root");
    let second_root = temp.0.join("second-root");
    let writable = temp.0.join("writable");
    std::fs::create_dir_all(&first_root).unwrap();
    std::fs::create_dir_all(&second_root).unwrap();
    std::fs::create_dir_all(&writable).unwrap();
    std::fs::write(first_root.join("value.txt"), b"first!").unwrap();
    std::fs::write(second_root.join("value.txt"), b"second").unwrap();

    let root_guest = temp.guest("root-layers.wasm", ROOT_LAYER_GUEST);
    let writable_guest = temp.guest("writable-mount.wasm", WRITABLE_MOUNT_GUEST);
    let context = NapiCtx::builder().max_sessions(1).max_envs(1).build();

    let (status, stdout, stderr) = run_wasix_main_capture_stdio_with_ctx(
        &context,
        &root_guest,
        &[],
        &[
            GuestMount {
                host_path: first_root.clone(),
                guest_path: PathBuf::from("/"),
            },
            GuestMount {
                host_path: second_root.clone(),
                guest_path: PathBuf::from("/"),
            },
        ],
    )
    .unwrap();
    assert_eq!(status, 0, "root layer guest failed: {stderr}");
    assert_eq!(stdout, "first!mutate");
    assert_eq!(
        std::fs::read(first_root.join("value.txt")).unwrap(),
        b"first!"
    );
    assert_eq!(
        std::fs::read(second_root.join("value.txt")).unwrap(),
        b"second"
    );
    assert_released(&context);

    let (status, stdout, stderr) = run_wasix_main_capture_stdio_with_ctx(
        &context,
        &writable_guest,
        &[],
        &[GuestMount {
            host_path: writable.clone(),
            guest_path: Path::new("/data").to_owned(),
        }],
    )
    .unwrap();
    assert_eq!(status, 0, "writable mount guest failed: {stderr}");
    assert!(stdout.is_empty());
    assert_eq!(
        std::fs::read(writable.join("entry.txt")).unwrap(),
        b"mounted"
    );
    assert_released(&context);
}
