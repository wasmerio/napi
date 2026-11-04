#![cfg(not(target_arch = "wasm32"))]

//! Real native bridge callbacks, without the legacy WASIX runtime adapter.

use std::{
    error::Error,
    fmt,
    process::{Child, Command},
    sync::{Arc, Mutex},
    thread,
    time::{Duration, Instant},
};

use wasmer::{AsStoreMut, Extern, Function, Instance, Memory, Module, RuntimeError, Store, Value};
use wasmer_napi::{NapiCtx, NapiSession};

const GUEST: &str = r#"(module
    (import "env" "memory" (memory 1 1024 shared))
    (import "test" "record" (func $record (param i32)))
    (import "napi" "napi_wasm_init_env" (func $new_env (result i32)))
    (import "napi" "napi_adjust_external_memory" (func $adjust_external (param i32 i64 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_acquire_buffer_lease"
        (func $acquire_lease (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_add_finalizer" (func $add_finalizer (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_get_global" (func $global (param i32 i32) (result i32)))
    (import "napi" "napi_create_function" (func $function (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_set_named_property" (func $set (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_run_script" (func $run (param i32 i32 i32) (result i32)))
    (import "napi" "napi_get_value_int32" (func $integer (param i32 i32 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_contextify_run_script"
        (func $contextify (param i32 i32 i32 i32 i32 i32 i64 i32 i32 i32 i32 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
        (func $configure_runtime (param i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
        (func $create_env (param i32 i32 i32 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
        (func $release_env (param i32 i32) (result i32)))
    (import "napi" "napi_create_external_arraybuffer" (func $external_array (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_create_external_buffer" (func $external_buffer (param i32 i32 i32 i32 i32 i32) (result i32)))
    (table (export "__indirect_function_table") 5 funcref)
    (elem (i32.const 1) $first $second $release_callback $finalizer)
    (data (i32.const 64) "callback\00")
    (global $env (mut i32) (i32.const 0))
    (global $release_status (mut i32) (i32.const -1))
    (func $check (param i32) local.get 0 if unreachable end)
    (func $first (export "first") (param i32 i32) (result i32)
        i32.const 1 call $record i32.const 0)
    (func $second (export "second") (param i32 i32) (result i32)
        i32.const 2 call $record i32.const 0)
    (func $release_callback (export "release_callback") (param i32 i32) (result i32)
        i32.const 36 i32.load i32.const 0 call $release_env global.set $release_status
        i32.const 0)
    (global $finalizer_len (mut i32) (i32.const 0))
    (global $finalizer_resources (mut i32) (i32.const 0))
    (global $finalizer_status (mut i32) (i32.const -1))
    (func (export "arm_finalizer") (param $len i32) (param $resources i32)
        local.get $len global.set $finalizer_len
        local.get $resources global.set $finalizer_resources
        global.get $env i32.const 8 i32.load i32.const 0 i32.const 4 i32.const 0 i32.const 0
        call $add_finalizer call $check)
    (func $finalizer (param i32 i32 i32)
        i32.const 3 call $record
        global.get $finalizer_resources if
            global.get $env i64.const -32 i32.const 40 call $adjust_external call $check
            global.get $env i64.const 64 i32.const 40 call $adjust_external call $check
        end
        i32.const 1024 global.get $finalizer_len call $eval_status global.set $finalizer_status
        global.get $finalizer_resources if
            global.get $env i32.const 20 i32.load i32.const 0 i32.const 16 i32.const 1 i32.const 56 i32.const 60
            call $acquire_lease call $check
            i32.const 4 call $record
        end)
    (func (export "finalizer_status") (result i32) global.get $finalizer_status)
    (func (export "release_status") (result i32) global.get $release_status)
    (func (export "release_owned") (result i32)
        i32.const 36 i32.load i32.const 0 call $release_env)
    (func (export "configure_runtime") (result i32) i32.const 0 call $configure_runtime)
    (func (export "configure_runtime_options") (param i32) (result i32) local.get 0 call $configure_runtime)
    (func (export "init_id") (result i32) call $new_env)
    (func (export "create_status") (param i32 i32) (result i32)
        i32.const 8 i32.const 0 local.get 0 local.get 1 call $create_env)
    (func (export "init")
        call $new_env global.set $env
        global.get $env i32.eqz if unreachable end
        global.get $env i32.const 8 call $global call $check
        global.get $env i32.const 64 i32.const 8 i32.const 1 i32.const 0 i32.const 12
        call $function call $check
        global.get $env i32.const 8 i32.load i32.const 64 i32.const 12 i32.load
        call $set call $check)
    (func (export "init_owned")
        i32.const 8 i32.const 0 i32.const 32 i32.const 36 call $create_env call $check
        i32.const 32 i32.load global.set $env
        global.get $env i32.const 8 call $global call $check
        global.get $env i32.const 64 i32.const 8 i32.const 1 i32.const 0 i32.const 12
        call $function call $check
        global.get $env i32.const 8 i32.load i32.const 64 i32.const 12 i32.load
        call $set call $check)
    (func (export "eval") (param i32 i32) (result i32)
        global.get $env local.get 0 local.get 1 i32.const 16 call $string call $check
        global.get $env i32.const 16 i32.load i32.const 20 call $run call $check
        global.get $env i32.const 20 i32.load i32.const 24 call $integer call $check
        i32.const 24 i32.load)
    (func $eval_status (export "eval_status") (param i32 i32) (result i32)
        global.get $env local.get 0 local.get 1 i32.const 16 call $string call $check
        global.get $env i32.const 16 i32.load i32.const 20 call $run)
    (func (export "external_array") (param i32 i32) (result i32)
        global.get $env local.get 0 local.get 1 i32.const 0 i32.const 0 i32.const 48 call $external_array)
    (func (export "external_buffer") (param i32 i32) (result i32)
        global.get $env local.get 1 local.get 0 i32.const 0 i32.const 0 i32.const 48 call $external_buffer)
    (func (export "fresh") (result i32) i32.const 9)
    (func (export "global_status") (result i32)
        global.get $env i32.const 48 call $global)
    (func (export "adjust_external") (param i64) (result i32)
        global.get $env local.get 0 i32.const 40 call $adjust_external)
    (func (export "eval_contextify") (param i32 i32) (result i32)
        global.get $env local.get 0 local.get 1 i32.const 16 call $string call $check
        i32.const 128 i32.const 0 i32.store
        i32.const 132 i32.const 16 i32.load i32.store
        i32.const 136 i32.const 0 i32.store
        global.get $env i32.const 0 i32.const 128 i32.const 16 i32.load
        i32.const 0 i32.const 0 i64.const -1 i32.const 1 i32.const 0 i32.const 0
        i32.const 0 i32.const 20 call $contextify call $check
        global.get $env i32.const 20 i32.load i32.const 24 call $integer call $check
        i32.const 24 i32.load)
)"#;

fn instance(
    store: &mut Store,
    record: impl Fn(i32) -> Result<(), RuntimeError> + Send + Sync + 'static,
) -> (NapiCtx, Instance, Memory, NapiSession) {
    instance_with_record(store, |store, _| Function::new_typed(store, record))
}

fn instance_with_record(
    store: &mut Store,
    record: impl FnOnce(&mut Store, &NapiCtx) -> Function,
) -> (NapiCtx, Instance, Memory, NapiSession) {
    instance_with_context(store, NapiCtx::default(), record)
}

fn instance_with_context(
    store: &mut Store,
    ctx: NapiCtx,
    record: impl FnOnce(&mut Store, &NapiCtx) -> Function,
) -> (NapiCtx, Instance, Memory, NapiSession) {
    let result = configured_instance_with_context(store, ctx, record);
    result
        .1
        .exports
        .get_typed_function::<(), ()>(store, "init")
        .unwrap()
        .call(store)
        .unwrap();
    result
}

fn configured_instance_with_context(
    store: &mut Store,
    ctx: NapiCtx,
    record: impl FnOnce(&mut Store, &NapiCtx) -> Function,
) -> (NapiCtx, Instance, Memory, NapiSession) {
    let module = Module::new(&*store, wat::parse_str(GUEST).unwrap()).unwrap();
    let hooks = ctx.runtime_hooks();
    let (mut imports, state) = hooks
        .additional_imports(&module, &mut store.as_store_mut())
        .unwrap();
    let session = state.session().unwrap().clone();
    imports.define("test", "record", record(store, &ctx));
    let Some(Extern::Memory(memory)) = imports.get_export("env", "memory") else {
        panic!("NAPI did not create the imported memory");
    };
    let instance = Instance::new(store, &module, &imports).unwrap();
    hooks
        .configure_instance(
            &module,
            &mut store.as_store_mut(),
            &instance,
            Some(&memory),
            state,
        )
        .unwrap();
    (ctx, instance, memory, session)
}

fn eval(
    store: &mut impl AsStoreMut,
    instance: &Instance,
    memory: &Memory,
    script: &str,
) -> Result<i32, RuntimeError> {
    memory.view(store).write(1024, script.as_bytes()).unwrap();
    instance
        .exports
        .get_typed_function::<(i32, i32), i32>(store, "eval")
        .unwrap()
        .call(store, 1024, script.len() as i32)
}

#[derive(Debug)]
struct Original(Arc<()>);
impl fmt::Display for Original {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("original callback failure")
    }
}
impl Error for Original {}

fn run_in_subprocess(test_name: &str, timeout: Duration) -> bool {
    const CHILD: &str = "NAPI_CALLBACK_CONTROL_CHILD";
    if std::env::var_os(CHILD).is_none() {
        struct OwnedChild(Child);
        impl Drop for OwnedChild {
            fn drop(&mut self) {
                let _ = self.0.kill();
                let _ = self.0.wait();
            }
        }
        let mut child = OwnedChild(
            Command::new(std::env::current_exe().unwrap())
                .args(["--exact", test_name, "--nocapture"])
                .env(CHILD, "1")
                .spawn()
                .unwrap(),
        );
        let started = Instant::now();
        loop {
            if let Some(status) = child.0.try_wait().unwrap() {
                assert!(status.success(), "callback child failed: {status}");
                return false;
            }
            assert!(
                started.elapsed() < timeout,
                "callback error failed to stop JavaScript execution"
            );
            thread::sleep(Duration::from_millis(10));
        }
    }

    true
}

#[test]
fn callback_error_stops_js_and_preserves_original_payload() {
    if !run_in_subprocess(
        "callback_error_stops_js_and_preserves_original_payload",
        Duration::from_secs(15),
    ) {
        return;
    }

    let mut store = Store::default();
    let payload = Arc::new(());
    let captured = payload.clone();
    let (_ctx, instance, memory, _session) = instance(&mut store, move |_| {
        Err(RuntimeError::user(Box::new(Original(captured.clone()))))
    });
    for _ in 0..2 {
        let error = eval(
            &mut store,
            &instance,
            &memory,
            "try { callback(); } catch (e) {} while (true) {}",
        )
        .unwrap_err();
        assert!(Arc::ptr_eq(
            &error.downcast_ref::<Original>().unwrap().0,
            &payload
        ));
        // Only this bridge invocation was stopped; the isolate stays usable.
        assert_eq!(eval(&mut store, &instance, &memory, "40 + 2").unwrap(), 42);
    }
}

#[test]
fn callback_uses_replaced_table_target() {
    let mut store = Store::default();
    let seen = Arc::new(Mutex::new(Vec::new()));
    let captured = seen.clone();
    let (_ctx, instance, memory, _session) = instance(&mut store, move |which| {
        captured.lock().unwrap().push(which);
        Ok(())
    });
    assert_eq!(
        eval(&mut store, &instance, &memory, "callback(); 42").unwrap(),
        42
    );
    let replacement = instance.exports.get_function("second").unwrap().clone();
    instance
        .exports
        .get_table("__indirect_function_table")
        .unwrap()
        .set(&mut store, 1, Value::FuncRef(Some(replacement)))
        .unwrap();
    assert_eq!(
        eval(&mut store, &instance, &memory, "callback(); 42").unwrap(),
        42
    );
    assert_eq!(*seen.lock().unwrap(), vec![1, 2]);
}

#[test]
fn explicit_deactivation_releases_provider_roots_before_store_drop() {
    let mut store = Store::default();
    let (ctx, instance, memory, session) = instance(&mut store, |_| Ok(()));
    assert_eq!(ctx.active_sessions(), 1);
    assert_eq!(ctx.budget().snapshot().live_isolates, 1);
    assert!(ctx.budget().snapshot().wasm_linear > 0);
    assert_eq!(
        eval(
            &mut store,
            &instance,
            &memory,
            "globalThis.buffer = new Uint8Array(65536); 42",
        )
        .unwrap(),
        42
    );
    // SAFETY: all synchronous calls returned on this thread; this test owns
    // the sole session and has no pending callbacks or Promise continuations.
    unsafe {
        session.deactivate(&mut store.as_store_mut());
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
    assert_eq!(ctx.budget().snapshot().v8_external, 0);
    assert_eq!(ctx.active_sessions(), 1);
    // Imports remain in the append-only store, but cannot resurrect a retired env.
    assert!(
        instance
            .exports
            .get_typed_function::<(), ()>(&store, "init")
            .unwrap()
            .call(&mut store)
            .is_err()
    );
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    drop(session);
    assert_eq!(ctx.active_sessions(), 0);
}

#[test]
fn guest_release_rejects_a_provider_with_active_javascript_frames() {
    if !run_in_subprocess(
        "guest_release_rejects_a_provider_with_active_javascript_frames",
        Duration::from_secs(20),
    ) {
        return;
    }
    let mut store = Store::default();
    let (ctx, instance, memory, session) = instance(&mut store, |_| Ok(()));
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init_owned")
        .unwrap()
        .call(&mut store)
        .unwrap();
    let target = instance
        .exports
        .get_function("release_callback")
        .unwrap()
        .clone();
    instance
        .exports
        .get_table("__indirect_function_table")
        .unwrap()
        .set(&mut store, 1, Value::FuncRef(Some(target)))
        .unwrap();
    assert_eq!(ctx.budget().snapshot().live_isolates, 2);
    assert_eq!(
        eval(&mut store, &instance, &memory, "callback(); 42").unwrap(),
        42
    );
    assert_ne!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "release_status")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        0
    );
    assert_eq!(ctx.budget().snapshot().live_isolates, 2);
    assert_eq!(eval(&mut store, &instance, &memory, "40 + 2").unwrap(), 42);
    let release = instance
        .exports
        .get_typed_function::<(), i32>(&store, "release_owned")
        .unwrap();
    assert_eq!(release.call(&mut store).unwrap(), 0);
    assert_ne!(release.call(&mut store).unwrap(), 0);
    assert_eq!(ctx.budget().snapshot().live_isolates, 1);
    // Reuse the original default environment after the other env was released.
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert_eq!(ctx.budget().snapshot().live_isolates, 1);
    assert_eq!(eval(&mut store, &instance, &memory, "41 + 1").unwrap(), 42);
    // Both environments have no active guest, callback or pending provider job.
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
}

#[test]
fn contextify_callback_allows_another_threads_provider_to_progress() {
    if !run_in_subprocess(
        "contextify_callback_allows_another_threads_provider_to_progress",
        Duration::from_secs(30),
    ) {
        return;
    }

    let (entered, wait_for_callback) = std::sync::mpsc::channel();
    let (done, wait_for_worker) = std::sync::mpsc::channel();
    let wait_for_worker = Mutex::new(wait_for_worker);
    let worker = thread::spawn(move || {
        wait_for_callback
            .recv_timeout(Duration::from_secs(10))
            .unwrap();
        let mut store = Store::default();
        let (ctx, instance, memory, session) = instance(&mut store, |_| Ok(()));
        assert_eq!(eval(&mut store, &instance, &memory, "40 + 2").unwrap(), 42);
        // This thread owns the provider, with no outstanding call or job.
        unsafe {
            session.deactivate(&mut store.as_store_mut());
        }
        assert_eq!(ctx.budget().snapshot().live_isolates, 0);
        assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
        done.send(()).unwrap();
    });

    let mut store = Store::default();
    let (_ctx, instance, memory, _session) = instance(&mut store, move |_| {
        entered.send(()).unwrap();
        wait_for_worker
            .lock()
            .unwrap()
            .recv_timeout(Duration::from_secs(10))
            .map_err(|_| RuntimeError::new("another provider was blocked by contextify"))
    });
    let script = "callback(); 42";
    memory.view(&store).write(1024, script.as_bytes()).unwrap();
    let result = instance
        .exports
        .get_typed_function::<(i32, i32), i32>(&store, "eval_contextify")
        .unwrap()
        .call(&mut store, 1024, script.len() as i32);
    worker.join().unwrap();
    assert_eq!(result.unwrap(), 42);
}

#[test]
fn wasm_start_cannot_create_a_provider_before_configuration() {
    let mut store = Store::default();
    let module = Module::new(
        &store,
        wat::parse_str(
            r#"(module
                (import "env" "memory" (memory 1 1024 shared))
                (import "napi" "napi_wasm_init_env" (func $init (result i32)))
                (func $start call $init if unreachable end)
                (start $start)
                (func (export "init") (result i32) call $init)
            )"#,
        )
        .unwrap(),
    )
    .unwrap();
    let ctx = NapiCtx::default();
    let hooks = ctx.runtime_hooks();
    let (imports, state) = hooks
        .additional_imports(&module, &mut store.as_store_mut())
        .unwrap();
    let session = state.session().unwrap().clone();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
    assert_eq!(ctx.budget().snapshot().v8_external, 0);
    let Some(Extern::Memory(memory)) = imports.get_export("env", "memory") else {
        panic!("missing guest memory");
    };
    hooks
        .configure_instance(
            &module,
            &mut store.as_store_mut(),
            &instance,
            Some(&memory),
            state,
        )
        .unwrap();
    assert!(
        session
            .configure_instance(&mut store.as_store_mut(), &instance, Some(&memory))
            .is_err()
    );
    let init = instance
        .exports
        .get_typed_function::<(), i32>(&store, "init")
        .unwrap();
    assert_ne!(init.call(&mut store).unwrap(), 0);
    assert_eq!(ctx.budget().snapshot().live_isolates, 1);
    // All guest calls returned; the provider has no callback or pending job.
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(init.call(&mut store).unwrap(), 0);
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
}

#[test]
fn bridge_initializes_permissions_on_threads_predating_the_runtime() {
    if !run_in_subprocess(
        "bridge_initializes_permissions_on_threads_predating_the_runtime",
        Duration::from_secs(30),
    ) {
        return;
    }

    let mut releases = Vec::new();
    let mut workers = Vec::new();
    for _ in 0..2 {
        let (release, wait) = std::sync::mpsc::channel();
        let (ready, started) = std::sync::mpsc::channel();
        workers.push(thread::spawn(move || {
            ready.send(()).unwrap();
            wait.recv().unwrap();
            let mut store = Store::default();
            let (ctx, instance, memory, session) = instance(&mut store, |_| Ok(()));
            assert_eq!(eval(&mut store, &instance, &memory, "40 + 2").unwrap(), 42);
            // This worker owns its provider and all invocations have returned.
            unsafe {
                session.deactivate(&mut store.as_store_mut());
            }
            assert_eq!(ctx.budget().snapshot().live_isolates, 0);
            assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
        }));
        started.recv().unwrap();
        releases.push(release);
    }

    // Both OS threads already exist when the provider allocates its process-wide
    // protection key. They cannot inherit permissions from this initialization.
    let mut store = Store::default();
    let (ctx, instance, memory, session) = instance(&mut store, |_| Ok(()));
    assert_eq!(eval(&mut store, &instance, &memory, "41 + 1").unwrap(), 42);
    for release in releases {
        release.send(()).unwrap();
    }
    for worker in workers {
        worker.join().unwrap();
    }
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
}

#[test]
fn bridge_isolates_can_retire_in_arbitrary_creation_order() {
    if !run_in_subprocess(
        "bridge_isolates_can_retire_in_arbitrary_creation_order",
        Duration::from_secs(30),
    ) {
        return;
    }
    for order in [
        [0, 1, 2],
        [0, 2, 1],
        [1, 0, 2],
        [1, 2, 0],
        [2, 0, 1],
        [2, 1, 0],
    ] {
        let mut environments = (0..3)
            .map(|index| {
                let mut store = Store::default();
                let (ctx, instance, memory, session) = instance(&mut store, |_| Ok(()));
                let value = 40 + index;
                assert_eq!(
                    eval(
                        &mut store,
                        &instance,
                        &memory,
                        &format!("globalThis.identity = {value}; identity")
                    )
                    .unwrap(),
                    value
                );
                (ctx, store, instance, memory, session)
            })
            .collect::<Vec<_>>();
        for index in order {
            let (ctx, store, _, _, session) = &mut environments[index];
            // No invocation of this env is active. Other env roots on this same
            // thread may outlive it in any creation order.
            unsafe {
                session.deactivate(&mut store.as_store_mut());
            }
            assert_eq!(ctx.budget().snapshot().live_isolates, 0);
            assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
            for (index, (ctx, store, instance, memory, _)) in environments.iter_mut().enumerate() {
                if ctx.budget().snapshot().live_isolates != 0 {
                    assert_eq!(
                        eval(store, instance, memory, "identity").unwrap(),
                        40 + index as i32
                    );
                }
            }
        }
    }
}

#[test]
fn nested_bridge_isolate_restores_the_callers_current_isolate() {
    if !run_in_subprocess(
        "nested_bridge_isolate_restores_the_callers_current_isolate",
        Duration::from_secs(15),
    ) {
        return;
    }
    let entered = Arc::new(Mutex::new(0));
    let seen = entered.clone();
    let mut store = Store::default();
    let (_ctx, outer, memory, _session) = instance(&mut store, move |_| {
        let mut nested_store = Store::default();
        let (_ctx, nested, memory, session) = instance(&mut nested_store, |_| Ok(()));
        assert_eq!(eval(&mut nested_store, &nested, &memory, "17").unwrap(), 17);
        // The child invocation finished while the outer JavaScript callback is
        // still physically active. Teardown must restore the caller's isolate.
        unsafe {
            session.deactivate(&mut nested_store.as_store_mut());
        }
        *seen.lock().unwrap() += 1;
        Ok(())
    });
    assert_eq!(
        eval(&mut store, &outer, &memory, "callback(); 40 + 2").unwrap(),
        42
    );
    assert_eq!(*entered.lock().unwrap(), 1);
    assert_eq!(eval(&mut store, &outer, &memory, "41 + 1").unwrap(), 42);
}

#[test]
fn callback_cleanup_uses_its_pinned_environment_after_slot_replacement() {
    if !run_in_subprocess(
        "callback_cleanup_uses_its_pinned_environment_after_slot_replacement",
        Duration::from_secs(20),
    ) {
        return;
    }
    use std::sync::atomic::{AtomicUsize, Ordering};
    use wasmer::sys::vm::{StoreObject, VMFunctionEnvironment};
    use wasmer::{FunctionEnv, FunctionEnvMut};

    let mut store = Store::default();
    let before = VMFunctionEnvironment::list(store.objects_mut().as_sys_mut()).len();
    let callbacks = Arc::new(AtomicUsize::new(0));
    let observed = callbacks.clone();
    let (ctx, instance, memory, session) = instance_with_record(&mut store, move |store, ctx| {
        // The session appends its NapiEnv and the no-env imports append unit
        // cells. Validate the exact sole non-unit cell while no frame is active,
        // then retain only its opaque identity/index for the deliberate mutation.
        let candidates: Vec<_> = VMFunctionEnvironment::list(store.objects_mut().as_sys_mut())
            .iter()
            .enumerate()
            .skip(before)
            .filter(|(_, owner)| !owner.as_ref().is::<()>())
            .map(|(index, owner)| (index, owner.identity()))
            .collect();
        assert_eq!(candidates.len(), 1);
        let (index, identity) = candidates.into_iter().next().unwrap();
        let budget = ctx.budget();
        let unit = FunctionEnv::new(store, ());
        Function::new_typed_with_env(
            store,
            &unit,
            move |mut current: FunctionEnvMut<()>, _: i32| -> Result<(), RuntimeError> {
                let owner = VMFunctionEnvironment::list_mut(current.objects_mut().as_sys_mut())
                    .get_mut(index)
                    .unwrap();
                assert_eq!(owner.identity(), identity);
                drop(std::mem::replace(owner, VMFunctionEnvironment::new(())));
                assert_eq!(
                    budget.snapshot().live_isolates,
                    1,
                    "active CPP frame still owns the original NapiEnv"
                );
                observed.fetch_add(1, Ordering::SeqCst);
                Ok(())
            },
        )
    });
    let script = "callback(); callback(); 42";
    memory.view(&store).write(1024, script.as_bytes()).unwrap();
    let status = instance
        .exports
        .get_typed_function::<(i32, i32), i32>(&store, "eval_status")
        .unwrap()
        .call(&mut store, 1024, script.len() as i32)
        .unwrap();
    assert_eq!(
        status, 0,
        "paired callback cleanup must use the original cell"
    );
    assert_eq!(callbacks.load(Ordering::SeqCst), 1);
    assert_eq!(
        ctx.budget().snapshot().live_isolates,
        0,
        "original provider retires after its CPP call returns"
    );
    assert_eq!(ctx.budget().snapshot().v8_heap_reserved, 0);
    assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "fresh")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        9
    );
    drop(session);
    assert_eq!(ctx.active_sessions(), 0);
    let (_next_ctx, next, memory, _next_session) = self::instance(&mut store, |_| Ok(()));
    assert_eq!(eval(&mut store, &next, &memory, "40 + 2").unwrap(), 42);
}

// Match budget::external_accountant_tests; only the owning thread's armed hook
// may reenter the real Store, and taking it ends the TLS borrow before callout.
thread_local! {
    static EXTERNAL_ACCOUNTANT_HOOK: std::cell::RefCell<Option<Box<dyn FnOnce()>>> = const { std::cell::RefCell::new(None) };
}

struct ReentrantAccountant(std::sync::atomic::AtomicU64);
impl ReentrantAccountant {
    fn call_hook() {
        let hook = EXTERNAL_ACCOUNTANT_HOOK.with(|slot| slot.borrow_mut().take());
        if let Some(hook) = hook {
            hook();
        }
    }
}
impl wasmer_napi::NapiMemoryAccountant for ReentrantAccountant {
    fn memory_limit(&self) -> u64 {
        u64::MAX
    }
    fn memory_charged(&self) -> u64 {
        self.0.load(std::sync::atomic::Ordering::SeqCst)
    }
    fn try_charge(&self, bytes: u64) -> bool {
        Self::call_hook();
        self.0.fetch_add(bytes, std::sync::atomic::Ordering::SeqCst);
        true
    }
    fn uncharge(&self, bytes: u64) {
        self.0.fetch_sub(bytes, std::sync::atomic::Ordering::SeqCst);
        Self::call_hook();
    }
}

#[test]
fn external_memory_accountant_reenters_with_fresh_environment_borrows() {
    if !run_in_subprocess(
        "external_memory_accountant_reenters_with_fresh_environment_borrows",
        Duration::from_secs(15),
    ) {
        return;
    }
    use std::sync::atomic::{AtomicUsize, Ordering};
    let accountant = Arc::new(ReentrantAccountant(std::sync::atomic::AtomicU64::new(0)));
    let ctx = NapiCtx::builder().memory_accountant(accountant).build();
    let mut store = Store::default();
    let (ctx, instance, _, session) = instance_with_context(&mut store, ctx, |store, _| {
        Function::new_typed(store, |_: i32| {})
    });
    let nested = instance
        .exports
        .get_function("global_status")
        .unwrap()
        .clone();
    let calls = Arc::new(AtomicUsize::new(0));
    let adjust = instance
        .exports
        .get_typed_function::<i64, i32>(&store, "adjust_external")
        .unwrap();
    for (delta, expected) in [(64, 64), (-64, 0)] {
        let nested = nested.clone();
        let calls = calls.clone();
        EXTERNAL_ACCOUNTANT_HOOK.with(|slot| {
            *slot.borrow_mut() = Some(Box::new(move || {
                let result = Store::with_current(|store| {
                    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                        nested.call(store, &[])
                    }))
                });
                assert_eq!(&*result.unwrap().unwrap().unwrap(), &[Value::I32(0)]);
                calls.fetch_add(1, Ordering::SeqCst);
            }))
        });
        assert_eq!(adjust.call(&mut store, delta).unwrap(), 0);
        assert_eq!(ctx.budget().snapshot().v8_external, expected);
    }
    assert_eq!(calls.load(Ordering::SeqCst), 2);
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(ctx.budget().snapshot().v8_external, 0);
}

#[test]
fn external_memory_pending_charge_rolls_back_after_environment_replacement() {
    if !run_in_subprocess(
        "external_memory_pending_charge_rolls_back_after_environment_replacement",
        Duration::from_secs(15),
    ) {
        return;
    }
    use wasmer::sys::vm::{StoreObject, VMFunctionEnvironment};
    let accountant = Arc::new(ReentrantAccountant(std::sync::atomic::AtomicU64::new(0)));
    let ctx = NapiCtx::builder().memory_accountant(accountant).build();
    let mut store = Store::default();
    let before = VMFunctionEnvironment::list(store.objects_mut().as_sys_mut()).len();
    let mut target = None;
    let (ctx, instance, _, session) = instance_with_context(&mut store, ctx, |store, _| {
        let candidates: Vec<_> = VMFunctionEnvironment::list(store.objects_mut().as_sys_mut())
            .iter()
            .enumerate()
            .skip(before)
            .filter(|(_, owner)| !owner.as_ref().is::<()>())
            .map(|(index, owner)| (index, owner.identity()))
            .collect();
        assert_eq!(candidates.len(), 1);
        target = candidates.into_iter().next();
        Function::new_typed(store, |_: i32| {})
    });
    let (index, identity) = target.unwrap();
    EXTERNAL_ACCOUNTANT_HOOK.with(|slot| {
        *slot.borrow_mut() = Some(Box::new(move || {
            Store::with_current(|store| {
                let owner =
                    &mut VMFunctionEnvironment::list_mut(store.objects_mut().as_sys_mut())[index];
                assert_eq!(owner.identity(), identity);
                drop(std::mem::replace(owner, VMFunctionEnvironment::new(())));
            })
            .unwrap();
        }))
    });
    assert_eq!(
        instance
            .exports
            .get_typed_function::<i64, i32>(&store, "adjust_external")
            .unwrap()
            .call(&mut store, 64)
            .unwrap(),
        1
    );
    assert_eq!(
        ctx.budget().snapshot().v8_external,
        0,
        "unpublished charge rolled back"
    );
    assert_eq!(
        ctx.budget().snapshot().live_isolates,
        0,
        "original pin owns provider until adjustment returns"
    );
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "fresh")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        9
    );
    drop(session);
    let (_next_ctx, next, memory, _next_session) = self::instance(&mut store, |_| Ok(()));
    assert_eq!(eval(&mut store, &next, &memory, "40 + 2").unwrap(), 42);
}

#[test]
fn external_memory_rejects_unbalanced_and_out_of_range_guest_deltas() {
    if !run_in_subprocess(
        "external_memory_rejects_unbalanced_and_out_of_range_guest_deltas",
        Duration::from_secs(15),
    ) {
        return;
    }
    let mut store = Store::default();
    let (ctx, instance, memory, session) = self::instance(&mut store, |_| Ok(()));
    let adjust = instance
        .exports
        .get_typed_function::<i64, i32>(&store, "adjust_external")
        .unwrap();
    // Exercise repeated ordinary increments/decrements before boundary checks.
    for (delta, expected) in [(64, 64), (32, 96), (-32, 64)] {
        assert_eq!(adjust.call(&mut store, delta).unwrap(), 0);
        assert_eq!(ctx.budget().snapshot().v8_external, expected);
    }
    for delta in [-65, i64::MIN, i64::MAX, 1_i64 << 60, -(1_i64 << 60) - 1] {
        assert_eq!(
            adjust.call(&mut store, delta).unwrap(),
            1,
            "invalid delta {delta}"
        );
        assert_eq!(
            ctx.budget().snapshot().v8_external,
            64,
            "rejection preserves the accepted declaration"
        );
    }
    // A second provider may not debit the first provider's declaration.
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init_owned")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert_eq!(adjust.call(&mut store, -1).unwrap(), 1);
    assert_eq!(ctx.budget().snapshot().v8_external, 64);
    assert_eq!(adjust.call(&mut store, 16).unwrap(), 0);
    assert_eq!(adjust.call(&mut store, -16).unwrap(), 0);
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "release_owned")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        0
    );
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert_eq!(adjust.call(&mut store, -64).unwrap(), 0);
    assert_eq!(adjust.call(&mut store, -1).unwrap(), 1);
    assert_eq!(ctx.budget().snapshot().v8_external, 0);
    assert_eq!(eval(&mut store, &instance, &memory, "40 + 2").unwrap(), 42);
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
}

// Scalar-only observation in the old-source regression: no Store/T/JS access
// occurs from this accountant hook until the fixed-source sibling below.
struct RetirementAccountant(std::sync::atomic::AtomicU64);
impl wasmer_napi::NapiMemoryAccountant for RetirementAccountant {
    fn memory_limit(&self) -> u64 {
        512 * 1024 * 1024
    }
    fn memory_charged(&self) -> u64 {
        self.0.load(std::sync::atomic::Ordering::SeqCst)
    }
    fn try_charge(&self, bytes: u64) -> bool {
        self.0.fetch_add(bytes, std::sync::atomic::Ordering::SeqCst);
        true
    }
    fn uncharge(&self, bytes: u64) {
        self.0.fetch_sub(bytes, std::sync::atomic::Ordering::SeqCst);
        ReentrantAccountant::call_hook();
    }
}

unsafe extern "C" {
    fn snapi_bridge_unofficial_get_live_env_count() -> u64;
    fn snapi_bridge_unofficial_get_live_buffer_lease_count() -> u64;
}

#[test]
fn native_retirement_disposes_provider_before_releasing_its_charge() {
    if !run_in_subprocess(
        "native_retirement_disposes_provider_before_releasing_its_charge",
        Duration::from_secs(15),
    ) {
        return;
    }
    use std::sync::atomic::{AtomicU64, Ordering};
    let accountant = Arc::new(RetirementAccountant(AtomicU64::new(0)));
    let ctx = NapiCtx::builder().memory_accountant(accountant).build();
    let mut store = Store::default();
    let (ctx, instance, _, session) = instance_with_context(&mut store, ctx, |store, _| {
        Function::new_typed(store, |_: i32| {})
    });
    assert_eq!(unsafe { snapi_bridge_unofficial_get_live_env_count() }, 1);
    let observed = Arc::new(AtomicU64::new(u64::MAX));
    let during = observed.clone();
    EXTERNAL_ACCOUNTANT_HOOK.with(|slot| {
        *slot.borrow_mut() = Some(Box::new(move || {
            // Host registry diagnostic only: this deliberately does not access
            // old NapiEnv/Store/JavaScript while testing the old release order.
            during.store(
                unsafe { snapi_bridge_unofficial_get_live_env_count() },
                Ordering::SeqCst,
            );
        }));
    });
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(unsafe { snapi_bridge_unofficial_get_live_env_count() }, 0);
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().v8_heap_reserved, 0);
    assert_eq!(
        observed.load(Ordering::SeqCst),
        0,
        "provider membership must end before its reservation is released"
    );
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "fresh")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        9
    );
    drop(session);
    let (_next_ctx, next, memory, _session) = self::instance(&mut store, |_| Ok(()));
    assert_eq!(eval(&mut store, &next, &memory, "40 + 2").unwrap(), 42);
}

#[test]
fn native_retirement_accountant_reenters_only_an_inert_session() {
    if !run_in_subprocess(
        "native_retirement_accountant_reenters_only_an_inert_session",
        Duration::from_secs(20),
    ) {
        return;
    }
    use std::sync::atomic::{AtomicUsize, Ordering};
    let accountant = Arc::new(RetirementAccountant(std::sync::atomic::AtomicU64::new(0)));
    let ctx = NapiCtx::builder().memory_accountant(accountant).build();
    let mut store = Store::default();
    let (ctx, instance, _, session) = instance_with_context(&mut store, ctx, |store, _| {
        Function::new_typed(store, |_: i32| {})
    });
    let global = instance
        .exports
        .get_function("global_status")
        .unwrap()
        .clone();
    let calls = Arc::new(AtomicUsize::new(0));
    let nested = calls.clone();
    EXTERNAL_ACCOUNTANT_HOOK.with(|slot| {
        *slot.borrow_mut() = Some(Box::new(move || {
            assert_eq!(unsafe { snapi_bridge_unofficial_get_live_env_count() }, 0);
            let result = Store::with_current(|store| {
                std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| global.call(store, &[])))
            });
            assert_eq!(&*result.unwrap().unwrap().unwrap(), &[Value::I32(1)]);
            nested.fetch_add(1, Ordering::SeqCst);
        }));
    });
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(calls.load(Ordering::SeqCst), 1);
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().v8_heap_reserved, 0);
    assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
}

