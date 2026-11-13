//! Exercise batched reference deletion through real Wasmer guest imports.

#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Instance, Module, Store};
use wasmer_napi::NapiCtx;

const WAT: &str = r#"(module
  (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
    (func $configure (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
    (func $create_env (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
    (func $release_env (param i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_delete_references"
    (func $delete_batch (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_object" (func $object (param i32 i32) (result i32)))
  (import "napi" "napi_create_reference" (func $reference (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_delete_reference" (func $delete (param i32 i32) (result i32)))
  (memory (export "memory") 1)
  (global $env (mut i32) (i32.const 0))

  (func (export "create")
    (if (call $configure (i32.const 0)) (then unreachable))
    (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
      (then unreachable))
    (global.set $env (i32.load (i32.const 8))))

  (func (export "create_ref") (param $out i32) (result i32)
    (if (call $object (global.get $env) (i32.const 16)) (then unreachable))
    (if (call $reference (global.get $env) (i32.load (i32.const 16)) (i32.const 1) (local.get $out))
      (then unreachable))
    (i32.load (local.get $out)))

  (func (export "delete_ref") (param $id i32) (result i32)
    (call $delete (global.get $env) (local.get $id)))

  (func (export "delete_batch") (param $refs i32) (param $count i32) (result i32)
    (call $delete_batch (global.get $env) (local.get $refs) (local.get $count)))

  (func (export "release") (result i32)
    (call $release_env (i32.load (i32.const 12)) (i32.const 0)))
)"#;

#[test]
fn deletes_valid_references_after_an_error_and_checks_the_whole_range_first() {
    let mut store = Store::default();
    let module = Module::new(&store, wat::parse_str(WAT).unwrap()).unwrap();
    let context = NapiCtx::default();
    let hooks = context.runtime_hooks();
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

    let create = instance
        .exports
        .get_typed_function::<(), ()>(&store, "create")
        .unwrap();
    let create_ref = instance
        .exports
        .get_typed_function::<i32, i32>(&store, "create_ref")
        .unwrap();
    let delete_ref = instance
        .exports
        .get_typed_function::<i32, i32>(&store, "delete_ref")
        .unwrap();
    let delete_batch = instance
        .exports
        .get_typed_function::<(i32, i32), i32>(&store, "delete_batch")
        .unwrap();
    let release = instance
        .exports
        .get_typed_function::<(), i32>(&store, "release")
        .unwrap();

    create.call(&mut store).unwrap();
    let first = create_ref.call(&mut store, 64).unwrap();
    // The second entry is zero and the third is valid.
    let third = create_ref.call(&mut store, 72).unwrap();
    assert_eq!(delete_batch.call(&mut store, 64, 3).unwrap(), 1);
    assert_eq!(delete_ref.call(&mut store, first).unwrap(), 1);
    assert_eq!(delete_ref.call(&mut store, third).unwrap(), 1);

    let last_offset = i32::try_from(memory.view(&store).data_size() - 4).unwrap();
    let last = create_ref.call(&mut store, last_offset).unwrap();
    assert_eq!(delete_batch.call(&mut store, last_offset, 2).unwrap(), 1);
    assert_eq!(delete_batch.call(&mut store, last_offset, 1).unwrap(), 0);
    assert_eq!(delete_ref.call(&mut store, last).unwrap(), 1);
    assert_eq!(delete_batch.call(&mut store, 0, 0).unwrap(), 0);
    assert_eq!(release.call(&mut store).unwrap(), 0);
}
