//! Real host-JS imports, guest memory outputs and a Wasm initializer callback.
#![cfg(all(target_arch = "wasm32", feature = "js"))]

use wasm_bindgen_test::wasm_bindgen_test;
use wasmer::{AsStoreMut, Instance, Module, Store, TypedFunction};
use wasmer_napi::NapiCtx;

#[wasm_bindgen_test]
fn lazy_properties_guest_dispatch_initializes_once_and_writes_a_bool() {
    let wasm = wat::parse_str(r#"(module
      (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
        (func $configure (param i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
        (func $create_env (param i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
        (func $release_env (param i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_define_lazy_data_properties"
        (func $define (param i32 i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_is_lazy_data_property"
        (func $lazy (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8"
        (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
      (import "napi" "napi_create_function"
        (func $function (param i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_int32" (func $int (param i32 i32 i32) (result i32)))
      (import "napi" "napi_get_value_int32" (func $read_int (param i32 i32 i32) (result i32)))
      (import "napi" "napi_get_named_property"
        (func $get (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_get_global" (func $global (param i32 i32) (result i32)))
      (import "napi" "napi_set_named_property"
        (func $set (param i32 i32 i32 i32) (result i32)))
      (memory (export "memory") 1)
      (table (export "__indirect_function_table") 2 funcref)
      (global $env (mut i32) (i32.const 0))
      (global $calls (mut i32) (i32.const 0))
      (global $stage (export "stage") (mut i32) (i32.const 0))
      (data (i32.const 256) "answer\00")
      (data (i32.const 272) "({})")
      (data (i32.const 288) "Object.defineProperty(['answer'], '0', {get() { lazyInitializer(); return 'answer'; }})\00")
      (data (i32.const 512) "lazyInitializer\00")

      (func $eval (param $ptr i32) (param $len i32) (param $out i32)
        (if (call $string (global.get $env) (local.get $ptr) (local.get $len) (i32.const 40))
          (then unreachable))
        (if (call $script (global.get $env) (i32.load (i32.const 40)) (local.get $out))
          (then unreachable)))
      (func $initializer (param $env i32) (param $info i32) (result i32)
        (global.set $calls (i32.add (global.get $calls) (i32.const 1)))
        (if (call $int (local.get $env) (i32.const 42) (i32.const 44)) (then unreachable))
        (i32.load (i32.const 44)))
      (elem (i32.const 1) $initializer)

      (func (export "test")
        (if (call $configure (i32.const 0)) (then unreachable))
        (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
          (then unreachable))
        (global.set $env (i32.load (i32.const 8)))
        (global.set $stage (i32.const 1))
        (call $eval (i32.const 272) (i32.const 4) (i32.const 16))
        (global.set $stage (i32.const 2))
        (global.set $stage (i32.const 3))
        (if (call $string (global.get $env) (i32.const 256) (i32.const 6) (i32.const 28))
          (then unreachable))
        (if (call $function (global.get $env) (i32.const 256) (i32.const 6)
              (i32.const 1) (i32.const 0) (i32.const 24)) (then unreachable))
        (if (call $global (global.get $env) (i32.const 48)) (then unreachable))
        (if (call $set (global.get $env) (i32.load (i32.const 48))
              (i32.const 512) (i32.load (i32.const 24))) (then unreachable))
        (call $eval (i32.const 288) (i32.const -1) (i32.const 20))
        (global.set $stage (i32.const 4))
        (if (call $define (global.get $env) (i32.load (i32.const 16))
              (i32.load (i32.const 20)) (i32.load (i32.const 24)) (i32.const 7))
          (then unreachable))
        (global.set $stage (i32.const 5))
        ;; Reading names also lends the store: its getter calls the guest once.
        (if (i32.ne (global.get $calls) (i32.const 1)) (then unreachable))

        ;; A guest bool occupies one byte, including on failure paths.
        (i32.store (i32.const 64) (i32.const 0xaaaaaaaa))
        (if (call $lazy (global.get $env) (i32.load (i32.const 16))
              (i32.load (i32.const 28)) (i32.const 64)) (then unreachable))
        (if (i32.ne (i32.load (i32.const 64)) (i32.const 0xaaaaaa01)) (then unreachable))
        (global.set $stage (i32.const 6))
        (if (i32.ne (call $lazy (global.get $env) (i32.load (i32.const 16))
              (i32.load (i32.const 28)) (i32.const 65536)) (i32.const 1)) (then unreachable))

        ;; This import lends the store for the JS getter to call back into Wasm.
        (global.set $stage (i32.const 7))
        (if (call $get (global.get $env) (i32.load (i32.const 16))
              (i32.const 256) (i32.const 32)) (then unreachable))
        (global.set $stage (i32.const 8))
        (if (call $get (global.get $env) (i32.load (i32.const 16))
              (i32.const 256) (i32.const 32)) (then unreachable))
        (if (i32.ne (global.get $calls) (i32.const 2)) (then unreachable))
        (if (call $read_int (global.get $env) (i32.load (i32.const 32)) (i32.const 68))
          (then unreachable))
        (if (i32.ne (i32.load (i32.const 68)) (i32.const 42)) (then unreachable))
        (if (call $lazy (global.get $env) (i32.load (i32.const 16))
              (i32.load (i32.const 28)) (i32.const 64)) (then unreachable))
        (if (i32.ne (i32.load (i32.const 64)) (i32.const 0xaaaaaa00)) (then unreachable))
        (if (call $release_env (i32.load (i32.const 12)) (i32.const 0)) (then unreachable)))
    )"#).unwrap();
    let mut store = Store::default();
    let module = Module::new(&store, wasm).unwrap();
    let ctx = NapiCtx::default();
    let hooks = ctx.runtime_hooks();
    let (imports, state) = hooks
        .additional_imports(&module, &mut store.as_store_mut())
        .unwrap();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    let memory = instance.exports.get_memory("memory").unwrap();
    hooks
        .configure_instance(
            &module,
            &mut store.as_store_mut(),
            &instance,
            Some(memory),
            state,
        )
        .unwrap();
    let test: TypedFunction<(), ()> = instance.exports.get_typed_function(&store, "test").unwrap();
    let result = test.call(&mut store);
    let stage = instance
        .exports
        .get_global("stage")
        .unwrap()
        .get(&mut store);
    assert!(result.is_ok(), "guest stage {stage:?}: {result:?}");
}