#[test]
fn native_retirement_preserves_accountant_panic_after_disposing_every_provider() {
    if !run_in_subprocess(
        "native_retirement_preserves_accountant_panic_after_disposing_every_provider",
        Duration::from_secs(20),
    ) {
        return;
    }
    use std::sync::atomic::{AtomicU64, Ordering};
    let accountant = Arc::new(RetirementAccountant(AtomicU64::new(0)));
    let ctx = NapiCtx::builder().memory_accountant(accountant).build();
    let mut store = Store::default();
    let (ctx, instance, _, session) = instance_with_context(&mut store, ctx, |store, _| {
        Function::new_typed(store, |_: i32| {})
    });
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init_owned")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert_eq!(unsafe { snapi_bridge_unofficial_get_live_env_count() }, 2);
    let original = Arc::new(());
    let payload = original.clone();
    let observed = Arc::new(AtomicU64::new(u64::MAX));
    let at_panic = observed.clone();
    EXTERNAL_ACCOUNTANT_HOOK.with(|slot| {
        *slot.borrow_mut() = Some(Box::new(move || {
            at_panic.store(
                unsafe { snapi_bridge_unofficial_get_live_env_count() },
                Ordering::SeqCst,
            );
            std::panic::panic_any(payload);
        }));
    });
    let panic = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        session.deactivate(&mut store.as_store_mut());
    }))
    .unwrap_err();
    assert!(Arc::ptr_eq(
        panic.downcast_ref::<Arc<()>>().unwrap(),
        &original
    ));
    assert_eq!(observed.load(Ordering::SeqCst), 0);
    assert_eq!(unsafe { snapi_bridge_unofficial_get_live_env_count() }, 0);
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().v8_heap_reserved, 0);
    assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "fresh")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        9
    );
}

