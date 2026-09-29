use wasmer::{AsStoreMut, Imports, Instance, Memory, MemoryType, Module, SharedMemory, Store};
use wasmer_napi::NapiCtx;

// The cross-memory case is based on the ship-review reproducer: a serialized
// SharedArrayBuffer retains a V8 backing-store pointer into the producer's
// guest memory after its Store has gone away.
const WAT: &str = r#"(module
 (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
 (import "napi" "node_api_create_sharedarraybuffer" (func $sab (param i32 i32 i32 i32) (result i32)))
 (import "napi" "unofficial_napi_serialize_value" (func $serialize (param i32 i32 i32) (result i32)))
 (import "napi" "unofficial_napi_deserialize_value" (func $deserialize (param i32 i32 i32) (result i32)))
 (import "napi" "unofficial_napi_release_serialized_value" (func $release (param i32)))
 (import "napi_extension_wasmer_v0" "unofficial_napi_message_create" (func $message_create (param i32 i32 i32) (result i32)))
 (import "napi_extension_wasmer_v0" "unofficial_napi_message_take" (func $message_take (param i32 i32 i32) (result i32)))
 (import "napi_extension_wasmer_v0" "unofficial_napi_message_drop" (func $message_drop (param i32)))
 (import "napi" "napi_get_global" (func $global (param i32 i32) (result i32)))
 (import "napi" "napi_set_named_property" (func $set (param i32 i32 i32 i32) (result i32)))
 (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
 (import "napi" "napi_run_script" (func $run (param i32 i32 i32) (result i32)))
 (import "napi" "napi_get_value_int32" (func $int (param i32 i32 i32) (result i32)))
 (memory (export "memory") 1)
 (global $env (mut i32) (i32.const 0))
 (data (i32.const 100) "sab\00")
 (data (i32.const 110) "new Uint8Array(sab)[0]\00")
 (func (export "init") (result i32) (local $status i32)
   (local.set $status (call $create (i32.const 8) (i32.const 4) (i32.const 8)))
   (global.set $env (i32.load (i32.const 4)))
   (local.get $status))
 (func (export "produce_legacy") (result i32) (local $env i32)
   (local.set $env (global.get $env))
   (if (call $sab (local.get $env) (i32.const 8) (i32.const 16) (i32.const 20)) (then unreachable))
   (i32.store8 (i32.load (i32.const 16)) (i32.const 42))
   (if (call $serialize (local.get $env) (i32.load (i32.const 20)) (i32.const 24)) (then unreachable))
   (i32.load (i32.const 24)))
 (func (export "produce_current") (result i32) (local $env i32)
   (local.set $env (global.get $env))
   (if (call $sab (local.get $env) (i32.const 8) (i32.const 16) (i32.const 20)) (then unreachable))
   (i32.store8 (i32.load (i32.const 16)) (i32.const 42))
   (if (call $message_create (local.get $env) (i32.load (i32.const 20)) (i32.const 24)) (then unreachable))
   (i32.load (i32.const 24)))
 (func (export "read_legacy") (param $id i32) (result i32)
   (call $deserialize (global.get $env) (local.get $id) (i32.const 20)))
 (func (export "read_current") (param $id i32) (result i32)
   (call $message_take (global.get $env) (local.get $id) (i32.const 20)))
 (func (export "release_legacy") (param $id i32) (call $release (local.get $id)))
 (func (export "release_current") (param $id i32) (call $message_drop (local.get $id)))
 (func (export "use_sab") (result i32) (local $env i32)
   (local.set $env (global.get $env))
   (if (call $global (local.get $env) (i32.const 24)) (then unreachable))
   (if (call $set (local.get $env) (i32.load (i32.const 24)) (i32.const 100) (i32.load (i32.const 20))) (then unreachable))
   (if (call $string (local.get $env) (i32.const 110) (i32.const -1) (i32.const 28)) (then unreachable))
   (if (call $run (local.get $env) (i32.load (i32.const 28)) (i32.const 32)) (then unreachable))
   (if (call $int (local.get $env) (i32.load (i32.const 32)) (i32.const 36)) (then unreachable))
   (i32.load (i32.const 36))))"#;

fn setup(ctx: &NapiCtx, shared: Option<SharedMemory>) -> (Store, Instance) {
    let mut store = Store::default();
    let source = if shared.is_some() {
        WAT.replace(
            "(memory (export \"memory\") 1)",
            "(import \"env\" \"memory\" (memory 1 1024 shared)) (export \"memory\" (memory 0))",
        )
    } else {
        WAT.to_string()
    };
    let module = Module::new(&store, wat::parse_str(source).unwrap()).unwrap();
    let mut imports = Imports::new();
    if let Some(shared) = shared {
        imports.define("env", "memory", shared.attach(&mut store));
    }
    let hooks = ctx.runtime_hooks();
    let state = hooks
        .add_imports(&module, &mut store.as_store_mut(), &mut imports)
        .unwrap();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    hooks
        .configure_instance(&module, &mut store.as_store_mut(), &instance, None, state)
        .unwrap();
    assert_eq!(call0(&mut store, &instance, "init"), 0);
    (store, instance)
}

fn call0(store: &mut Store, instance: &Instance, name: &str) -> i32 {
    instance
        .exports
        .get_typed_function::<(), i32>(&*store, name)
        .unwrap()
        .call(store)
        .unwrap()
}

fn call1(store: &mut Store, instance: &Instance, name: &str, id: i32) -> i32 {
    instance
        .exports
        .get_typed_function::<i32, i32>(&*store, name)
        .unwrap()
        .call(store, id)
        .unwrap()
}

fn release(store: &mut Store, instance: &Instance, name: &str, id: i32) {
    instance
        .exports
        .get_typed_function::<i32, ()>(&*store, name)
        .unwrap()
        .call(store, id)
        .unwrap();
}

#[test]
fn serialized_sab_rejects_a_different_memory_after_producer_teardown() {
    for (produce, read, release_name) in [
        ("produce_legacy", "read_legacy", "release_legacy"),
        ("produce_current", "read_current", "release_current"),
    ] {
        let ctx = NapiCtx::default();
        let (mut consumer, consumer_instance) = setup(&ctx, None);
        let consumer_bytes = ctx.budget().snapshot().wasm_linear;
        let id = {
            let (mut producer, producer_instance) = setup(&ctx, None);
            call0(&mut producer, &producer_instance, produce)
        };
        let after_teardown = ctx.budget().snapshot();
        assert!(after_teardown.wasm_linear > consumer_bytes);
        assert!(after_teardown.serialized_message > 0);
        assert_ne!(call1(&mut consumer, &consumer_instance, read, id), 0);
        release(&mut consumer, &consumer_instance, release_name, id);
        let after_release = ctx.budget().snapshot();
        assert_eq!(after_release.serialized_message, 0);
        assert_eq!(after_release.wasm_linear, consumer_bytes);
    }
}

#[test]
fn serialized_sab_survives_producer_teardown_and_message_release_in_same_memory() {
    for (produce, read, release_name) in [
        ("produce_legacy", "read_legacy", "release_legacy"),
        ("produce_current", "read_current", "release_current"),
    ] {
        let ctx = NapiCtx::default();
        let mut origin_store = Store::default();
        let memory = Memory::new(&mut origin_store, MemoryType::new(1, Some(1024), true)).unwrap();
        let shared = memory.as_shared(&origin_store).unwrap();
        drop(origin_store);
        let (mut consumer, consumer_instance) = setup(&ctx, Some(shared.clone()));
        let id = {
            let (mut producer, producer_instance) = setup(&ctx, Some(shared.clone()));
            call0(&mut producer, &producer_instance, produce)
        };
        drop(shared);
        let queued = ctx.budget().snapshot();
        assert!(queued.wasm_linear > 0);
        assert!(queued.serialized_message > 0);
        assert_eq!(call1(&mut consumer, &consumer_instance, read, id), 0);
        assert_eq!(call0(&mut consumer, &consumer_instance, "use_sab"), 42);
        release(&mut consumer, &consumer_instance, release_name, id);
        assert_eq!(call0(&mut consumer, &consumer_instance, "use_sab"), 42);
        assert_eq!(ctx.budget().snapshot().serialized_message, 0);
        assert!(ctx.budget().snapshot().wasm_linear > 0);
        drop(consumer);
        assert_eq!(ctx.budget().snapshot().wasm_linear, 0);
    }
}

#[cfg(target_os = "linux")]
fn has_mapping_at(address: usize) -> bool {
    // Query the kernel's mapping table. After every Store has dropped, this
    // test never dereferences the guest address just to check its lifetime.
    std::fs::read_to_string("/proc/self/maps")
        .unwrap()
        .lines()
        .filter_map(|line| line.split_whitespace().next())
        .filter_map(|range| range.split_once('-'))
        .filter_map(|(start, end)| {
            Some((
                usize::from_str_radix(start, 16).ok()?,
                usize::from_str_radix(end, 16).ok()?,
            ))
        })
        .any(|(start, end)| start <= address && address < end)
}

#[cfg(target_os = "linux")]
#[test]
fn queued_sab_alone_pins_mapping_until_release_or_host_stop() {
    for (produce, stop_host) in [("produce_legacy", false), ("produce_current", true)] {
        let ctx = NapiCtx::default();
        let budget = ctx.budget();
        let (base, shared) = {
            let mut origin_store = Store::default();
            let memory =
                Memory::new(&mut origin_store, MemoryType::new(1, Some(1024), true)).unwrap();
            let base = memory.view(&origin_store).data_ptr() as usize;
            let shared = memory.as_shared(&origin_store).unwrap();
            (base, shared)
        };
        let message_id = {
            let (mut producer, instance) = setup(&ctx, Some(shared.clone()));
            call0(&mut producer, &instance, produce)
        };
        assert!(message_id > 0);
        drop(shared);

        // The queued payload is now the sole owner of its detached mapping
        // and GuestHeap charge. No Store or external SharedMemory remains.
        assert!(has_mapping_at(base));
        let queued = budget.snapshot();
        assert!(queued.wasm_linear > 0);
        assert!(queued.serialized_message > 0);

        if stop_host {
            ctx.runtime_control().terminate_all();
        } else {
            drop(ctx);
        }
        assert!(!has_mapping_at(base));
        let released = budget.snapshot();
        assert_eq!(released.wasm_linear, 0);
        assert_eq!(released.serialized_message, 0);
    }
}
