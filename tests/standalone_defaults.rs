use wasmer::{AsStoreMut, Instance, Module, Store};
use wasmer_napi::NapiCtx;

// Exercise the actual guest import boundary, including a NAPI_AUTO_LENGTH
// string and descriptor array beyond the managed per-call ceilings.
fn run(ctx: &NapiCtx, mode: i32) -> i32 {
    let mut store = Store::default();
    let module = Module::new(
        &store,
        wat::parse_str(
            r#"(module
      (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
        (func $create (param i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_create_serdes_binding"
        (func $serdes (param i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8"
        (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_run_script"
        (func $script (param i32 i32 i32) (result i32)))
      (import "napi" "napi_create_object"
        (func $object (param i32 i32) (result i32)))
      (import "napi" "napi_create_int32"
        (func $int (param i32 i32 i32) (result i32)))
      (import "napi" "napi_define_properties"
        (func $properties (param i32 i32 i32 i32) (result i32)))
      (memory (export "memory") 4)
      (table (export "__indirect_function_table") 1 funcref)
      (data (i32.const 128)
        "new WebAssembly.Module(Uint8Array.of(0,97,115,109,1,0,0,0))\00")
      (data (i32.const 512) "x\00")
      (func (export "run") (param $mode i32) (result i32)
        (local $env i32) (local $i i32) (local $ptr i32)
        (if (call $create (i32.const 8) (i32.const 0) (i32.const 4) (i32.const 8))
          (then (return (i32.const 99))))
        (local.set $env (i32.load (i32.const 4)))
        (if (i32.eqz (local.get $mode))
          (then (return (call $serdes (local.get $env) (i32.const 16)))))
        (if (i32.eq (local.get $mode) (i32.const 1))
          (then
            (if (call $string (local.get $env) (i32.const 128) (i32.const -1) (i32.const 16))
              (then (return (i32.const 98))))
            (return (call $script (local.get $env) (i32.load (i32.const 16)) (i32.const 20)))))
        (if (i32.eq (local.get $mode) (i32.const 3))
          (then
            (if (call $object (local.get $env) (i32.const 20))
              (then (return (i32.const 98))))
            (if (call $int (local.get $env) (i32.const 42) (i32.const 24))
              (then (return (i32.const 98))))
            (loop $descriptors
              (local.set $ptr (i32.add (i32.const 8192) (i32.mul (local.get $i) (i32.const 32))))
              (i32.store (local.get $ptr) (i32.const 512))
              (i32.store offset=20 (local.get $ptr) (i32.load (i32.const 24)))
              (i32.store offset=24 (local.get $ptr) (i32.const 7))
              (local.set $i (i32.add (local.get $i) (i32.const 1)))
              (br_if $descriptors (i32.lt_u (local.get $i) (i32.const 5000))))
            (return (call $properties (local.get $env) (i32.load (i32.const 20))
                                     (i32.const 5000) (i32.const 8192)))))
        (memory.fill (i32.const 1024) (i32.const 97) (i32.const 70000))
        (i32.store8 (i32.const 71024) (i32.const 0))
        (call $string (local.get $env) (i32.const 1024) (i32.const -1) (i32.const 16))))"#,
        )
        .unwrap(),
    )
    .unwrap();
    let session = ctx.new_session(&module).unwrap();
    let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    session
        .configure_instance(&mut store.as_store_mut(), &instance, None)
        .unwrap();
    instance
        .exports
        .get_typed_function::<i32, i32>(&store, "run")
        .unwrap()
        .call(&mut store, mode)
        .unwrap()
}

#[test]
fn standalone_allows_serdes_webassembly_and_large_guest_operations() {
    let ctx = NapiCtx::default();
    assert!(ctx.budget().is_unlimited());
    for mode in 0..4 {
        assert_eq!(run(&ctx, mode), 0, "standalone mode {mode}");
    }
}

#[test]
fn explicit_memory_budget_keeps_unaccounted_features_and_copy_caps_restricted() {
    let ctx = NapiCtx::builder()
        .total_memory_bytes(160 * 1024 * 1024)
        .build();
    for mode in 0..4 {
        let status = run(&ctx, mode);
        assert_ne!(status, 99, "failed to create budgeted environment");
        assert_ne!(status, 98, "failed to create script source");
        assert_ne!(status, 0, "budgeted mode {mode} must remain restricted");
    }
}