#[test]
fn native_retirement_releases_only_the_selected_providers_external_charge() {
    if !run_in_subprocess(
        "native_retirement_releases_only_the_selected_providers_external_charge",
        Duration::from_secs(20),
    ) {
        return;
    }
    let mut store = Store::default();
    let (ctx, instance, _, session) = instance(&mut store, |_| Ok(()));
    let adjust = instance
        .exports
        .get_typed_function::<i64, i32>(&store, "adjust_external")
        .unwrap();
    assert_eq!(adjust.call(&mut store, 64).unwrap(), 0);
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init_owned")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert_eq!(adjust.call(&mut store, 32).unwrap(), 0);
    assert_eq!(ctx.budget().snapshot().v8_external, 96);
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "release_owned")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        0
    );
    assert_eq!(ctx.budget().snapshot().v8_external, 64);
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert_eq!(adjust.call(&mut store, -65).unwrap(), 1);
    assert_eq!(adjust.call(&mut store, -64).unwrap(), 0);
    assert_eq!(ctx.budget().snapshot().v8_external, 0);
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
}

fn run_explicit_release_finalizer(terminate: bool) {
    let seen = Arc::new(Mutex::new(Vec::new()));
    let observations = seen.clone();
    let acquired = Arc::new(std::sync::atomic::AtomicU64::new(u64::MAX));
    let lease_observation = acquired.clone();
    let mut store = Store::default();
    let ctx = NapiCtx::default();
    let control = ctx.runtime_control();
    let (notify, entered) = std::sync::mpsc::sync_channel(1);
    let (ctx, instance, memory, session) =
        instance_with_context(&mut store, ctx, move |store, _| {
            Function::new_typed(store, move |code: i32| {
                observations.lock().unwrap().push(code);
                if code == 4 {
                    lease_observation.store(
                        unsafe { snapi_bridge_unofficial_get_live_buffer_lease_count() },
                        std::sync::atomic::Ordering::SeqCst,
                    );
                }
                if terminate && code == 1 {
                    notify.send(()).unwrap();
                }
            })
        });
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init_owned")
        .unwrap()
        .call(&mut store)
        .unwrap();
    if !terminate {
        assert_eq!(
            instance
                .exports
                .get_typed_function::<i64, i32>(&store, "adjust_external")
                .unwrap()
                .call(&mut store, 64)
                .unwrap(),
            0
        );
    }
    let script = if terminate {
        "callback(); while (true) {}"
    } else {
        "callback(); new Uint8Array(16)"
    };
    memory.view(&store).write(1024, script.as_bytes()).unwrap();
    instance
        .exports
        .get_typed_function::<(i32, i32), ()>(&store, "arm_finalizer")
        .unwrap()
        .call(&mut store, script.len() as i32, i32::from(!terminate))
        .unwrap();
    let worker = terminate.then(|| {
        thread::spawn(move || {
            entered
                .recv_timeout(Duration::from_secs(5))
                .expect("entered explicit finalizer callback");
            control.terminate_all();
        })
    });
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "release_owned")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        0
    );
    if let Some(worker) = worker {
        worker.join().unwrap();
    }
    assert_eq!(
        *seen.lock().unwrap(),
        if terminate { vec![3, 1] } else { vec![3, 1, 4] }
    );
    if !terminate {
        assert_eq!(
            acquired.load(std::sync::atomic::Ordering::SeqCst),
            1,
            "the finalizer actually published a persistent lease shell"
        );
    }
    assert_eq!(
        unsafe { snapi_bridge_unofficial_get_live_buffer_lease_count() },
        0,
        "post-finalizer resources are released before the reference factory dies"
    );
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "finalizer_status")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        if terminate { 10 } else { 0 }
    );
    assert_eq!(
        ctx.budget().snapshot().v8_external,
        0,
        "explicit release takes the final original-cell declaration ledger"
    );
    assert_eq!(ctx.budget().snapshot().live_isolates, 1);
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
}

