use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
};

use wasmer::{AsStoreMut, Function, Instance, Module, RuntimeError, Store};
use wasmer_napi::NapiCtx;
use wasmer_wasix::{WasiError, wasmer_wasix_types::wasi::ExitCode};

const MIB: u64 = 1024 * 1024;

#[test]
fn proxy_and_getter_exits_cross_current_and_legacy_boundaries() {
    let wat = r#"(module
      (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
      (import "napi" "napi_create_function" (func $function (param i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_get_global" (func $global (param i32 i32) (result i32)))
      (import "napi" "napi_set_named_property" (func $set (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
      (import "napi" "unofficial_napi_contextify_make_context" (func $make_legacy
        (param i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "unofficial_napi_get_own_non_index_properties" (func $own_legacy
        (param i32 i32 i32 i32) (result i32)))
      (import "napi" "unofficial_napi_contextify_contains_module_syntax" (func $syntax_legacy
        (param i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_contextify_make_context" (func $make_current
        (param i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_get_own_non_index_properties" (func $own_current
        (param i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_contextify_contains_module_syntax" (func $syntax_current
        (param i32 i32 i32 i32 i32 i32) (result i32)))
      (import "wasi_snapshot_preview1" "proc_exit" (func $exit (param i32)))
      (import "test" "observe" (func $observe))
      (memory (export "memory") 1)
      (table (export "__indirect_function_table") 1 funcref)
      (elem (i32.const 0) $getter)
      (data (i32.const 100) "exitNow\00")
      (data (i32.const 150) "Object.defineProperty({}, 'x', { enumerable: true, get() { return exitNow() } })\00")
      (data (i32.const 260) "new Proxy({}, { ownKeys() { exitNow(); return []; } })\00")
      (data (i32.const 380) "({ toString() { exitNow(); return 'x'; } })\00")
      (data (i32.const 500) "ctx\00")
      (func $getter (param i32 i32) (result i32)
        (call $observe)
        (call $exit (i32.const 31))
        (i32.const 0))
      (func (export "run") (param $mode i32) (result i32)
        (local $env i32) (local $kind i32) (local $source i32)
        (if (call $create (i32.const 8) (i32.const 4) (i32.const 8))
          (then (return (i32.const 1))))
        (local.set $env (i32.load (i32.const 4)))
        (if (call $function (local.get $env) (i32.const 100) (i32.const -1)
                            (i32.const 0) (i32.const 0) (i32.const 12))
          (then (return (i32.const 2))))
        (if (call $global (local.get $env) (i32.const 16))
          (then (return (i32.const 3))))
        (if (call $set (local.get $env) (i32.load (i32.const 16))
                       (i32.const 100) (i32.load (i32.const 12)))
          (then (return (i32.const 4))))
        (local.set $kind (i32.rem_u (local.get $mode) (i32.const 3)))
        (local.set $source (i32.const 150))
        (if (i32.eq (local.get $kind) (i32.const 1))
          (then (local.set $source (i32.const 260))))
        (if (i32.eq (local.get $kind) (i32.const 2))
          (then (local.set $source (i32.const 380))))
        (if (call $string (local.get $env) (local.get $source) (i32.const -1) (i32.const 20))
          (then (return (i32.const 5))))
        (if (call $script (local.get $env) (i32.load (i32.const 20)) (i32.const 24))
          (then (return (i32.const 6))))
        (if (call $string (local.get $env) (i32.const 500) (i32.const -1) (i32.const 28))
          (then (return (i32.const 7))))
        (if (result i32) (i32.lt_u (local.get $mode) (i32.const 3))
          (then (if (result i32) (i32.eqz (local.get $kind))
            (then (call $make_legacy (local.get $env) (i32.load (i32.const 24))
              (i32.load (i32.const 28)) (i32.const 0) (i32.const 1) (i32.const 1)
              (i32.const 0) (i32.const 0) (i32.const 32)))
            (else (if (result i32) (i32.eq (local.get $kind) (i32.const 1))
              (then (call $own_legacy (local.get $env) (i32.load (i32.const 24))
                (i32.const 0) (i32.const 32)))
              (else (call $syntax_legacy (local.get $env) (i32.load (i32.const 24))
                (i32.load (i32.const 28)) (i32.const 0) (i32.const 0)
                (i32.const 32)))))))
          (else (if (result i32) (i32.eqz (local.get $kind))
            (then (call $make_current (local.get $env) (i32.load (i32.const 24))
              (i32.load (i32.const 28)) (i32.const 0) (i32.const 1) (i32.const 1)
              (i32.const 0) (i32.const 0) (i32.const 32)))
            (else (if (result i32) (i32.eq (local.get $kind) (i32.const 1))
              (then (call $own_current (local.get $env) (i32.load (i32.const 24))
                (i32.const 0) (i32.const 32)))
              (else (call $syntax_current (local.get $env) (i32.load (i32.const 24))
                (i32.load (i32.const 28)) (i32.const 0) (i32.const 0)
                (i32.const 32))))))))))"#;

    for mode in 0..6 {
        let ctx = NapiCtx::builder().total_memory_bytes(160 * MIB).build();
        let budget = ctx.budget();
        let observed = Arc::new(AtomicU64::new(0));
        let mut store = Store::default();
        let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
        let session = ctx.new_session(&module).unwrap();
        let mut imports = session.create_imports(&mut store.as_store_mut()).unwrap();
        imports.define(
            "wasi_snapshot_preview1",
            "proc_exit",
            Function::new_typed(&mut store, |code: i32| -> Result<(), RuntimeError> {
                Err(RuntimeError::user(Box::new(WasiError::Exit(
                    ExitCode::from(code),
                ))))
            }),
        );
        let observed_for_hook = Arc::clone(&observed);
        let budget_for_hook = Arc::clone(&budget);
        imports.define(
            "test",
            "observe",
            Function::new_typed(&mut store, move || {
                observed_for_hook
                    .store(budget_for_hook.snapshot().host_transient, Ordering::Relaxed);
            }),
        );
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        session
            .configure_instance(&mut store.as_store_mut(), &instance, None)
            .unwrap();
        let run = instance
            .exports
            .get_typed_function::<i32, i32>(&store, "run")
            .unwrap();
        let error = run.call(&mut store, mode).unwrap_err();
        assert!(
            matches!(
                error.downcast_ref::<WasiError>(),
                Some(WasiError::Exit(code)) if *code == ExitCode::from(31)
            ),
            "mode {mode}: {error}"
        );
        if mode % 3 == 0 {
            assert_eq!(observed.load(Ordering::Relaxed), 2 * MIB);
        }
        assert_eq!(budget.snapshot().host_transient, 0, "mode {mode}");
    }
}

#[test]
fn nested_compile_reservations_enforce_finite_budget_and_unwind() {
    let wat = r#"(module
      (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
      (import "napi" "napi_create_function" (func $function (param i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_get_global" (func $global (param i32 i32) (result i32)))
      (import "napi" "napi_set_named_property" (func $set (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
      (import "napi" "unofficial_napi_contextify_compile_function" (func $compile_legacy
        (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_contextify_compile_function" (func $compile_current
        (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
      (import "test" "observe" (func $observe))
      (memory (export "memory") 1)
      (table (export "__indirect_function_table") 1 funcref)
      (elem (i32.const 0) $getter)
      (global $mode (mut i32) (i32.const 0))
      (global $calls (mut i32) (i32.const 0))
      (global $denials (mut i32) (i32.const 0))
      (data (i32.const 100) "getParam\00")
      (data (i32.const 150) "Object.defineProperty(['x'], '0', {get(){return getParam()}})\00")
      (data (i32.const 250) "['x']\00")
      (data (i32.const 300) "return x;\00")
      (data (i32.const 320) "compile.js\00")
      (data (i32.const 350) "x\00")
      (func $compile (result i32)
        (if (result i32) (i32.eqz (global.get $mode))
          (then (call $compile_legacy (i32.load (i32.const 4))
            (i32.load (i32.const 32)) (i32.load (i32.const 28))
            (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 0)
            (i32.const 0) (i32.const 0) (i32.load (i32.const 24))
            (i32.const 0) (i32.const 44)))
          (else (call $compile_current (i32.load (i32.const 4))
            (i32.const 400) (i32.load (i32.const 28))
            (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 0)
            (i32.load (i32.const 24)) (i32.const 0) (i32.const 44)))))
      (func $getter (param i32 i32) (result i32)
        (global.set $calls (i32.add (global.get $calls) (i32.const 1)))
        (call $observe)
        (if (call $compile)
          (then (global.set $denials
            (i32.add (global.get $denials) (i32.const 1)))))
        (i32.load (i32.const 36)))
      (func (export "calls") (result i32) (global.get $calls))
      (func (export "denials") (result i32) (global.get $denials))
      (func (export "run") (param $mode_arg i32) (param $recursive i32) (result i32)
        (local $env i32) (local $params_source i32)
        (global.set $mode (local.get $mode_arg))
        (if (call $create (i32.const 8) (i32.const 4) (i32.const 8))
          (then (return (i32.const 1))))
        (local.set $env (i32.load (i32.const 4)))
        (if (call $function (local.get $env) (i32.const 100) (i32.const -1)
                            (i32.const 0) (i32.const 0) (i32.const 12))
          (then (return (i32.const 2))))
        (if (call $global (local.get $env) (i32.const 16))
          (then (return (i32.const 3))))
        (if (call $set (local.get $env) (i32.load (i32.const 16))
                       (i32.const 100) (i32.load (i32.const 12)))
          (then (return (i32.const 4))))
        (local.set $params_source (i32.const 250))
        (if (local.get $recursive)
          (then (local.set $params_source (i32.const 150))))
        (if (call $string (local.get $env) (local.get $params_source)
                          (i32.const -1) (i32.const 20))
          (then (return (i32.const 5))))
        (if (call $script (local.get $env) (i32.load (i32.const 20)) (i32.const 24))
          (then (return (i32.const 6))))
        (if (call $string (local.get $env) (i32.const 320) (i32.const -1) (i32.const 28))
          (then (return (i32.const 7))))
        (if (call $string (local.get $env) (i32.const 300) (i32.const -1) (i32.const 32))
          (then (return (i32.const 8))))
        (if (call $string (local.get $env) (i32.const 350) (i32.const -1) (i32.const 36))
          (then (return (i32.const 9))))
        (i32.store (i32.const 400) (i32.const 0))
        (i32.store (i32.const 404) (i32.load (i32.const 32)))
        (i32.store (i32.const 408) (i32.const 0))
        (call $compile)))"#;

    for mode in 0..2 {
        for recursive in [false, true] {
            // The guest heap and default V8 reservation consume about 106 MiB.
            // A 128 MiB workload can compile a small function. At 144 MiB,
            // two nested 16 MiB reservations fit and the third is denied.
            let limit = if recursive { 144 * MIB } else { 128 * MIB };
            let ctx = NapiCtx::builder().total_memory_bytes(limit).build();
            let budget = ctx.budget();
            let peak = Arc::new(AtomicU64::new(0));
            let mut store = Store::default();
            let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
            let session = ctx.new_session(&module).unwrap();
            let mut imports = session.create_imports(&mut store.as_store_mut()).unwrap();
            let observed_peak = Arc::clone(&peak);
            let observed_budget = Arc::clone(&budget);
            imports.define(
                "test",
                "observe",
                Function::new_typed(&mut store, move || {
                    observed_peak
                        .fetch_max(observed_budget.snapshot().host_transient, Ordering::Relaxed);
                }),
            );
            let instance = Instance::new(&mut store, &module, &imports).unwrap();
            session
                .configure_instance(&mut store.as_store_mut(), &instance, None)
                .unwrap();
            let run = instance
                .exports
                .get_typed_function::<(i32, i32), i32>(&store, "run")
                .unwrap();
            assert_eq!(
                run.call(&mut store, mode, i32::from(recursive)).unwrap(),
                0,
                "mode {mode}, recursive {recursive}, budget {:?}",
                budget.snapshot()
            );
            let calls = instance
                .exports
                .get_typed_function::<(), i32>(&store, "calls")
                .unwrap()
                .call(&mut store)
                .unwrap();
            let denials = instance
                .exports
                .get_typed_function::<(), i32>(&store, "denials")
                .unwrap()
                .call(&mut store)
                .unwrap();
            if recursive {
                assert_eq!(calls, 2, "mode {mode}");
                assert_eq!(denials, 1, "mode {mode}");
                assert_eq!(peak.load(Ordering::Relaxed), 32 * MIB);
            } else {
                assert_eq!(calls, 0, "mode {mode}");
                assert_eq!(denials, 0, "mode {mode}");
            }
            assert_eq!(budget.snapshot().host_transient, 0);
        }
    }
}

#[test]
fn large_sparse_message_retains_charge_through_deserialization() {
    // The wire payload is small, but the decoded array's logical element
    // range is over 16 MiB of pointer slots. Consumption must keep the
    // serialized owner charged until native message_take destroys it.
    let wat = r#"(module
      (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
      (import "napi" "napi_get_array_length" (func $length (param i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_message_create" (func $message_create
        (param i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_message_take" (func $message_take
        (param i32 i32 i32) (result i32)))
      (memory (export "memory") 1)
      (data (i32.const 100) "new Array(5000000)\00")
      (func (export "create") (result i32)
        (local $env i32)
        (if (call $create (i32.const 8) (i32.const 4) (i32.const 8))
          (then (return (i32.const 1))))
        (local.set $env (i32.load (i32.const 4)))
        (if (call $string (local.get $env) (i32.const 100) (i32.const -1) (i32.const 12))
          (then (return (i32.const 2))))
        (if (call $script (local.get $env) (i32.load (i32.const 12)) (i32.const 16))
          (then (return (i32.const 3))))
        (if (call $message_create (local.get $env) (i32.load (i32.const 16))
                                  (i32.const 20))
          (then (return (i32.const 4))))
        (i32.load (i32.const 20)))
      (func (export "consume") (result i32)
        (local $env i32)
        (local.set $env (i32.load (i32.const 4)))
        (if (call $message_take (local.get $env) (i32.load (i32.const 20))
                                (i32.const 24))
          (then (return (i32.const -1))))
        (if (call $length (local.get $env) (i32.load (i32.const 24)) (i32.const 28))
          (then (return (i32.const -2))))
        (i32.load (i32.const 28))))"#;

    let ctx = NapiCtx::builder().total_memory_bytes(192 * MIB).build();
    let budget = ctx.budget();
    let mut store = Store::default();
    let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
    let session = ctx.new_session(&module).unwrap();
    let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    session
        .configure_instance(&mut store.as_store_mut(), &instance, None)
        .unwrap();
    let create = instance
        .exports
        .get_typed_function::<(), i32>(&store, "create")
        .unwrap();
    let consume = instance
        .exports
        .get_typed_function::<(), i32>(&store, "consume")
        .unwrap();
    let message = create.call(&mut store).unwrap();
    assert!(message > 0, "message create returned status {message}");
    assert!(
        budget.snapshot().serialized_message > 0,
        "message create failed: {message}"
    );
    assert_eq!(consume.call(&mut store).unwrap(), 5_000_000);
    assert_eq!(budget.snapshot().serialized_message, 0);
    assert_eq!(budget.snapshot().host_transient, 0);
}
