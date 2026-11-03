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
    (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
        (func $create_env (param i32 i32 i32 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
        (func $release_env (param i32 i32) (result i32)))
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
    instance
        .exports
        .get_typed_function::<(), ()>(store, "init")
        .unwrap()
        .call(store)
        .unwrap();
    (ctx, instance, memory, session)
}

fn eval(
    store: &mut Store,
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