#[test]
fn native_retirement_preserves_explicit_release_finalizer_callbacks() {
    if !run_in_subprocess(
        "native_retirement_preserves_explicit_release_finalizer_callbacks",
        Duration::from_secs(20),
    ) {
        return;
    }
    run_explicit_release_finalizer(false);
}

#[test]
fn native_retirement_keeps_foreign_termination_live_through_finalizer_drain() {
    if !run_in_subprocess(
        "native_retirement_keeps_foreign_termination_live_through_finalizer_drain",
        Duration::from_secs(20),
    ) {
        return;
    }
    run_explicit_release_finalizer(true);
}

// Match the existing accountant siblings. The old-source panic probe uses
// only this scalar hook; Store reentry is exercised on the repaired source.
struct CreationAccountant {
    charged: std::sync::atomic::AtomicU64,
    panic_after_charge: std::sync::atomic::AtomicBool,
    pending_query: std::sync::atomic::AtomicBool,
    providers_at_query: std::sync::atomic::AtomicU64,
}
impl CreationAccountant {
    fn new() -> Self {
        Self {
            charged: std::sync::atomic::AtomicU64::new(0),
            panic_after_charge: std::sync::atomic::AtomicBool::new(false),
            pending_query: std::sync::atomic::AtomicBool::new(false),
            providers_at_query: std::sync::atomic::AtomicU64::new(u64::MAX),
        }
    }
}
impl wasmer_napi::NapiMemoryAccountant for CreationAccountant {
    fn memory_limit(&self) -> u64 {
        use std::sync::atomic::Ordering;
        if self.pending_query.swap(false, Ordering::SeqCst) {
            self.providers_at_query.store(
                unsafe { snapi_bridge_unofficial_get_live_env_count() },
                Ordering::SeqCst,
            );
            std::panic::panic_any("creation limit query");
        }
        512 * 1024 * 1024
    }
    fn memory_charged(&self) -> u64 {
        self.charged.load(std::sync::atomic::Ordering::SeqCst)
    }
    fn try_charge(&self, bytes: u64) -> bool {
        use std::sync::atomic::Ordering;
        // A throwing hook has not charged anything yet.
        ReentrantAccountant::call_hook();
        self.charged.fetch_add(bytes, Ordering::SeqCst);
        if self.panic_after_charge.swap(false, Ordering::SeqCst) {
            self.pending_query.store(true, Ordering::SeqCst);
        }
        true
    }
    fn uncharge(&self, bytes: u64) {
        self.charged
            .fetch_sub(bytes, std::sync::atomic::Ordering::SeqCst);
    }
}

#[test]
fn native_creation_owns_reservation_before_the_value_limit_query() {
    if !run_in_subprocess(
        "native_creation_owns_reservation_before_the_value_limit_query",
        Duration::from_secs(20),
    ) {
        return;
    }
    use std::sync::atomic::Ordering;
    let accountant = Arc::new(CreationAccountant::new());
    let ctx = NapiCtx::builder()
        .memory_accountant(accountant.clone())
        .max_envs(1)
        .build();
    let mut store = Store::default();
    let (ctx, instance, _, session) =
        configured_instance_with_context(&mut store, ctx, |store, _| {
            Function::new_typed(store, |_: i32| {})
        });
    accountant.panic_after_charge.store(true, Ordering::SeqCst);
    let init = instance
        .exports
        .get_typed_function::<(), i32>(&store, "init_id")
        .unwrap();
    let panic = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| init.call(&mut store)))
        .unwrap_err();
    assert_eq!(panic.downcast_ref::<&str>(), Some(&"creation limit query"));
    // Dispose all logical providers before checking scalar ownership. The old
    // path publishes a provider but strands its reservation before charge-map insertion.
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(unsafe { snapi_bridge_unofficial_get_live_env_count() }, 0);
    assert_eq!(
        ctx.budget().snapshot().live_isolates,
        0,
        "failed creation must release its reservation"
    );
    assert_eq!(ctx.budget().snapshot().v8_heap_reserved, 0);
    assert_eq!(accountant.providers_at_query.load(Ordering::SeqCst), 0);
    assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
    assert_eq!(
        wasmer_napi::NapiMemoryAccountant::memory_charged(&*accountant),
        0
    );
    let (_next_ctx, next, memory, _session) = self::instance(&mut store, |_| Ok(()));
    assert_eq!(eval(&mut store, &next, &memory, "40 + 2").unwrap(), 42);
}

#[test]
fn native_creation_rechecks_default_after_accountant_reentry() {
    if !run_in_subprocess(
        "native_creation_rechecks_default_after_accountant_reentry",
        Duration::from_secs(20),
    ) {
        return;
    }
    use std::sync::atomic::{AtomicI32, Ordering};
    let accountant = Arc::new(CreationAccountant::new());
    let ctx = NapiCtx::builder()
        .memory_accountant(accountant)
        .max_envs(2)
        .build();
    let mut store = Store::default();
    let (ctx, instance, memory, session) =
        configured_instance_with_context(&mut store, ctx, |store, _| {
            Function::new_typed(store, |_: i32| {})
        });
    let init = instance.exports.get_function("init_id").unwrap().clone();
    let nested_id = Arc::new(AtomicI32::new(0));
    let observed = nested_id.clone();
    EXTERNAL_ACCOUNTANT_HOOK.with(|slot| {
        *slot.borrow_mut() = Some(Box::new(move || {
            let result = Store::with_current(|store| {
                std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| init.call(store, &[])))
            });
            let values = result.unwrap().unwrap().unwrap();
            observed.store(values[0].i32().unwrap(), Ordering::SeqCst);
        }))
    });
    let outer = instance
        .exports
        .get_typed_function::<(), i32>(&store, "init_id")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert!(outer > 0);
    assert_eq!(outer, nested_id.load(Ordering::SeqCst));
    assert_eq!(ctx.budget().snapshot().live_isolates, 1);
    assert_eq!(unsafe { snapi_bridge_unofficial_get_live_env_count() }, 1);
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert_eq!(eval(&mut store, &instance, &memory, "40 + 2").unwrap(), 42);
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().v8_heap_reserved, 0);
}

#[test]
fn native_creation_revalidates_attachment_and_stop_before_publication() {
    if !run_in_subprocess(
        "native_creation_revalidates_attachment_and_stop_before_publication",
        Duration::from_secs(25),
    ) {
        return;
    }
    use wasmer::sys::vm::{StoreObject, VMFunctionEnvironment};
    for detach in [true, false] {
        let accountant = Arc::new(CreationAccountant::new());
        let ctx = NapiCtx::builder()
            .memory_accountant(accountant)
            .max_envs(1)
            .build();
        let control = ctx.runtime_control();
        let mut store = Store::default();
        let before = VMFunctionEnvironment::list(store.objects_mut().as_sys_mut()).len();
        let mut target = None;
        let (ctx, instance, _, session) =
            configured_instance_with_context(&mut store, ctx, |store, _| {
                let candidates: Vec<_> =
                    VMFunctionEnvironment::list(store.objects_mut().as_sys_mut())
                        .iter()
                        .enumerate()
                        .skip(before)
                        .filter(|(_, owner)| !owner.as_ref().is::<()>())
                        .map(|(index, owner)| (index, owner.identity()))
                        .collect();
                assert_eq!(candidates.len(), 1);
                target = candidates.into_iter().next();
                Function::new_typed(store, |_: i32| {})
            });
        let (index, identity) = target.unwrap();
        EXTERNAL_ACCOUNTANT_HOOK.with(|slot| {
            *slot.borrow_mut() = Some(Box::new(move || {
                if detach {
                    Store::with_current(|store| {
                        let owner =
                            &mut VMFunctionEnvironment::list_mut(store.objects_mut().as_sys_mut())
                                [index];
                        assert_eq!(owner.identity(), identity);
                        drop(std::mem::replace(owner, VMFunctionEnvironment::new(())));
                    })
                    .unwrap();
                } else {
                    control.terminate_all();
                }
            }))
        });
        let id = instance
            .exports
            .get_typed_function::<(), i32>(&store, "init_id")
            .unwrap()
            .call(&mut store)
            .unwrap();
        assert_eq!(id, 0);
        assert_eq!(ctx.budget().snapshot().live_isolates, 0);
        assert_eq!(ctx.budget().snapshot().v8_heap_reserved, 0);
        assert_eq!(unsafe { snapi_bridge_unofficial_get_live_env_count() }, 0);
        assert_eq!(
            instance
                .exports
                .get_typed_function::<(), i32>(&store, "fresh")
                .unwrap()
                .call(&mut store)
                .unwrap(),
            9
        );
        if !detach {
            unsafe {
                session.deactivate(&mut store.as_store_mut());
            }
        }
        drop(session);
        let (_next_ctx, next, memory, _session) = self::instance(&mut store, |_| Ok(()));
        assert_eq!(eval(&mut store, &next, &memory, "40 + 2").unwrap(), 42);
    }
}

#[test]
fn native_creation_rolls_back_provider_when_guest_output_is_invalid() {
    if !run_in_subprocess(
        "native_creation_rolls_back_provider_when_guest_output_is_invalid",
        Duration::from_secs(20),
    ) {
        return;
    }
    let accountant = Arc::new(CreationAccountant::new());
    let ctx = NapiCtx::builder()
        .memory_accountant(accountant)
        .max_envs(1)
        .build();
    let mut store = Store::default();
    let (ctx, instance, memory, session) =
        configured_instance_with_context(&mut store, ctx, |store, _| {
            Function::new_typed(store, |_: i32| {})
        });
    // Explicit provider creation requires the runtime's prior configuration.
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "configure_runtime")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        0
    );
    let create = instance
        .exports
        .get_typed_function::<(i32, i32), i32>(&store, "create_status")
        .unwrap();
    // The env id write succeeds, the scope write does not. Neither id may
    // become a logical owner, and the already-created provider must be retired.
    assert_eq!(create.call(&mut store, 32, i32::MAX).unwrap(), 1);
    let mut bytes = [0; 4];
    memory.view(&store).read(32, &mut bytes).unwrap();
    let rejected_id = u32::from_le_bytes(bytes);
    assert!(rejected_id > 0);
    assert_eq!(unsafe { snapi_bridge_unofficial_get_live_env_count() }, 0);
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().v8_heap_reserved, 0);
    assert_eq!(create.call(&mut store, 32, 36).unwrap(), 0);
    memory.view(&store).read(32, &mut bytes).unwrap();
    assert!(
        u32::from_le_bytes(bytes) > rejected_id,
        "failed ids are never reused"
    );
    assert_eq!(ctx.budget().snapshot().live_isolates, 1);
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "release_owned")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        0
    );
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert_eq!(eval(&mut store, &instance, &memory, "40 + 2").unwrap(), 42);
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
}

#[test]
fn native_creation_accountant_reenters_an_existing_provider_with_fresh_borrows() {
    if !run_in_subprocess(
        "native_creation_accountant_reenters_an_existing_provider_with_fresh_borrows",
        Duration::from_secs(20),
    ) {
        return;
    }
    use std::sync::atomic::{AtomicUsize, Ordering};
    let accountant = Arc::new(CreationAccountant::new());
    let ctx = NapiCtx::builder()
        .memory_accountant(accountant)
        .max_envs(2)
        .build();
    let mut store = Store::default();
    let (ctx, instance, memory, session) = instance_with_context(&mut store, ctx, |store, _| {
        Function::new_typed(store, |_: i32| {})
    });
    let nested = instance
        .exports
        .get_function("global_status")
        .unwrap()
        .clone();
    let calls = Arc::new(AtomicUsize::new(0));
    let observed = calls.clone();
    EXTERNAL_ACCOUNTANT_HOOK.with(|slot| {
        *slot.borrow_mut() = Some(Box::new(move || {
            let result = Store::with_current(|store| {
                std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| nested.call(store, &[])))
            });
            assert_eq!(&*result.unwrap().unwrap().unwrap(), &[Value::I32(0)]);
            observed.fetch_add(1, Ordering::SeqCst);
        }))
    });
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(i32, i32), i32>(&store, "create_status")
            .unwrap()
            .call(&mut store, 32, 36)
            .unwrap(),
        0
    );
    assert_eq!(calls.load(Ordering::SeqCst), 1);
    assert_eq!(ctx.budget().snapshot().live_isolates, 2);
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "release_owned")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        0
    );
    assert_eq!(eval(&mut store, &instance, &memory, "40 + 2").unwrap(), 42);
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().v8_heap_reserved, 0);
}

