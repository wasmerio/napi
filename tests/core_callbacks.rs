use wasmer::{AsStoreMut, Function, Instance, Module, RuntimeError, Store};
use wasmer_napi::NapiCtx;
use wasmer_wasix::{WasiError, wasmer_wasix_types::wasi::ExitCode};
fn configure_runtime() {
    unsafe extern "C" {
        fn snapi_bridge_unofficial_configure_runtime(
            flags: *const std::ffi::c_char,
            length: u32,
        ) -> i32;
    }
    assert_eq!(
        unsafe { snapi_bridge_unofficial_configure_runtime(std::ptr::null(), 0) },
        0
    );
}

#[test]
fn core_callback_exit_probe() {
    configure_runtime();
    let cases = [
        (
            "create_buffer",
            "(import \"napi\" \"napi_create_buffer\" (func $op (param i32 i32 i32 i32) (result i32)))",
            "Object.defineProperty(globalThis, 'Buffer', {get(){exitNow();return undefined;}}); 0",
            "",
            "(call $op (local.get $env) (i32.const 8) (i32.const 28) (i32.const 32))",
        ),
        (
            "create_buffer_copy",
            "(import \"napi\" \"napi_create_buffer_copy\" (func $op (param i32 i32 i32 i32 i32) (result i32)))",
            "Object.defineProperty(globalThis, 'Buffer', {get(){exitNow();return undefined;}}); 0",
            "",
            "(call $op (local.get $env) (i32.const 1) (i32.const 500) (i32.const 28) (i32.const 32))",
        ),
        (
            "create_typedarray",
            "(import \"napi\" \"napi_create_typedarray\" (func $op (param i32 i32 i32 i32 i32 i32) (result i32)))",
            "globalThis.Uint8Array=function(){exitNow();}; new ArrayBuffer(8)",
            "",
            "(call $op (local.get $env) (i32.const 1) (i32.const 8) (i32.load (i32.const 24)) (i32.const 0) (i32.const 28))",
        ),
        (
            "create_dataview",
            "(import \"napi\" \"napi_create_dataview\" (func $op (param i32 i32 i32 i32 i32) (result i32)))",
            "globalThis.DataView=function(){exitNow();}; new ArrayBuffer(8)",
            "",
            "(call $op (local.get $env) (i32.const 8) (i32.load (i32.const 24)) (i32.const 0) (i32.const 28))",
        ),
        (
            "create_error",
            "(import \"napi\" \"napi_create_error\" (func $op (param i32 i32 i32 i32) (result i32)))",
            "Object.defineProperty(Error.prototype, 'code', { set(v) {exitNow();} }); 'msg'",
            "",
            "(call $op (local.get $env) (i32.load (i32.const 24)) (i32.load (i32.const 24)) (i32.const 28))",
        ),
        (
            "create_type_error",
            "(import \"napi\" \"napi_create_type_error\" (func $op (param i32 i32 i32 i32) (result i32)))",
            "Object.defineProperty(Error.prototype, 'code', { set(v) {exitNow();} }); 'msg'",
            "",
            "(call $op (local.get $env) (i32.load (i32.const 24)) (i32.load (i32.const 24)) (i32.const 28))",
        ),
        (
            "create_range_error",
            "(import \"napi\" \"napi_create_range_error\" (func $op (param i32 i32 i32 i32) (result i32)))",
            "Object.defineProperty(Error.prototype, 'code', { set(v) {exitNow();} }); 'msg'",
            "",
            "(call $op (local.get $env) (i32.load (i32.const 24)) (i32.load (i32.const 24)) (i32.const 28))",
        ),
        (
            "throw_error",
            "(import \"napi\" \"napi_throw_error\" (func $op (param i32 i32 i32) (result i32)))",
            "Object.defineProperty(Error.prototype, 'code', { set(v) {exitNow();} }); 'msg'",
            "",
            "(call $op (local.get $env) (i32.const 500) (i32.const 500))",
        ),
        (
            "throw_type_error",
            "(import \"napi\" \"napi_throw_type_error\" (func $op (param i32 i32 i32) (result i32)))",
            "Object.defineProperty(Error.prototype, 'code', { set(v) {exitNow();} }); 'msg'",
            "",
            "(call $op (local.get $env) (i32.const 500) (i32.const 500))",
        ),
        (
            "throw_range_error",
            "(import \"napi\" \"napi_throw_range_error\" (func $op (param i32 i32 i32) (result i32)))",
            "Object.defineProperty(Error.prototype, 'code', { set(v) {exitNow();} }); 'msg'",
            "",
            "(call $op (local.get $env) (i32.const 500) (i32.const 500))",
        ),
        (
            "coerce_number",
            "(import \"napi\" \"napi_coerce_to_number\" (func $op (param i32 i32 i32) (result i32)))",
            "({ valueOf() { exitNow(); return 7; } })",
            "",
            "(call $op (local.get $env) (i32.load (i32.const 24)) (i32.const 28))",
        ),
        (
            "coerce_string",
            "(import \"napi\" \"napi_coerce_to_string\" (func $op (param i32 i32 i32) (result i32)))",
            "({ toString() { exitNow(); return 'x'; } })",
            "",
            "(call $op (local.get $env) (i32.load (i32.const 24)) (i32.const 28))",
        ),
        (
            "instanceof",
            "(import \"napi\" \"napi_instanceof\" (func $op (param i32 i32 i32 i32) (result i32)))",
            "function C() {}; Object.defineProperty(C, Symbol.hasInstance, {value: function(){exitNow(); return true;}}); C",
            "",
            "(call $op (local.get $env) (i32.load (i32.const 16)) (i32.load (i32.const 24)) (i32.const 28))",
        ),
        (
            "freeze",
            "(import \"napi\" \"napi_object_freeze\" (func $op (param i32 i32) (result i32)))",
            "new Proxy({}, { preventExtensions(t) { exitNow(); return Reflect.preventExtensions(t) } })",
            "",
            "(call $op (local.get $env) (i32.load (i32.const 24)))",
        ),
        (
            "seal",
            "(import \"napi\" \"napi_object_seal\" (func $op (param i32 i32) (result i32)))",
            "new Proxy({}, { preventExtensions(t) { exitNow(); return Reflect.preventExtensions(t) } })",
            "",
            "(call $op (local.get $env) (i32.load (i32.const 24)))",
        ),
        (
            "set_prototype",
            "(import \"napi\" \"node_api_set_prototype\" (func $op (param i32 i32 i32) (result i32)))",
            "new Proxy({}, { setPrototypeOf(t,p) { exitNow(); return Reflect.setPrototypeOf(t,p) } })",
            "",
            "(call $op (local.get $env) (i32.load (i32.const 24)) (i32.load (i32.const 16)))",
        ),
        (
            "resolve",
            "(import \"napi\" \"napi_create_promise\" (func $promise (param i32 i32 i32) (result i32))) (import \"napi\" \"napi_resolve_deferred\" (func $op (param i32 i32 i32) (result i32)))",
            "({ get then() { exitNow(); return undefined; } })",
            "",
            "(drop (call $promise (local.get $env) (i32.const 28) (i32.const 32))) (call $op (local.get $env) (i32.load (i32.const 28)) (i32.load (i32.const 24)))",
        ),
        (
            "reject",
            "(import \"napi\" \"napi_create_promise\" (func $promise (param i32 i32 i32) (result i32))) (import \"napi\" \"napi_reject_deferred\" (func $op (param i32 i32 i32) (result i32))) (import \"napi_extension_wasmer_v0\" \"unofficial_napi_set_promise_reject_callback\" (func $set_cb (param i32 i32) (result i32)))",
            "7",
            "(drop (call $set_cb (local.get $env) (i32.load (i32.const 12))))",
            "(drop (call $promise (local.get $env) (i32.const 28) (i32.const 32))) (call $op (local.get $env) (i32.load (i32.const 28)) (i32.load (i32.const 24)))",
        ),
        (
            "create_promise",
            "(import \"napi\" \"napi_create_promise\" (func $op (param i32 i32 i32) (result i32))) (import \"napi_extension_wasmer_v0\" \"unofficial_napi_set_promise_hooks\" (func $set_hooks (param i32 i32 i32 i32 i32) (result i32)))",
            "7",
            "(drop (call $set_hooks (local.get $env) (i32.load (i32.const 12)) (i32.const 0) (i32.const 0) (i32.const 0)))",
            "(call $op (local.get $env) (i32.const 28) (i32.const 32))",
        ),
        (
            "define_properties",
            "(import \"napi\" \"napi_define_properties\" (func $op (param i32 i32 i32 i32) (result i32)))",
            "new Proxy({}, { defineProperty(t,k,d) { exitNow(); return Reflect.defineProperty(t,k,d) } })",
            "(i32.store (i32.const 400) (i32.const 500)) (i32.store (i32.const 420) (i32.load (i32.const 16)))",
            "(call $op (local.get $env) (i32.load (i32.const 24)) (i32.const 1) (i32.const 400))",
        ),
    ];
    let mut failures = Vec::new();
    for (name, import, js, setup, op) in cases {
        let wat = format!(
            r#"(module
      (import "napi_extension_wasmer_v0" "unofficial_napi_create_env" (func $create (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_function" (func $function (param i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_get_global" (func $global (param i32 i32) (result i32)))
      (import "napi" "napi_set_named_property" (func $set (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
      (import "wasi_snapshot_preview1" "proc_exit" (func $exit (param i32)))
      {import}
      (memory (export "memory") 1)
      (table (export "__indirect_function_table") 1 funcref)
      (elem (i32.const 0) $callback)
      (data (i32.const 100) "exitNow\00")
      (data (i32.const 150) "{js}\00")
      (data (i32.const 500) "x\00")
      (func $callback (param i32 i32) (result i32)
        (call $exit (i32.const 57)) (i32.const 0))
      (func (export "run") (result i32)
        (local $env i32)
        (drop (call $create (i32.const 8) (i32.const 0) (i32.const 4) (i32.const 8)))
        (local.set $env (i32.load (i32.const 4)))
        (drop (call $function (local.get $env) (i32.const 100) (i32.const -1) (i32.const 0) (i32.const 0) (i32.const 12)))
        (drop (call $global (local.get $env) (i32.const 16)))
        (drop (call $set (local.get $env) (i32.load (i32.const 16)) (i32.const 100) (i32.load (i32.const 12))))
        (drop (call $string (local.get $env) (i32.const 150) (i32.const -1) (i32.const 20)))
        (drop (call $script (local.get $env) (i32.load (i32.const 20)) (i32.const 24)))
        {setup}
        {op}))"#
        );
        let ctx = NapiCtx::default();
        let mut store = Store::default();
        let module = Module::new(&store, wat::parse_str(&wat).unwrap()).unwrap();
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
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        session
            .configure_instance(
                &mut store.as_store_mut(),
                &instance,
                Some(instance.exports.get_memory("memory").unwrap()),
            )
            .unwrap();
        let result = instance
            .exports
            .get_typed_function::<(), i32>(&store, "run")
            .unwrap()
            .call(&mut store);
        if !matches!(result, Err(ref e) if matches!(e.downcast_ref::<WasiError>(), Some(WasiError::Exit(c)) if *c == ExitCode::from(57)))
        {
            failures.push(format!("{name}: {result:?}"));
        }
    }
    assert!(failures.is_empty(), "Lost callback exits: {failures:?}");
}

#[test]
fn reentrant_release_refuses_to_dispose_the_entered_isolate() {
    configure_runtime();
    let wat = r#"(module
      (import "napi_extension_wasmer_v0" "unofficial_napi_create_env" (func $create (param i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_release_env" (func $release (param i32 i32) (result i32)))
      (import "napi" "napi_create_function" (func $function (param i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_get_global" (func $global (param i32 i32) (result i32)))
      (import "napi" "napi_set_named_property" (func $set (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
      (memory (export "memory") 1)
      (table (export "__indirect_function_table") 1 funcref)
      (elem (i32.const 0) $callback)
      (data (i32.const 100) "releaseNow\00")
      (data (i32.const 150) "releaseNow(); 42\00")
      (func $callback (param i32 i32) (result i32)
        (i32.store (i32.const 40) (call $release (i32.load (i32.const 8)) (i32.const 0)))
        (i32.const 0))
      (func (export "run") (result i32)
        (local $env i32)
        (drop (call $create (i32.const 8) (i32.const 0) (i32.const 4) (i32.const 8)))
        (local.set $env (i32.load (i32.const 4)))
        (drop (call $function (local.get $env) (i32.const 100) (i32.const -1) (i32.const 0) (i32.const 0) (i32.const 12)))
        (drop (call $global (local.get $env) (i32.const 16)))
        (drop (call $set (local.get $env) (i32.load (i32.const 16)) (i32.const 100) (i32.load (i32.const 12))))
        (drop (call $string (local.get $env) (i32.const 150) (i32.const -1) (i32.const 20)))
        (if (call $script (local.get $env) (i32.load (i32.const 20)) (i32.const 24))
          (then (return (i32.const -1))))
        ;; The callback must fail without losing the environment registration.
        (if (i32.eqz (i32.load (i32.const 40))) (then (return (i32.const -2))))
        (call $script (local.get $env) (i32.load (i32.const 20)) (i32.const 24))))"#;
    let ctx = NapiCtx::default();
    let mut store = Store::default();
    let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
    let session = ctx.new_session(&module).unwrap();
    let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    session
        .configure_instance(
            &mut store.as_store_mut(),
            &instance,
            Some(instance.exports.get_memory("memory").unwrap()),
        )
        .unwrap();
    let status = instance
        .exports
        .get_typed_function::<(), i32>(&store, "run")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert_eq!(status, 0);
}
