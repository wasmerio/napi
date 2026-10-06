use std::sync::{
    Arc,
    atomic::{AtomicUsize, Ordering},
};

use wasmer::{AsStoreMut, Function, Instance, Module, RuntimeError, Store};
use wasmer_napi::NapiCtx;
use wasmer_wasix::{WasiError, wasmer_wasix_types::wasi::ExitCode};

#[test]
fn source_map_callback_exit_crosses_error_metadata_imports() {
    let wat = r#"(module
      (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
      (import "napi" "napi_create_function" (func $function (param i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_configure_source_maps" (func $configure (param i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_get_error_metadata" (func $metadata (param i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_preserve_error_source_message" (func $preserve_current (param i32 i32) (result i32)))
      (import "napi" "unofficial_napi_preserve_error_source_message" (func $preserve_legacy (param i32 i32) (result i32)))
      (import "wasi_snapshot_preview1" "proc_exit" (func $exit (param i32)))
      (import "test" "observe" (func $observe))
      (memory (export "memory") 1)
      (table (export "__indirect_function_table") 1 funcref)
      (elem (i32.const 0) $callback)
      (data (i32.const 100) "sourceMap\00")
      (data (i32.const 160) "(() => { try { throw new Error('boom'); } catch (e) { return e; } })()\0a//# sourceURL=original.js\00")
      (func $callback (param i32 i32) (result i32)
        (call $observe)
        (call $exit (i32.const 41))
        (i32.const 0))
      (func (export "run") (param $mode i32) (result i32)
        (local $env i32)
        (if (call $create (i32.const 8) (i32.const 4) (i32.const 8))
          (then (return (i32.const 1))))
        (local.set $env (i32.load (i32.const 4)))
        (if (call $function (local.get $env) (i32.const 100) (i32.const -1)
                            (i32.const 0) (i32.const 0) (i32.const 12))
          (then (return (i32.const 2))))
        (if (call $configure (local.get $env) (i32.const 1) (i32.load (i32.const 12)))
          (then (return (i32.const 3))))
        (if (call $string (local.get $env) (i32.const 160) (i32.const -1) (i32.const 16))
          (then (return (i32.const 4))))
        (if (call $script (local.get $env) (i32.load (i32.const 16)) (i32.const 20))
          (then (return (i32.const 5))))
        (if (result i32) (i32.eqz (local.get $mode))
          (then (call $metadata (local.get $env) (i32.load (i32.const 20))
                                (i32.const 0) (i32.const 32)))
          (else (if (result i32) (i32.eq (local.get $mode) (i32.const 1))
            (then (call $preserve_current (local.get $env) (i32.load (i32.const 20))))
            (else (call $preserve_legacy (local.get $env) (i32.load (i32.const 20)))))))))"#;

    for mode in 0..3 {
        let ctx = NapiCtx::default();
        let callback_count = Arc::new(AtomicUsize::new(0));
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
        let observed = Arc::clone(&callback_count);
        imports.define(
            "test",
            "observe",
            Function::new_typed(&mut store, move || {
                observed.fetch_add(1, Ordering::Relaxed);
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
                Some(WasiError::Exit(code)) if *code == ExitCode::from(41)
            ),
            "mode {mode}: {error}"
        );
        assert_eq!(callback_count.load(Ordering::Relaxed), 1, "mode {mode}");
        assert_eq!(ctx.budget().snapshot().host_transient, 0);
    }
}