#[cfg(feature = "experimental-async")]
mod native_frame_tests {
    use super::*;
    use std::{
        future::Future,
        pin::Pin,
        sync::atomic::{AtomicBool, AtomicUsize, Ordering},
        task::{Context, Poll, Waker},
    };
    use wasmer::{AsStoreAsync, sys::NativeInvocationScope};

    struct CallbackBody {
        ready: Arc<AtomicBool>,
        polls: Arc<AtomicUsize>,
        drops: Arc<AtomicUsize>,
        panic: Option<Arc<()>>,
    }
    impl Future for CallbackBody {
        type Output = Result<(), RuntimeError>;
        fn poll(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<Self::Output> {
            self.polls.fetch_add(1, Ordering::SeqCst);
            if let Some(panic) = &self.panic {
                std::panic::panic_any(panic.clone());
            }
            if self.ready.load(Ordering::SeqCst) {
                Poll::Ready(Ok(()))
            } else {
                Poll::Pending
            }
        }
    }
    impl Drop for CallbackBody {
        fn drop(&mut self) {
            self.drops.fetch_add(1, Ordering::SeqCst);
        }
    }

    #[derive(Clone, Copy)]
    enum Completion {
        Ready,
        Cancel,
        Panic,
    }

    // Existing callback_control owned-child fixture, with the real typed async
    // record import reached by JS -> cached CPP context -> Wasm table callback.
    fn pending_proof(completion: Completion) {
        let mut store = Store::default();
        let ready = Arc::new(AtomicBool::new(false));
        let polls = Arc::new(AtomicUsize::new(0));
        let drops = Arc::new(AtomicUsize::new(0));
        let original = Arc::new(());
        let pending_ready = ready.clone();
        let pending_polls = polls.clone();
        let pending_drops = drops.clone();
        let panic = matches!(completion, Completion::Panic).then(|| original.clone());
        let (ctx, instance, memory, session) = instance_with_record(&mut store, move |store, _| {
            Function::new_typed_async(store, move |_tag: i32| CallbackBody {
                ready: pending_ready.clone(),
                polls: pending_polls.clone(),
                drops: pending_drops.clone(),
                panic: panic.clone(),
            })
        });
        // Like callback_error_stops_js_and_preserves_original_payload, test
        // V8 termination at a backedge; it promises no zero-bytecode boundary.
        let script = match completion {
            Completion::Ready => "callback(); globalThis.nativeAfter = 1; 42",
            Completion::Cancel | Completion::Panic => {
                "try { callback(); } catch (_) {} while (true) {} globalThis.nativeAfter = 1; 42"
            }
        };
        memory.view(&store).write(1024, script.as_bytes()).unwrap();
        let eval = instance.exports.get_function("eval").unwrap().clone();
        let fresh = instance.exports.get_function("fresh").unwrap().clone();
        let mut scope = NativeInvocationScope::bind(&mut store).unwrap();
        let store = store.into_async();
        let mut call = Box::pin(eval.call_async(
            &store,
            vec![Value::I32(1024), Value::I32(script.len() as i32)],
        ));
        let first = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            call.as_mut().poll(&mut Context::from_waker(Waker::noop()))
        }));
        let first_pending = matches!(&first, Ok(Poll::Pending));
        let mut original_panic = None;
        let mut result = None;
        match completion {
            Completion::Ready => {
                if first_pending {
                    ready.store(true, Ordering::SeqCst);
                    result = Some(call.as_mut().poll(&mut Context::from_waker(Waker::noop())));
                }
            }
            Completion::Cancel => {
                scope.close();
            }
            Completion::Panic => {
                original_panic = first.err();
            }
        }
        // The original CPP call was parked through a same-Store borrowed Sync
        // suffix; cancellation must use the graceful driver and exit CPP before
        // the generation reports physical completion. No forced-unwind model.
        drop(call);
        let settled = {
            let mut wait = Box::pin(scope.close_and_wait());
            wait.as_mut().poll(&mut Context::from_waker(Waker::noop()))
        };
        let (next_scope, next_store) = {
            let mut guard = store.store().write();
            scope.successor(&store, &mut guard.as_store_mut()).unwrap()
        };
        let mut next = Box::pin(fresh.call_async(&next_store, vec![]));
        let fresh_result = next.as_mut().poll(&mut Context::from_waker(Waker::noop()));
        drop(next);
        let after = {
            let mut guard = next_store.store().write();
            // Inspect real JS state after all prior CPP frames have exited.
            super::eval(
                &mut guard,
                &instance,
                &memory,
                "typeof nativeAfter === 'undefined' ? 0 : nativeAfter",
            )
            .unwrap()
        };
        {
            let mut guard = next_store.store().write();
            unsafe {
                session.deactivate(&mut guard.as_store_mut());
            }
        }
        drop(next_scope);
        drop(next_store);
        drop(scope);
        drop(session);
        drop(instance);
        drop(store);
        assert!(
            matches!(settled, Poll::Ready(Ok(()))),
            "CPP frame remained physically active after cancellation"
        );
        assert!(matches!(fresh_result, Poll::Ready(Ok(ref values)) if values[0].i32() == Some(9)));
        assert_eq!(drops.load(Ordering::SeqCst), 1);
        assert!(polls.load(Ordering::SeqCst) >= 1);
        assert_eq!(ctx.budget().snapshot().live_isolates, 0);
        assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
        match completion {
            Completion::Ready => {
                assert!(first_pending, "real native callback did not suspend");
                assert!(
                    matches!(result, Some(Poll::Ready(Ok(ref values))) if values[0].i32() == Some(42))
                );
                assert_eq!(after, 1);
            }
            Completion::Cancel => {
                assert!(first_pending, "real native callback did not suspend");
                assert_eq!(
                    after, 0,
                    "JS continued after its guest callback was cancelled"
                );
            }
            Completion::Panic => {
                assert!(Arc::ptr_eq(
                    original_panic.unwrap().downcast_ref::<Arc<()>>().unwrap(),
                    &original
                ));
                assert_eq!(after, 0, "JS continued after the original callback panic");
            }
        }
    }

    #[test]
    fn native_cpp_callback_pending_completes_after_ready() {
        if run_in_subprocess(
            "native_frame_tests::native_cpp_callback_pending_completes_after_ready",
            Duration::from_secs(30),
        ) {
            pending_proof(Completion::Ready);
        }
    }
    #[test]
    fn native_cpp_callback_pending_cancels_before_physical_completion() {
        if run_in_subprocess(
            "native_frame_tests::native_cpp_callback_pending_cancels_before_physical_completion",
            Duration::from_secs(30),
        ) {
            pending_proof(Completion::Cancel);
        }
    }
    #[test]
    fn native_cpp_callback_preserves_original_async_panic() {
        if run_in_subprocess(
            "native_frame_tests::native_cpp_callback_preserves_original_async_panic",
            Duration::from_secs(30),
        ) {
            pending_proof(Completion::Panic);
        }
    }
    #[test]
    fn native_cpp_without_coroutine_allows_ready_and_refuses_pending() {
        if !run_in_subprocess(
            "native_frame_tests::native_cpp_without_coroutine_allows_ready_and_refuses_pending",
            Duration::from_secs(30),
        ) {
            return;
        }
        for ready_now in [true, false] {
            let mut store = Store::default();
            let polls = Arc::new(AtomicUsize::new(0));
            let drops = Arc::new(AtomicUsize::new(0));
            let outside_coroutine = Arc::new(AtomicBool::new(false));
            let record_polls = polls.clone();
            let record_drops = drops.clone();
            let record_outside = outside_coroutine.clone();
            let (ctx, instance, memory, session) =
                instance_with_record(&mut store, move |store, _| {
                    Function::new_async(
                        store,
                        wasmer::FunctionType::new([wasmer::Type::I32], []),
                        move |_args| {
                            record_outside.store(
                                wasmer::sys::NativeEngineInvocation::current().is_none(),
                                Ordering::SeqCst,
                            );
                            let body = CallbackBody {
                                ready: Arc::new(AtomicBool::new(ready_now)),
                                polls: record_polls.clone(),
                                drops: record_drops.clone(),
                                panic: None,
                            };
                            async move {
                                body.await?;
                                Ok(Vec::new())
                            }
                        },
                    )
                });
            let outcome = super::eval(
                &mut store,
                &instance,
                &memory,
                if ready_now {
                    "callback(); globalThis.nativeAfter = 1; 42"
                } else {
                    // A later callback is also refused by the same first slot;
                    // the first body's poll/drop counters must remain one.
                    "try { callback(); } catch (_) {} callback(); while (true) {} globalThis.nativeAfter = 1; 42"
                },
            );
            let after = super::eval(
                &mut store,
                &instance,
                &memory,
                "typeof nativeAfter === 'undefined' ? 0 : nativeAfter",
            )
            .unwrap();
            let fresh = super::eval(&mut store, &instance, &memory, "40 + 2").unwrap();
            unsafe {
                session.deactivate(&mut store.as_store_mut());
            }
            drop(instance);
            drop(session);
            drop(store);
            assert!(outside_coroutine.load(Ordering::SeqCst));
            assert_eq!(polls.load(Ordering::SeqCst), 1);
            assert_eq!(drops.load(Ordering::SeqCst), 1);
            assert_eq!(fresh, 42);
            assert_eq!(ctx.budget().snapshot().live_isolates, 0);
            assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
            if ready_now {
                assert_eq!(outcome.unwrap(), 42);
                assert_eq!(after, 1);
            } else {
                assert!(outcome.is_err());
                assert_eq!(after, 0);
            }
        }
    }
    #[test]
    #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
    fn native_cpp_relocates_and_restores_the_configured_stack_limit() {
        if !run_in_subprocess(
            "native_frame_tests::native_cpp_relocates_and_restores_the_configured_stack_limit",
            Duration::from_secs(30),
        ) {
            return;
        }
        unsafe extern "C" {
            fn snapi_bridge_native_current_stack_info(
                saved: *mut usize,
                budget: *mut usize,
                applied: *mut usize,
            ) -> i32;
        }
        // Match the existing owned async fixtures; all bodies here are Ready.
        fn complete<F: Future>(future: F) -> F::Output {
            let mut future = Box::pin(future);
            for _ in 0..8 {
                if let Poll::Ready(result) = future
                    .as_mut()
                    .poll(&mut Context::from_waker(Waker::noop()))
                {
                    return result;
                }
            }
            panic!("a Ready stack-limit fixture remained pending");
        }
        let observations = Arc::new(Mutex::new(Vec::new()));
        let recorded = observations.clone();
        let mut store = Store::default();
        let (ctx, instance, memory, session) =
            configured_instance_with_context(&mut store, NapiCtx::default(), move |store, _| {
                Function::new_async(
                    store,
                    wasmer::FunctionType::new([wasmer::Type::I32], []),
                    move |_| {
                        let mut saved = 0;
                        let mut budget = 0;
                        let mut applied = 0;
                        let present = unsafe {
                            snapi_bridge_native_current_stack_info(
                                &mut saved,
                                &mut budget,
                                &mut applied,
                            )
                        };
                        let bounds = wasmer::sys::NativeEngineStackBounds::current()
                            .map(|bounds| (bounds.usable_low, bounds.base));
                        recorded
                            .lock()
                            .unwrap()
                            .push((present, saved, budget, applied, bounds));
                        async { Ok(Vec::new()) }
                    },
                )
            });
        let flags = b"--stack-size=512";
        let options: Vec<_> = [16_u32, 1, 256, flags.len() as u32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect();
        memory.view(&store).write(128, &options).unwrap();
        memory.view(&store).write(256, flags).unwrap();
        let configured = instance
            .exports
            .get_typed_function::<i32, i32>(&store, "configure_runtime_options")
            .unwrap()
            .call(&mut store, 128)
            .unwrap();
        let init = instance.exports.get_function("init").unwrap().clone();
        let run = instance.exports.get_function("eval").unwrap().clone();
        let scope = NativeInvocationScope::bind(&mut store).unwrap();
        let store = store.into_async();
        // Initialization itself occurs on the real API coroutine stack, before
        // the provider's first Context::New and ordinary getter frames.
        let initialized = complete(init.call_async(&store, vec![]));
        let script = "callback(); 42";
        {
            let guard = store.store().write();
            memory.view(&guard).write(1024, script.as_bytes()).unwrap();
        }
        let rooted = complete(run.call_async(
            &store,
            vec![Value::I32(1024), Value::I32(script.len() as i32)],
        ));
        let synchronous = {
            let mut guard = store.store().write();
            super::eval(&mut guard, &instance, &memory, script)
        };
        {
            let mut guard = store.store().write();
            unsafe {
                session.deactivate(&mut guard.as_store_mut());
            }
        }
        drop(scope);
        drop(instance);
        drop(session);
        drop(store);
        let observations = observations.lock().unwrap();
        assert_eq!(ctx.budget().snapshot().live_isolates, 0);
        assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
        assert_eq!(configured, 0);
        initialized.unwrap();
        assert_eq!(rooted.unwrap()[0].i32(), Some(42));
        assert_eq!(synchronous.unwrap(), 42);
        assert_eq!(observations.len(), 2);
        let (present, saved, budget, applied, bounds) = observations[0];
        assert_eq!(present, 1);
        assert_eq!(budget, 512 * 1024);
        let (low, base) = bounds.expect("root callback must expose actual numeric stack bounds");
        assert_eq!(applied, (low + 64 * 1024).max(base - budget));
        assert_ne!(applied, saved, "the coroutine reused the OS stack limit");
        let (present, later_saved, budget, applied, bounds) = observations[1];
        assert_eq!(present, 1);
        assert!(bounds.is_none());
        assert_eq!(budget, 512 * 1024);
        assert_eq!(
            later_saved, saved,
            "root exit did not restore the exact prior OS limit"
        );
        assert_eq!(applied, saved);
    }

    #[test]
    #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
    fn native_cpp_zero_stack_budget_retires_failed_creation() {
        if !run_in_subprocess(
            "native_frame_tests::native_cpp_zero_stack_budget_retires_failed_creation",
            Duration::from_secs(30),
        ) {
            return;
        }
        let mut store = Store::default();
        let (ctx, instance, memory, session) =
            configured_instance_with_context(&mut store, NapiCtx::default(), |store, _| {
                Function::new_typed(store, |_: i32| {})
            });
        let flags = b"--stack-size=0";
        let options: Vec<_> = [16_u32, 1, 256, flags.len() as u32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect();
        memory.view(&store).write(128, &options).unwrap();
        memory.view(&store).write(256, flags).unwrap();
        let configured = instance
            .exports
            .get_typed_function::<i32, i32>(&store, "configure_runtime_options")
            .unwrap()
            .call(&mut store, 128);
        // Reading the configured limit succeeds, but ordinary managed entry
        // must fail. Inert recovery still has to dispose the initialized isolate.
        let initialized = instance
            .exports
            .get_typed_function::<(), i32>(&store, "init_id")
            .unwrap()
            .call(&mut store);
        unsafe {
            session.deactivate(&mut store.as_store_mut());
        }
        drop(instance);
        drop(session);
        drop(store);
        // V8 flags are process-wide and are deliberately not reset. This fresh
        // independent Wasm Store checks recovery without creating another V8 env.
        let mut successor = Store::default();
        let module = Module::new(
            &successor,
            r#"(module (func (export "fresh") (result i32) i32.const 42))"#,
        )
        .unwrap();
        let instance = Instance::new(&mut successor, &module, &wasmer::imports! {}).unwrap();
        let fresh = instance
            .exports
            .get_typed_function::<(), i32>(&successor, "fresh")
            .unwrap()
            .call(&mut successor);
        drop(instance);
        drop(module);
        drop(successor);
        assert_eq!(configured.unwrap(), 0);
        assert!(
            initialized.is_err(),
            "zero JS budget admitted a native provider"
        );
        assert_eq!(unsafe { snapi_bridge_unofficial_get_live_env_count() }, 0);
        let usage = ctx.budget().snapshot();
        assert_eq!(usage.live_isolates, 0);
        assert_eq!(usage.v8_heap_reserved, 0);
        assert_eq!(usage.wasm_linear, 0);
        assert_eq!(fresh.unwrap(), 42);
    }

    #[test]
    #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
    fn native_cpp_preserves_same_stack_nested_js_roots_and_exceptions() {
        if run_in_subprocess(
            "native_frame_tests::native_cpp_preserves_same_stack_nested_js_roots_and_exceptions",
            Duration::from_secs(30),
        ) {
            nested_stack_proof(false);
        }
    }

    #[test]
    #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
    fn native_cpp_refuses_same_isolate_host_body_reentry_across_stacks() {
        if run_in_subprocess(
            "native_frame_tests::native_cpp_refuses_same_isolate_host_body_reentry_across_stacks",
            Duration::from_secs(30),
        ) {
            nested_stack_proof(true);
        }
    }

    #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
    fn nested_stack_proof(cross_stack: bool) {
        use wasmer::FunctionEnv;
        unsafe extern "C" {
            fn snapi_bridge_native_current_stack_info(
                saved: *mut usize,
                budget: *mut usize,
                applied: *mut usize,
            ) -> i32;
        }
        fn stack_info() -> (i32, usize, usize, usize, Option<(usize, usize)>) {
            let (mut saved, mut budget, mut applied) = (0, 0, 0);
            let present = unsafe {
                snapi_bridge_native_current_stack_info(&mut saved, &mut budget, &mut applied)
            };
            let bounds = wasmer::sys::NativeEngineStackBounds::current()
                .map(|bounds| (bounds.usable_low, bounds.base));
            (present, saved, budget, applied, bounds)
        }
        #[derive(Default)]
        struct Nested {
            handles: Option<(Memory, Function, Function)>,
        }
        struct BodyDrop(Arc<AtomicUsize>);
        impl Drop for BodyDrop {
            fn drop(&mut self) {
                self.0.fetch_add(1, Ordering::SeqCst);
            }
        }
        // The same-stack sibling runs GC with both JS entries live. Across
        // stacks, admission must refuse before this inner script or GC runs.
        let guest = GUEST
            .replace(
                "    (table (export \"__indirect_function_table\") 5 funcref)",
                r#"    (import "napi_extension_wasmer_v0" "unofficial_napi_collect_garbage"
        (func $gc (param i32) (result i32)))
    (import "napi" "napi_get_and_clear_last_exception"
        (func $clear (param i32 i32) (result i32)))
    (table (export "__indirect_function_table") 6 funcref)"#,
            )
            .replace(
                "$release_callback $finalizer)",
                "$release_callback $finalizer $nested)",
            )
            .replace(
                r#"    (data (i32.const 64) "callback\00")"#,
                r#"    (data (i32.const 64) "callback\00")
    (data (i32.const 80) "nested\00")"#,
            )
            .replace(
                "    (func (export \"init_owned\")",
                r#"    (func $nested (param i32 i32) (result i32)
        global.get $env call $gc call $check
        i32.const 2 call $record i32.const 0)
    (func (export "install_nested")
        global.get $env i32.const 80 i32.const 6 i32.const 5 i32.const 0 i32.const 12
        call $function call $check
        global.get $env i32.const 8 i32.load i32.const 80 i32.const 12 i32.load
        call $set call $check)
    (func (export "clear_nested_exception") (result i32)
        global.get $env i32.const 56 call $clear)
    (func (export "init_owned")"#,
            );
        let observations = Arc::new(Mutex::new(Vec::new()));
        let statuses = Arc::new(Mutex::new(None));
        let entries = Arc::new(AtomicUsize::new(0));
        let polls = Arc::new(AtomicUsize::new(0));
        let drops = Arc::new(AtomicUsize::new(0));
        let mut store = Store::default();
        let environment = FunctionEnv::new(&mut store, Nested::default());
        let recorded = observations.clone();
        let body_status = statuses.clone();
        let calls = entries.clone();
        let body_polls = polls.clone();
        let body_drops = drops.clone();
        let record = if cross_stack {
            Function::new_with_env_async(
                &mut store,
                &environment,
                wasmer::FunctionType::new([wasmer::Type::I32], []),
                move |env, _| -> Pin<Box<dyn Future<Output = Result<Vec<Value>, RuntimeError>>>> {
                    let entry = calls.fetch_add(1, Ordering::SeqCst);
                    recorded.lock().unwrap().push(stack_info());
                    if entry != 0 {
                        return Box::pin(async { Ok(Vec::new()) });
                    }
                    let recorded = recorded.clone();
                    let status = body_status.clone();
                    let polls = body_polls.clone();
                    let body_drop = BodyDrop(body_drops.clone());
                    Box::pin(async move {
                        let _drop = body_drop;
                        let mut yielded = false;
                        std::future::poll_fn(|_| {
                            polls.fetch_add(1, Ordering::SeqCst);
                            if std::mem::replace(&mut yielded, true) {
                                Poll::Ready(())
                            } else {
                                Poll::Pending
                            }
                        })
                        .await;
                        // This is an actual host-future body, outside the import
                        // factory. The existing explicit same-Store loan reacquires
                        // a fresh guard; no ambient Store lookup or new root poll.
                        let mut guard = env.write().await;
                        let (memory, run, clear) =
                            guard.data_mut().handles.as_ref().unwrap().clone();
                        let script = "globalThis.nestedEntered = 1; nested(); throw new Error('nested-stack')";
                        memory.view(&guard).write(4096, script.as_bytes()).unwrap();
                        let inner = run.call(
                            &mut guard,
                            &[Value::I32(4096), Value::I32(script.len() as i32)],
                        );
                        // Even refused entry must preserve the parked parent's
                        // limit. Record only owned metadata before replaying it.
                        recorded.lock().unwrap().push(stack_info());
                        let inner = inner?;
                        let cleared = clear.call(&mut guard, &[])?;
                        *status.lock().unwrap() = Some((inner[0].i32(), cleared[0].i32()));
                        Ok(Vec::new())
                    })
                },
            )
        } else {
            Function::new_typed_with_env(
                &mut store,
                &environment,
                move |mut env: wasmer::FunctionEnvMut<Nested>,
                      _: i32|
                      -> Result<(), RuntimeError> {
                    let entry = calls.fetch_add(1, Ordering::SeqCst);
                    recorded.lock().unwrap().push(stack_info());
                    if entry != 0 {
                        return Ok(());
                    }
                    let (memory, run, clear) = env.data().handles.as_ref().unwrap().clone();
                    let script =
                        "globalThis.nestedEntered = 1; nested(); throw new Error('nested-stack')";
                    memory.view(&env).write(4096, script.as_bytes()).unwrap();
                    let inner = run.call(
                        &mut env,
                        &[Value::I32(4096), Value::I32(script.len() as i32)],
                    )?;
                    let cleared = clear.call(&mut env, &[])?;
                    *body_status.lock().unwrap() = Some((inner[0].i32(), cleared[0].i32()));
                    recorded.lock().unwrap().push(stack_info());
                    Ok(())
                },
            )
        };
        // Match configured_instance_with_context's module/import/configuration
        // order, using this sibling's two extra NAPI imports and callback.
        let ctx = NapiCtx::default();
        let module = Module::new(&store, wat::parse_str(&guest).unwrap()).unwrap();
        let hooks = ctx.runtime_hooks();
        let (mut imports, state) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .unwrap();
        let session = state.session().unwrap().clone();
        imports.define("test", "record", record);
        let Some(Extern::Memory(memory)) = imports.get_export("env", "memory") else {
            panic!("NAPI did not create the imported memory");
        };
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        hooks
            .configure_instance(
                &module,
                &mut store.as_store_mut(),
                &instance,
                Some(&memory),
                state,
            )
            .unwrap();
        environment.as_mut(&mut store).handles = Some((
            memory.clone(),
            instance
                .exports
                .get_function("eval_status")
                .unwrap()
                .clone(),
            instance
                .exports
                .get_function("clear_nested_exception")
                .unwrap()
                .clone(),
        ));
        let flags = b"--stack-size=512";
        let options: Vec<_> = [16_u32, 1, 256, flags.len() as u32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect();
        memory.view(&store).write(128, &options).unwrap();
        memory.view(&store).write(256, flags).unwrap();
        let configured = instance
            .exports
            .get_typed_function::<i32, i32>(&store, "configure_runtime_options")
            .unwrap()
            .call(&mut store, 128);
        instance
            .exports
            .get_typed_function::<(), ()>(&store, "init")
            .unwrap()
            .call(&mut store)
            .unwrap();
        instance
            .exports
            .get_typed_function::<(), ()>(&store, "install_nested")
            .unwrap()
            .call(&mut store)
            .unwrap();
        let script = "(() => { const live = { answer: 35, bytes: new Uint8Array([7,8,9]) }; try { callback(); throw live; } catch (error) { return error === live ? error.answer + error.bytes[0] : -1; } })()";
        memory.view(&store).write(1024, script.as_bytes()).unwrap();
        let run = instance.exports.get_function("eval").unwrap().clone();
        let scope = NativeInvocationScope::bind(&mut store).unwrap();
        let store = store.into_async();
        let mut call = Box::pin(run.call_async(
            &store,
            vec![Value::I32(1024), Value::I32(script.len() as i32)],
        ));
        let first = call.as_mut().poll(&mut Context::from_waker(Waker::noop()));
        let first_pending = first.is_pending();
        let mut result = first;
        for _ in 0..8 {
            if result.is_ready() {
                break;
            }
            result = call.as_mut().poll(&mut Context::from_waker(Waker::noop()));
        }
        drop(call);
        let (entered, fresh) = {
            let mut guard = store.store().write();
            let entered = super::eval(
                &mut guard,
                &instance,
                &memory,
                "typeof nestedEntered === 'undefined' ? 0 : nestedEntered",
            );
            let fresh = super::eval(&mut guard, &instance, &memory, "40 + 2");
            (entered, fresh)
        };
        {
            let mut guard = store.store().write();
            unsafe { session.deactivate(&mut guard.as_store_mut()) };
        }
        drop(scope);
        drop(instance);
        drop(imports);
        drop(module);
        drop(session);
        drop(store);
        assert_eq!(ctx.budget().snapshot().live_isolates, 0);
        assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
        assert_eq!(configured.unwrap(), 0);
        assert_eq!(first_pending, cross_stack);
        let Poll::Ready(result) = result else {
            panic!("the nested callback did not settle");
        };
        assert_eq!(fresh.unwrap(), 42);
        assert_eq!(entered.unwrap(), i32::from(!cross_stack));
        if cross_stack {
            let error = result.expect_err("same-isolate entry crossed physical stacks");
            assert!(error.to_string().contains("native engine stack bounds"));
            assert_eq!(
                entries.load(Ordering::SeqCst),
                1,
                "refused JS entered its callback"
            );
            assert_eq!(polls.load(Ordering::SeqCst), 2);
            assert_eq!(drops.load(Ordering::SeqCst), 1);
            assert!(statuses.lock().unwrap().is_none());
        } else {
            assert_eq!(result.unwrap()[0].i32(), Some(42));
            assert_eq!(entries.load(Ordering::SeqCst), 2);
            assert_eq!(polls.load(Ordering::SeqCst), 0);
            assert_eq!(drops.load(Ordering::SeqCst), 0);
            assert_eq!(*statuses.lock().unwrap(), Some((Some(10), Some(0))));
        }
        let observations = observations.lock().unwrap();
        assert_eq!(observations.len(), if cross_stack { 2 } else { 3 });
        let (present, saved, budget, parent, bounds) = observations[0];
        assert_eq!(present, 1);
        assert_eq!(budget, 512 * 1024);
        let (low, base) = bounds.expect("outer callback must be on its actual root stack");
        assert_eq!(parent, (low + 64 * 1024).max(base - budget));
        if !cross_stack {
            let (present, inner_saved, inner_budget, inner_applied, inner_bounds) = observations[1];
            assert_eq!(present, 1);
            assert_eq!(inner_bounds, bounds);
            assert_eq!(inner_budget, budget);
            assert_eq!(inner_saved, parent);
            assert_eq!(inner_applied, parent);
        }
        let (present, restored_saved, restored_budget, restored, restored_bounds) =
            *observations.last().unwrap();
        assert_eq!(present, 1);
        assert_eq!(restored_bounds, if cross_stack { None } else { bounds });
        assert_eq!(restored_saved, saved);
        assert_eq!(restored_budget, budget);
        assert_eq!(
            restored, parent,
            "nested entry lost the parent's exact limit"
        );
    }

    #[cfg(all(
        feature = "native-stack-diagnostics",
        target_os = "linux",
        target_arch = "x86_64",
        target_env = "gnu",
        target_pointer_width = "64"
    ))]
    mod scanner_proofs {
        use super::*;
        use std::{
            any::Any,
            panic::{AssertUnwindSafe, catch_unwind, resume_unwind},
        };

        unsafe extern "C" {
            fn snapi_bridge_native_probe_begin() -> i32;
            fn snapi_bridge_native_probe_end(out: *mut u64);
            fn snapi_bridge_native_probe_with_register(
                operation: unsafe extern "C" fn(*mut std::ffi::c_void),
                context: *mut std::ffi::c_void,
                ancestor: i32,
            );
        }

        // The actual patterns are encoded only in the CPP assembly. Rust owns
        // expected bits and ordinary outcomes, so a stack copy cannot stand in
        // for the external assembly register roots under test.
        fn with_register<R, F: FnOnce() -> R>(ancestor: bool, operation: F) -> R {
            struct Call<F, R> {
                operation: Option<F>,
                result: Option<Result<R, Box<dyn Any + Send>>>,
            }
            unsafe extern "C" fn invoke<F: FnOnce() -> R, R>(raw: *mut std::ffi::c_void) {
                let call = unsafe { &mut *raw.cast::<Call<F, R>>() };
                let operation = call.operation.take().unwrap();
                call.result = Some(catch_unwind(AssertUnwindSafe(operation)));
            }
            let mut call = Call {
                operation: Some(operation),
                result: None,
            };
            unsafe {
                snapi_bridge_native_probe_with_register(
                    invoke::<F, R>,
                    (&mut call as *mut Call<F, R>).cast(),
                    i32::from(ancestor),
                );
            }
            match call.result.unwrap() {
                Ok(value) => value,
                Err(panic) => resume_unwind(panic),
            }
        }

        struct ScanProbe;
        impl ScanProbe {
            fn begin() -> Self {
                assert_eq!(unsafe { snapi_bridge_native_probe_begin() }, 1);
                Self
            }
            fn finish(self) -> [u64; 5] {
                let mut counters = [0; 5];
                unsafe { snapi_bridge_native_probe_end(counters.as_mut_ptr()) };
                std::mem::forget(self);
                counters
            }
        }
        impl Drop for ScanProbe {
            fn drop(&mut self) {
                let mut ignored = [0; 5];
                unsafe { snapi_bridge_native_probe_end(ignored.as_mut_ptr()) };
            }
        }

        fn guest() -> String {
            GUEST
                .replace(
                    "    (table (export \"__indirect_function_table\") 5 funcref)",
                    r#"    (import "test" "wait" (func $wait))
    (import "napi_extension_wasmer_v0" "unofficial_napi_collect_garbage"
        (func $gc (param i32) (result i32)))
    (table (export "__indirect_function_table") 5 funcref)"#,
                )
                .replace(
                    "    (func (export \"fresh\")",
                    r#"    (func (export "collect") global.get $env call $gc call $check)
    (func (export "wait") call $wait)
    (func (export "fresh")"#,
                )
        }

        // Same construction/configuration order as the existing GUEST sibling.
        fn configured(
            store: &mut Store,
            record: Function,
            wait: Function,
        ) -> (NapiCtx, Instance, Memory, NapiSession) {
            let ctx = NapiCtx::default();
            let module = Module::new(&*store, wat::parse_str(guest()).unwrap()).unwrap();
            let hooks = ctx.runtime_hooks();
            let (mut imports, state) = hooks
                .additional_imports(&module, &mut store.as_store_mut())
                .unwrap();
            let session = state.session().unwrap().clone();
            imports.define("test", "record", record);
            imports.define("test", "wait", wait);
            let Some(Extern::Memory(memory)) = imports.get_export("env", "memory") else {
                panic!("NAPI did not create its imported memory");
            };
            let instance = Instance::new(store, &module, &imports).unwrap();
            hooks
                .configure_instance(
                    &module,
                    &mut store.as_store_mut(),
                    &instance,
                    Some(&memory),
                    state,
                )
                .unwrap();
            (ctx, instance, memory, session)
        }

        fn poll_ready<F: Future>(future: F) -> F::Output {
            let mut future = Box::pin(future);
            for _ in 0..8 {
                if let Poll::Ready(result) = future
                    .as_mut()
                    .poll(&mut Context::from_waker(Waker::noop()))
                {
                    return result;
                }
            }
            panic!("a native GC proof remained pending");
        }

        fn dispose(store: &mut Store, session: &NapiSession) {
            unsafe { session.deactivate(&mut store.as_store_mut()) };
        }

        #[test]
        fn allocation_gc_preserves_current_register_os_and_live_js_roots() {
            if !run_in_subprocess(
                "native_frame_tests::scanner_proofs::allocation_gc_preserves_current_register_os_and_live_js_roots",
                Duration::from_secs(45),
            ) {
                return;
            }
            let mut store = Store::default();
            let record = Function::new_typed(&mut store, |_: i32| {});
            let wait = Function::new_typed(&mut store, || {});
            let (ctx, instance, memory, session) = configured(&mut store, record, wait);
            let flags = b"--max-old-space-size=32";
            let options: Vec<_> = [16_u32, 1, 256, flags.len() as u32]
                .into_iter()
                .flat_map(u32::to_le_bytes)
                .collect();
            memory.view(&store).write(128, &options).unwrap();
            memory.view(&store).write(256, flags).unwrap();
            let configured = instance
                .exports
                .get_typed_function::<i32, i32>(&store, "configure_runtime_options")
                .unwrap()
                .call(&mut store, 128);
            let init = instance.exports.get_function("init").unwrap().clone();
            let run = instance.exports.get_function("eval").unwrap().clone();
            let scope = NativeInvocationScope::bind(&mut store).unwrap();
            let store = store.into_async();
            let initialized = with_register(false, || poll_ready(init.call_async(&store, vec![])));
            // No explicit GC call: bounded live sets repeatedly allocate far
            // beyond the configured old-space limit and must trigger real GC.
            let script = "(() => { const live = { answer: 35, bytes: new Uint8Array([7,8,9]) }; for (let i=0; i<96; i++) { const junk = []; for (let j=0; j<16; j++) junk.push(new Array(32768).fill(i)); } try { throw live; } catch (error) { return error === live ? error.answer + error.bytes[0] : -1; } })()";
            {
                let guard = store.store().write();
                memory.view(&guard).write(1024, script.as_bytes()).unwrap();
            }
            let probe = ScanProbe::begin();
            let result = with_register(false, || {
                poll_ready(run.call_async(
                    &store,
                    vec![Value::I32(1024), Value::I32(script.len() as i32)],
                ))
            });
            let counters = probe.finish();
            let fresh = {
                let mut guard = store.store().write();
                let result = super::super::eval(&mut guard, &instance, &memory, "40 + 2");
                unsafe { session.deactivate(&mut guard.as_store_mut()) };
                result
            };
            drop(scope);
            drop(instance);
            drop(session);
            drop(store);
            assert_eq!(ctx.budget().snapshot().live_isolates, 0);
            assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
            assert_eq!(configured.unwrap(), 0);
            initialized.unwrap();
            assert_eq!(result.unwrap()[0].i32(), Some(42));
            assert_eq!(fresh.unwrap(), 42);
            assert_eq!(
                counters[0] & 5,
                5,
                "original visitor missed current-register or OS roots"
            );
            assert!(
                counters[1] > 0 && counters[3] > 0,
                "allocation did not run the actual native GC scanner"
            );
            assert_eq!(counters[4], 0);
        }

        #[test]
        fn independent_gc_scans_the_retained_native_ancestor() {
            if !run_in_subprocess(
                "native_frame_tests::scanner_proofs::independent_gc_scans_the_retained_native_ancestor",
                Duration::from_secs(45),
            ) {
                return;
            }
            let mut store = Store::default();
            let calls = Arc::new(AtomicUsize::new(0));
            let observed_calls = calls.clone();
            let inner = Arc::new(Mutex::new(None));
            let observed_inner = inner.clone();
            let wait =
                Function::new_async(&mut store, wasmer::FunctionType::new([], []), move |_| {
                    let observed_calls = observed_calls.clone();
                    let observed_inner = observed_inner.clone();
                    async move {
                        let mut yielded = false;
                        std::future::poll_fn(|_| {
                            observed_calls.fetch_add(1, Ordering::SeqCst);
                            if std::mem::replace(&mut yielded, true) {
                                Poll::Ready(())
                            } else {
                                Poll::Pending
                            }
                        })
                        .await;
                        // A is genuinely parked. B owns a different Store/isolate,
                        // enters on OS, collects while A's CPP/JS frames stay live,
                        // and retires fully before A resumes.
                        let mut other = Store::default();
                        let record = Function::new_typed(&mut other, |_: i32| {});
                        let wait = Function::new_typed(&mut other, || {});
                        let (ctx, instance, memory, session) = configured(&mut other, record, wait);
                        instance
                            .exports
                            .get_typed_function::<(), ()>(&other, "init")
                            .unwrap()
                            .call(&mut other)
                            .unwrap();
                        let before = super::super::eval(
                            &mut other,
                            &instance,
                            &memory,
                            "globalThis.live = new Uint8Array([42]); 42",
                        );
                        let collected = instance
                            .exports
                            .get_typed_function::<(), ()>(&other, "collect")
                            .unwrap()
                            .call(&mut other);
                        let after = super::super::eval(&mut other, &instance, &memory, "live[0]");
                        dispose(&mut other, &session);
                        drop(instance);
                        drop(session);
                        drop(other);
                        *observed_inner.lock().unwrap() =
                            Some((before, collected, after, ctx.budget().snapshot()));
                        Ok(Vec::new())
                    }
                });
            let environment = wasmer::FunctionEnv::new(&mut store, None::<Function>);
            let record = Function::new_typed_with_env(
                &mut store,
                &environment,
                |mut env: wasmer::FunctionEnvMut<Option<Function>>,
                 _: i32|
                 -> Result<(), RuntimeError> {
                    let wait = env.data().as_ref().unwrap().clone();
                    with_register(true, || wait.call(&mut env, &[])).map(|_| ())
                },
            );
            let (ctx, instance, memory, session) = configured(&mut store, record, wait);
            environment
                .as_mut(&mut store)
                .replace(instance.exports.get_function("wait").unwrap().clone());
            instance
                .exports
                .get_typed_function::<(), ()>(&store, "init")
                .unwrap()
                .call(&mut store)
                .unwrap();
            let script = "(() => { const live = { answer: 35, bytes: new Uint8Array([7,8,9]) }; callback(); try { throw live; } catch (error) { return error === live ? error.answer + error.bytes[0] : -1; } })()";
            memory.view(&store).write(1024, script.as_bytes()).unwrap();
            let run = instance.exports.get_function("eval").unwrap().clone();
            let scope = NativeInvocationScope::bind(&mut store).unwrap();
            let store = store.into_async();
            let mut call = Box::pin(run.call_async(
                &store,
                vec![Value::I32(1024), Value::I32(script.len() as i32)],
            ));
            let probe = ScanProbe::begin();
            let first = with_register(false, || {
                call.as_mut().poll(&mut Context::from_waker(Waker::noop()))
            });
            let pending = first.is_pending();
            let mut result = first;
            for _ in 0..8 {
                if result.is_ready() {
                    break;
                }
                result = with_register(false, || {
                    call.as_mut().poll(&mut Context::from_waker(Waker::noop()))
                });
            }
            drop(call);
            let counters = probe.finish();
            let fresh = {
                let mut guard = store.store().write();
                let value = super::super::eval(&mut guard, &instance, &memory, "40 + 2");
                unsafe { session.deactivate(&mut guard.as_store_mut()) };
                value
            };
            drop(scope);
            drop(instance);
            drop(session);
            drop(store);
            let inner = inner.lock().unwrap().take();
            assert_eq!(ctx.budget().snapshot().live_isolates, 0);
            assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
            assert!(pending, "ancestor never parked");
            let Poll::Ready(result) = result else {
                panic!("ancestor did not settle");
            };
            assert_eq!(result.unwrap()[0].i32(), Some(42));
            assert_eq!(fresh.unwrap(), 42);
            assert_eq!(calls.load(Ordering::SeqCst), 2);
            let (before, collected, after, usage) =
                inner.expect("independent provider never collected");
            assert_eq!(before.unwrap(), 42);
            collected.unwrap();
            assert_eq!(after.unwrap(), 42);
            assert_eq!(usage.live_isolates, 0);
            assert_eq!(usage.wasm_linear, 0);
            assert_eq!(
                counters[0] & 3,
                3,
                "original visitor missed current-register or retained ancestor roots"
            );
            assert!(counters[1] > 0 && counters[2] > 0);
            assert_eq!(counters[4], 0);
        }
    }

    thread_local! {
        static LOGICAL_DROP: std::cell::RefCell<Option<Box<dyn FnOnce()>>> = const { std::cell::RefCell::new(None) };
    }
    fn deferred_causal_cleanup_proof() {
        use std::sync::atomic::AtomicU64;
        use wasmer::sys::NativeEngineExt;
        struct CleanupAccountant {
            charged: AtomicU64,
            armed: AtomicBool,
            panics: Arc<AtomicUsize>,
            payload: Arc<()>,
        }
        impl wasmer_napi::NapiMemoryAccountant for CleanupAccountant {
            fn memory_limit(&self) -> u64 {
                512 * 1024 * 1024
            }
            fn memory_charged(&self) -> u64 {
                self.charged.load(Ordering::SeqCst)
            }
            fn try_charge(&self, bytes: u64) -> bool {
                self.charged.fetch_add(bytes, Ordering::SeqCst);
                true
            }
            fn uncharge(&self, bytes: u64) {
                self.charged.fetch_sub(bytes, Ordering::SeqCst);
                if self.armed.swap(false, Ordering::SeqCst) {
                    self.panics.fetch_add(1, Ordering::SeqCst);
                    std::panic::panic_any(self.payload.clone());
                }
            }
        }
        let panics = Arc::new(AtomicUsize::new(0));
        let original = Arc::new(());
        let other_accountant = Arc::new(CleanupAccountant {
            charged: AtomicU64::new(0),
            armed: AtomicBool::new(false),
            panics: panics.clone(),
            payload: original.clone(),
        });
        let other_ctx = NapiCtx::builder()
            .memory_accountant(other_accountant.clone())
            .build();
        let mut other = Store::default();
        let (other_ctx, other_instance, _other_memory, other_session) =
            instance_with_context(&mut other, other_ctx, |store, _| {
                Function::new_typed(store, |_: i32| {})
            });
        LOGICAL_DROP.with(|slot| {
            *slot.borrow_mut() = Some(Box::new(move || {
                drop(other_instance);
                drop(other_session);
                drop(other);
            }))
        });
        let accountant = Arc::new(ReentrantAccountant(AtomicU64::new(0)));
        let ctx = NapiCtx::builder()
            .memory_accountant(accountant.clone())
            .build();
        let mut store = Store::default();
        store
            .engine_mut()
            .set_tunables(wasmer_napi::budgeted_tunables(ctx.budget()));
        let (ctx, instance, memory, session) =
            instance_with_context(&mut store, ctx, |store, _| {
                Function::new_typed(store, |_: i32| {})
            });
        let child_done = Arc::new(AtomicBool::new(false));
        let observed = child_done.clone();
        other_accountant.armed.store(true, Ordering::SeqCst);
        EXTERNAL_ACCOUNTANT_HOOK.with(|slot| {
            *slot.borrow_mut() = Some(Box::new(move || {
                let cleanup = LOGICAL_DROP.with(|slot| slot.borrow_mut().take()).unwrap();
                cleanup();
                observed.store(true, Ordering::SeqCst);
            }))
        });
        let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            super::eval(
                &mut store,
                &instance,
                &memory,
                "globalThis.bytes = new Uint8Array(32 * 1024 * 1024); 42",
            )
        }));
        // Even a failed setup must release its independent Store here, rather
        // than leave a panicking owner for thread-local process teardown.
        let leftover = LOGICAL_DROP.with(|slot| slot.borrow_mut().take());
        let leftover_present = leftover.is_some();
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| drop(leftover)));
        let fresh = super::eval(&mut store, &instance, &memory, "40 + 2").unwrap();
        unsafe {
            session.deactivate(&mut store.as_store_mut());
        }
        drop(instance);
        drop(session);
        drop(store);
        assert!(!leftover_present, "allocator cleanup hook did not run");
        assert!(child_done.load(Ordering::SeqCst));
        assert_eq!(panics.load(Ordering::SeqCst), 1);
        assert_eq!(other_ctx.budget().snapshot().live_isolates, 0);
        assert_eq!(other_accountant.charged.load(Ordering::SeqCst), 0);
        assert_eq!(ctx.budget().snapshot().live_isolates, 0);
        assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
        assert_eq!(fresh, 42);
        assert!(Arc::ptr_eq(
            outcome
                .expect_err("causal deferred cleanup failure was lost")
                .downcast_ref::<Arc<()>>()
                .unwrap(),
            &original
        ));
    }
    #[test]
    fn deferred_causal_cleanup_preserves_the_initiating_import_failure() {
        if run_in_subprocess(
            "native_frame_tests::deferred_causal_cleanup_preserves_the_initiating_import_failure",
            Duration::from_secs(30),
        ) {
            deferred_causal_cleanup_proof();
        }
    }
}

