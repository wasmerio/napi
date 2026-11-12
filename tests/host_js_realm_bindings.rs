//! Real wasm32 realm and binding imports against the host-JS provider.
#![cfg(all(target_arch = "wasm32", feature = "js"))]

use wasm_bindgen_test::wasm_bindgen_test;
use wasmer::{AsStoreMut, Instance, Module, Store, TypedFunction};
use wasmer_napi::NapiCtx;

#[wasm_bindgen_test]
fn realm_modes_binding_and_callback_owner() {
    let wasm = wat::parse_str(r#"(module
      (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
        (func $configure (param i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
        (func $create_env (param i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
        (func $release (param i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_create_realm"
        (func $create (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_enter_realm"
        (func $enter (param i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_leave_realm"
        (func $leave (param i32 i64) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_get_current_realm_data"
        (func $owner (param i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_snapshot_commit"
        (func $commit (param i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_snapshot_guest_data"
        (func $guest_data (param i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_define_binding"
        (func $define (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8"
        (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
      (import "napi" "napi_set_named_property"
        (func $set (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_int32" (func $int (param i32 i32 i32) (result i32)))
      (import "napi" "napi_get_cb_info"
        (func $cb_info (param i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_get_new_target"
        (func $new_target (param i32 i32 i32) (result i32)))
      (import "napi" "napi_is_exception_pending"
        (func $pending (param i32 i32) (result i32)))
      (memory (export "memory") 1)
      (table (export "__indirect_function_table") 4 funcref)
      (elem (i32.const 1) $method $ctor $class_method)
      (global $env (mut i32) (i32.const 0))
      (global $stage (export "stage") (mut i32) (i32.const 0))
      (data (i32.const 256) "Bound\00")
      (data (i32.const 264) "Base\00Derived\00answer\00member\00")
      (data (i32.const 384) "\00\02") ;; arities for the value and method members
      (data (i32.const 512)
        "(() => { const fn = globalThis.Bound;"
        "if (fn.name !== 'Bound' || fn.length !== 3 || Object.hasOwn(fn, 'prototype') || fn.call(null) !== 42)"
        " throw Error('binding shape or result'); })()\00")
      (data (i32.const 768)
        "(() => { const {Base, Derived} = globalThis; const obj = new Derived();"
        "if (Base.name !== 'Base' || Base.length !== 1 || Derived.name !== 'Derived' || Derived.length !== 2"
        " || Object.getPrototypeOf(Derived) !== Base || Object.getPrototypeOf(Derived.prototype) !== Base.prototype"
        " || obj.constructor !== Derived || !(obj instanceof Base) || !(obj instanceof Derived)"
        " || Derived.answer !== 42 || obj.member.name !== 'member' || obj.member.length !== 2 || obj.member() !== 43"
        " || Object.getOwnPropertyDescriptor(Base, 'prototype').writable)"
        " throw Error('class shape, inheritance or callback'); })()\00")
      (func $status (param $actual i32) (param $expected i32)
        (if (i32.ne (local.get $actual) (local.get $expected)) (then unreachable)))
      (func $check_owner (param $expected i64)
        (i64.store (i32.const 64) (i64.const -1))
        (call $status (call $owner (global.get $env) (i32.const 64)) (i32.const 0))
        (if (i64.ne (i64.load (i32.const 64)) (local.get $expected)) (then unreachable)))
      (func $method (param i32 i32) (result i32)
        (global.set $stage (i32.const 4))
        (call $check_owner (i64.const 0xf1234567))
        (call $status (call $enter (global.get $env) (i32.load (i32.const 32)) (i32.const 80)) (i32.const 0))
        (call $check_owner (i64.const 13))
        ;; Deliberately leave the nested scope open. The trampoline unwinds it.
        (call $status (call $int (global.get $env) (i32.const 42) (i32.const 76)) (i32.const 0))
        (i32.load (i32.const 76)))
      (func $ctor (param $e i32) (param $info i32) (result i32)
        (call $check_owner (i64.const 0xf1234567))
        (call $status (call $new_target (local.get $e) (local.get $info) (i32.const 104)) (i32.const 0))
        (if (i32.eqz (i32.load (i32.const 104))) (then unreachable))
        (i32.const 0))
      (func $class_method (param $e i32) (param $info i32) (result i32)
        (call $check_owner (i64.const 0xf1234567))
        (call $status (call $cb_info (local.get $e) (local.get $info) (i32.const 0)
          (i32.const 0) (i32.const 0) (i32.const 104)) (i32.const 0))
        (call $status (call $int (local.get $e) (i32.load (i32.const 104)) (i32.const 76)) (i32.const 0))
        (i32.load (i32.const 76)))
      (func (export "test") (local $e i32)
        (call $status (call $configure (i32.const 0)) (i32.const 0))
        (call $status (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12)) (i32.const 0))
        (local.set $e (i32.load (i32.const 8)))
        (global.set $env (local.get $e))
        (call $check_owner (i64.const 0))
        (global.set $stage (i32.const 1))
        ;; Declines occur before reading the out-of-bounds options or other ranges.
        (call $status (call $create (local.get $e) (i32.const -1) (i32.const 2) (i32.const -1)
          (i32.const 0) (i32.const -1) (i32.const 0) (i32.const 0)) (i32.const 9))
        (call $status (call $create (local.get $e) (i32.const -1) (i32.const 3) (i32.const 0)
          (i32.const 0) (i32.const -1) (i32.const 0) (i32.const 0)) (i32.const 1))
        (call $status (call $create (local.get $e) (i32.const -1) (i32.const 3) (i32.const 0)
          (i32.const 0) (i32.const -1) (i32.const 0) (i32.const -1)) (i32.const 9))
        (call $status (call $commit (local.get $e) (i32.const 0) (i32.const 0)
          (i32.const 0) (i32.const -1) (i32.const -1)) (i32.const 9))
        (i32.store (i32.const 72) (i32.const -1))
        (call $status (call $guest_data (local.get $e) (i32.const 0) (i32.const 0) (i32.const 72)) (i32.const 0))
        (call $status (i32.load (i32.const 72)) (i32.const 0))
        (call $status (call $owner (local.get $e) (i32.const 65532)) (i32.const 1))
        (global.set $stage (i32.const 2))
        (i32.store (i32.const 28) (i32.const -1))
        (call $status (call $create (local.get $e) (i32.const 0) (i32.const 1) (i32.const 0)
          (i32.const 0xf1234567) (i32.const 20) (i32.const 24) (i32.const 28)) (i32.const 0))
        (call $status (i32.load (i32.const 20)) (i32.load (i32.const 24)))
        (call $status (i32.load (i32.const 28)) (i32.const 0))
        (call $status (call $create (local.get $e) (i32.const 0) (i32.const 1) (i32.const 0)
          (i32.const 13) (i32.const 32) (i32.const 0) (i32.const 0)) (i32.const 0))
        (call $status (call $enter (local.get $e) (i32.load (i32.const 20)) (i32.const 48)) (i32.const 0))
        (call $check_owner (i64.const 0xf1234567))
        (call $status (call $define (local.get $e) (i32.const 2) (i32.const 256) (i32.const 5)
          (i32.const 1) (i32.const 0) (i32.const 3) (i32.const 0) (i32.const 0) (i32.const 0)
          (i32.const 0) (i32.const 65536) (i32.const 0)) (i32.const 1))
        (call $status (call $define (local.get $e) (i32.const 2) (i32.const 256) (i32.const 5)
          (i32.const 1) (i32.const 0) (i32.const 3) (i32.const 0) (i32.const 0) (i32.const 0)
          (i32.const 0) (i32.const 36) (i32.const 0)) (i32.const 0))
        (global.set $stage (i32.const 5))
        ;; Class member descriptors. Base.answer is a static primitive and Base.prototype.member is callable.
        (call $status (call $int (local.get $e) (i32.const 42) (i32.const 76)) (i32.const 0))
        (i32.store (i32.const 320) (i32.const 277))
        (i32.store (i32.const 340) (i32.load (i32.const 76)))
        (i32.store (i32.const 344) (i32.const 1024))
        (i32.store (i32.const 352) (i32.const 284))
        (i32.store (i32.const 360) (i32.const 3))
        (i32.store (i32.const 380) (i32.const 43))
        (call $status (call $define (local.get $e) (i32.const 1) (i32.const 264) (i32.const 4)
          (i32.const 2) (i32.const 0) (i32.const 1) (i32.const 0) (i32.const 2) (i32.const 320)
          (i32.const 384) (i32.const 112) (i32.const 116)) (i32.const 0))
        ;; A METHOD is not a registered class and cannot serve as the parent.
        (call $status (call $define (local.get $e) (i32.const 1) (i32.const 269) (i32.const 7)
          (i32.const 2) (i32.const 0) (i32.const 2) (i32.load (i32.const 36)) (i32.const 0) (i32.const 0)
          (i32.const 0) (i32.const 120) (i32.const 124)) (i32.const 1))
        (call $status (call $define (local.get $e) (i32.const 1) (i32.const 269) (i32.const 7)
          (i32.const 2) (i32.const 0) (i32.const 2) (i32.load (i32.const 112)) (i32.const 0) (i32.const 0)
          (i32.const 0) (i32.const 120) (i32.const 124)) (i32.const 0))
        (call $status (call $leave (local.get $e) (i64.load (i32.const 48))) (i32.const 0))
        (call $status (call $enter (local.get $e) (i32.load (i32.const 32)) (i32.const 56)) (i32.const 0))
        (call $status (call $set (local.get $e) (i32.load (i32.const 32)) (i32.const 256)
          (i32.load (i32.const 36))) (i32.const 0))
        (call $status (call $string (local.get $e) (i32.const 512) (i32.const -1) (i32.const 84)) (i32.const 0))
        (global.set $stage (i32.const 3))
        (call $status (call $script (local.get $e) (i32.load (i32.const 84)) (i32.const 88)) (i32.const 0))
        (call $check_owner (i64.const 13))
        (global.set $stage (i32.const 6))
        (call $status (call $set (local.get $e) (i32.load (i32.const 32)) (i32.const 264)
          (i32.load (i32.const 112))) (i32.const 0))
        (call $status (call $set (local.get $e) (i32.load (i32.const 32)) (i32.const 269)
          (i32.load (i32.const 120))) (i32.const 0))
        (call $status (call $string (local.get $e) (i32.const 768) (i32.const -1) (i32.const 84)) (i32.const 0))
        (call $status (call $script (local.get $e) (i32.load (i32.const 84)) (i32.const 88)) (i32.const 0))
        (call $check_owner (i64.const 13))
        (call $status (call $leave (local.get $e) (i64.load (i32.const 56))) (i32.const 0))
        (call $check_owner (i64.const 0))
        (call $status (call $release (i32.load (i32.const 12)) (i32.const 0)) (i32.const 0)))
      (func (export "test_stopped") (local $e i32)
        (call $status (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12)) (i32.const 0))
        (local.set $e (i32.load (i32.const 8)))
        ;; Invalid mode and missing required outputs are checked before the stop.
        (call $status (call $create (local.get $e) (i32.const -1) (i32.const 99) (i32.const 0)
          (i32.const 0) (i32.const -1) (i32.const 0) (i32.const -1)) (i32.const 1))
        (call $status (call $define (local.get $e) (i32.const 1) (i32.const -1) (i32.const 5)
          (i32.const 1) (i32.const 0) (i32.const 3) (i32.const 0) (i32.const 0) (i32.const 0)
          (i32.const 0) (i32.const 65536) (i32.const 0)) (i32.const 1))
        (call $status (call $commit (local.get $e) (i32.const 0) (i32.const 0)
          (i32.const 0) (i32.const -1) (i32.const -1)) (i32.const 10))
        (call $status (call $pending (local.get $e) (i32.const 16)) (i32.const 0))
        (call $status (i32.load8_u (i32.const 16)) (i32.const 1))
        (call $status (call $create (local.get $e) (i32.const -1) (i32.const 3) (i32.const 0)
          (i32.const 0) (i32.const -1) (i32.const 0) (i32.const -1)) (i32.const 10))
        (call $status (call $define (local.get $e) (i32.const 2) (i32.const -1) (i32.const 5)
          (i32.const 1) (i32.const 0) (i32.const 3) (i32.const 0) (i32.const 0) (i32.const 0)
          (i32.const 0) (i32.const 65536) (i32.const 0)) (i32.const 10))
        (call $status (call $release (i32.load (i32.const 12)) (i32.const 0)) (i32.const 0)))
    )"#).unwrap();
    let mut store = Store::default();
    let module = Module::new(&store, wasm).unwrap();
    let ctx = NapiCtx::default();
    let hooks = ctx.runtime_hooks();
    let (imports, state) = hooks
        .additional_imports(&module, &mut store.as_store_mut())
        .unwrap();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    hooks
        .configure_instance(
            &module,
            &mut store.as_store_mut(),
            &instance,
            Some(instance.exports.get_memory("memory").unwrap()),
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

    ctx.runtime_control().terminate_all();
    let stopped: TypedFunction<(), ()> = instance
        .exports
        .get_typed_function(&store, "test_stopped")
        .unwrap();
    stopped.call(&mut store).expect("stopped realm calls");
}