#[test]
fn native_allocator_defers_independent_store_retirement_until_cpp_exit() {
    if !run_in_subprocess(
        "native_allocator_defers_independent_store_retirement_until_cpp_exit",
        Duration::from_secs(30),
    ) {
        return;
    }
    use std::sync::atomic::{AtomicU64, Ordering};
    use wasmer::sys::NativeEngineExt;
    let mut other = Store::default();
    let (other_ctx, other_instance, _other_memory, other_session) =
        instance(&mut other, |_| Ok(()));
    other_instance
        .exports
        .get_typed_function::<(), ()>(&other, "init_owned")
        .unwrap()
        .call(&mut other)
        .unwrap();
    assert_eq!(other_ctx.budget().snapshot().live_isolates, 2);
    let accountant = Arc::new(ReentrantAccountant(AtomicU64::new(0)));
    let ctx = NapiCtx::builder()
        .memory_accountant(accountant.clone())
        .build();
    let mut store = Store::default();
    store
        .engine_mut()
        .set_tunables(wasmer_napi::budgeted_tunables(ctx.budget()));
    let (ctx, instance, memory, session) = instance_with_context(&mut store, ctx, |store, _| {
        Function::new_typed(store, |_: i32| {})
    });
    let operation = Arc::new(AtomicU64::new(u64::MAX));
    let release = Arc::new(AtomicU64::new(u64::MAX));
    let retained = Arc::new(AtomicU64::new(u64::MAX));
    let observed_operation = operation.clone();
    let observed_release = release.clone();
    let observed_retained = retained.clone();
    let other_budget = other_ctx.budget();
    let during_budget = other_budget.clone();
    EXTERNAL_ACCOUNTANT_HOOK.with(|slot| {
        *slot.borrow_mut() = Some(Box::new(move || {
            // Pure Wasm ready work remains supported in an allocator callback.
            assert_eq!(
                other_instance
                    .exports
                    .get_typed_function::<(), i32>(&other, "fresh")
                    .unwrap()
                    .call(&mut other)
                    .unwrap(),
                9
            );
            // V8 entry and explicit release must reject before provider/logical
            // state changes. These are real ordinary public session imports.
            observed_operation.store(
                other_instance
                    .exports
                    .get_typed_function::<(), i32>(&other, "global_status")
                    .unwrap()
                    .call(&mut other)
                    .unwrap() as u64,
                Ordering::SeqCst,
            );
            observed_release.store(
                other_instance
                    .exports
                    .get_typed_function::<(), i32>(&other, "release_owned")
                    .unwrap()
                    .call(&mut other)
                    .unwrap() as u64,
                Ordering::SeqCst,
            );
            drop(other_instance);
            drop(other_session);
            drop(other);
            observed_retained.store(
                during_budget.snapshot().live_isolates as u64,
                Ordering::SeqCst,
            );
        }));
    });
    let value = eval(
        &mut store,
        &instance,
        &memory,
        "globalThis.bytes = new Uint8Array(32 * 1024 * 1024); 42",
    )
    .unwrap();
    let after = other_budget.snapshot();
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    drop(instance);
    drop(session);
    drop(store);
    assert_eq!(value, 42);
    assert_eq!(
        operation.load(Ordering::SeqCst),
        1,
        "allocator reentered V8"
    );
    assert_eq!(
        release.load(Ordering::SeqCst),
        1,
        "allocator consumed explicit release"
    );
    assert_eq!(
        retained.load(Ordering::SeqCst),
        2,
        "provider was disposed inside allocator"
    );
    assert_eq!(
        after.live_isolates, 0,
        "physical CPP exit did not drain the owned plan"
    );
    assert_eq!(after.wasm_linear, 0);
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
    assert_eq!(
        wasmer_napi::NapiMemoryAccountant::memory_charged(&*accountant),
        0
    );
}

#[test]
fn native_allocator_panic_preserves_first_payload_and_stops_caught_js() {
    if !run_in_subprocess(
        "native_allocator_panic_preserves_first_payload_and_stops_caught_js",
        Duration::from_secs(30),
    ) {
        return;
    }
    use std::sync::atomic::{AtomicU64, Ordering};
    use wasmer::sys::NativeEngineExt;
    for typed in [false, true] {
        let accountant = Arc::new(ReentrantAccountant(AtomicU64::new(0)));
        let ctx = NapiCtx::builder()
            .memory_accountant(accountant.clone())
            .build();
        let mut store = Store::default();
        store
            .engine_mut()
            .set_tunables(wasmer_napi::budgeted_tunables(ctx.budget()));
        let callbacks = Arc::new(AtomicU64::new(0));
        let observed_callbacks = callbacks.clone();
        let (ctx, instance, memory, session) =
            instance_with_context(&mut store, ctx, move |store, _| {
                Function::new_typed(store, move |_: i32| {
                    observed_callbacks.fetch_add(1, Ordering::SeqCst);
                })
            });
        let original = Arc::new(());
        let panic = original.clone();
        EXTERNAL_ACCOUNTANT_HOOK
            .with(|slot| *slot.borrow_mut() = Some(Box::new(move || std::panic::panic_any(panic))));
        let script = "try { new Uint8Array(32 * 1024 * 1024); } catch (_) {} callback(); while (true) {} globalThis.nativeAfter = 1; 42";
        memory.view(&store).write(1024, script.as_bytes()).unwrap();
        let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            if typed {
                instance
                    .exports
                    .get_typed_function::<(i32, i32), i32>(&store, "eval")
                    .unwrap()
                    .call(&mut store, 1024, script.len() as i32)
                    .map(|value| vec![Value::I32(value)])
            } else {
                instance
                    .exports
                    .get_function("eval")
                    .unwrap()
                    .call(
                        &mut store,
                        &[Value::I32(1024), Value::I32(script.len() as i32)],
                    )
                    .map(|values| values.to_vec())
            }
        }));
        let after = eval(
            &mut store,
            &instance,
            &memory,
            "typeof nativeAfter === 'undefined' ? 0 : nativeAfter",
        )
        .unwrap();
        let fresh = eval(&mut store, &instance, &memory, "40 + 2").unwrap();
        unsafe {
            session.deactivate(&mut store.as_store_mut());
        }
        drop(instance);
        drop(session);
        drop(store);
        assert_eq!(fresh, 42);
        assert_eq!(ctx.budget().snapshot().live_isolates, 0);
        assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
        assert_eq!(
            wasmer_napi::NapiMemoryAccountant::memory_charged(&*accountant),
            0
        );
        assert!(Arc::ptr_eq(
            outcome.unwrap_err().downcast_ref::<Arc<()>>().unwrap(),
            &original
        ));
        assert_eq!(
            callbacks.load(Ordering::SeqCst),
            0,
            "failed frame admitted another native callback"
        );
        assert_eq!(after, 0, "JS escaped the termination backedge");
    }
}

#[test]
fn native_external_buffers_reject_nonresident_guest_spans() {
    if !run_in_subprocess(
        "native_external_buffers_reject_nonresident_guest_spans",
        Duration::from_secs(20),
    ) {
        return;
    }
    let mut store = Store::default();
    let (ctx, instance, memory, session) = instance(&mut store, |_| Ok(()));
    let size = memory.view(&store).data_size();
    for export in ["external_array", "external_buffer"] {
        let create = instance
            .exports
            .get_typed_function::<(i32, i32), i32>(&store, export)
            .unwrap();
        for (offset, length) in [(size as i32 - 8, 16), (-1, 16), (64, -1)] {
            assert_eq!(create.call(&mut store, offset, length).unwrap(), 1);
        }
        // Real provider admission still accepts the ordinary valid prefix.
        assert_eq!(create.call(&mut store, 64, 8).unwrap(), 0);
    }
    let fresh = eval(&mut store, &instance, &memory, "40 + 2").unwrap();
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    drop(instance);
    drop(session);
    drop(store);
    assert_eq!(fresh, 42);
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
}

#[test]
fn native_retirement_does_not_resurrect_js_stopped_before_finalizer_entry() {
    if !run_in_subprocess(
        "native_retirement_does_not_resurrect_js_stopped_before_finalizer_entry",
        Duration::from_secs(20),
    ) {
        return;
    }
    let mut store = Store::default();
    let seen = Arc::new(Mutex::new(Vec::new()));
    let observed = seen.clone();
    let (ctx, instance, memory, session) = instance(&mut store, move |code| {
        observed.lock().unwrap().push(code);
        Ok(())
    });
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init_owned")
        .unwrap()
        .call(&mut store)
        .unwrap();
    let script = "callback(); 42";
    memory.view(&store).write(1024, script.as_bytes()).unwrap();
    instance
        .exports
        .get_typed_function::<(i32, i32), ()>(&store, "arm_finalizer")
        .unwrap()
        .call(&mut store, script.len() as i32, 0)
        .unwrap();
    ctx.runtime_control().terminate_all();
    let release = instance
        .exports
        .get_typed_function::<(), i32>(&store, "release_owned")
        .unwrap()
        .call(&mut store)
        .unwrap();
    let status = instance
        .exports
        .get_typed_function::<(), i32>(&store, "finalizer_status")
        .unwrap()
        .call(&mut store)
        .unwrap();
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
    drop(instance);
    drop(session);
    drop(store);
    assert_eq!(release, 0);
    // The explicit Wasm finalizer still runs. Its nested JS cannot execute a
    // newly entered callback after vendor teardown clears termination flags.
    assert_eq!(*seen.lock().unwrap(), vec![3]);
    assert_eq!(status, 10);
    assert_eq!(ctx.budget().snapshot().live_isolates, 0);
    assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
}
