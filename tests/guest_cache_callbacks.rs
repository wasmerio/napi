//! Cache serialization makes a plain Uint8Array and runs no JS, so it never
//! reaches the guest: not through a global Buffer, its getter or Buffer.from
//! (wasmerio/napi#146). Both imports keep their callback context all the same.
//! Exercise them through real Wasmer dispatch with a guest-native Buffer
//! getter installed that would throw or exit the guest if it ran.

#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Function, Instance, Memory, Module, RuntimeError, Store, TypedFunction};
use wasmer_napi::NapiCtx;
use wasmer_wasix::WasiError;

const SOURCE_PTR: u32 = 4096;
const OUTPUT_PTR: u64 = 208;
const UNWRITTEN: u32 = 0x1234;
const EXPORTS: [&str; 2] = ["bytecode_cache", "module_cache"];

const WAT: &str = r#"(module
  (import "test" "exit" (func $exit))
  (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
    (func $configure (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
    (func $create_env (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
    (func $release_env (param i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_bytecode_open"
    (func $bytecode_open (param i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_bytecode_serialize"
    (func $serialize (param i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_module_wrap_create"
    (func $module_create (param i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_module_wrap_create_cached_data"
    (func $module_cache (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_string_utf8"
    (func $create_string (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_run_script" (func $run_script (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_object" (func $create_object (param i32 i32) (result i32)))
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_property (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_function"
    (func $create_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_cb_info"
    (func $get_cb_info (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_throw_error" (func $throw (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_and_clear_last_exception"
    (func $clear_exception (param i32 i32) (result i32)))

  (memory (export "memory") 2)
  (table (export "__indirect_function_table") 2 funcref)
  (global $env (mut i32) (i32.const 0))
  (global $bytecode (mut i32) (i32.const 0))
  (global $module (mut i32) (i32.const 0))
  (global $mode (mut i32) (i32.const 0))
  (global $calls (mut i32) (i32.const 0))
  (data (i32.const 100) "from\00")
  (data (i32.const 112) "cache callback failed\00")
  (data (i32.const 320) "21 + 21")
  (data (i32.const 352) "export const value = 42;")
  (data (i32.const 384) "cache.js")
  (data (i32.const 400) "cache.mjs")

  (func $string (param $ptr i32) (param $len i32) (result i32)
    (if (call $create_string (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28))
      (then unreachable))
    (i32.load (i32.const 28)))
  (func (export "create")
    (if (call $configure (i32.const 0)) (then unreachable))
    (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
      (then unreachable))
    (global.set $env (i32.load (i32.const 8)))
    (if (call $get_global (global.get $env) (i32.const 48)) (then unreachable))
    (if (call $create_function (global.get $env) (i32.const 100) (i32.const 4)
          (i32.const 1) (i32.const 0) (i32.const 52)) (then unreachable))
    (if (call $set_property (global.get $env) (i32.load (i32.const 48))
          (i32.const 100) (i32.load (i32.const 52))) (then unreachable)))
  (func (export "eval") (param $ptr i32) (param $len i32) (result i32)
    (call $run_script (global.get $env) (call $string (local.get $ptr) (local.get $len))
      (i32.const 32)))

  (func (export "prepare")
    ;; Version-2 bytecode options (52 bytes); all other fields start zero.
    (i32.store (i32.const 128) (i32.const 52))
    (i32.store (i32.const 132) (i32.const 2))
    (i32.store (i32.const 136) (call $string (i32.const 320) (i32.const 7)))
    (i32.store (i32.const 140) (call $string (i32.const 384) (i32.const 8)))
    (if (call $bytecode_open (global.get $env) (i32.const 128) (i32.const 192))
      (then unreachable))
    (global.set $bytecode (i32.load (i32.const 192)))
    ;; Source descriptor and version-2 source module options (44 bytes).
    (i32.store (i32.const 20) (call $string (i32.const 352) (i32.const 24)))
    (if (call $create_object (global.get $env) (i32.const 48)) (then unreachable))
    (i32.store (i32.const 256) (i32.const 44))
    (i32.store (i32.const 260) (i32.const 2))
    (i32.store (i32.const 264) (i32.const 1))
    (i32.store (i32.const 268) (i32.load (i32.const 48)))
    (i32.store (i32.const 272) (call $string (i32.const 400) (i32.const 9)))
    (i32.store (i32.const 280) (i32.const 16))
    (if (call $module_create (global.get $env) (i32.const 256) (i32.const 304))
      (then unreachable))
    (global.set $module (i32.load (i32.const 304))))

  (func (export "bytecode_cache") (param $mode i32) (result i32)
    (global.set $mode (local.get $mode))
    (i32.store (i32.const 208) (i32.const 4660))
    (call $serialize (global.get $env) (global.get $bytecode) (i32.const 208)))
  (func (export "module_cache") (param $mode i32) (result i32)
    (global.set $mode (local.get $mode))
    (i32.store (i32.const 208) (i32.const 4660))
    (call $module_cache (global.get $env) (global.get $module) (i32.const 208)))
  (func (export "calls") (result i32) (global.get $calls))
  (func (export "clear_exception") (result i32)
    (call $clear_exception (global.get $env) (i32.const 32)))
  (func (export "release") (result i32)
    (call $release_env (i32.load (i32.const 12)) (i32.const 0)))

  (func $from (param $env i32) (param $info i32) (result i32)
    (global.set $calls (i32.add (global.get $calls) (i32.const 1)))
    (if (i32.eq (global.get $mode) (i32.const 2)) (then (call $exit)))
    (if (i32.eq (global.get $mode) (i32.const 1))
      (then
        (drop (call $throw (local.get $env) (i32.const 0) (i32.const 112)))
        (return (i32.const 0))))
    (i32.store (i32.const 200) (i32.const 1))
    (if (call $get_cb_info (local.get $env) (local.get $info) (i32.const 200)
          (i32.const 204) (i32.const 0) (i32.const 0)) (then unreachable))
    (i32.load (i32.const 204)))
  (elem (i32.const 1) $from)
)"#;

struct Guest {
    store: Store,
    instance: Instance,
    memory: Memory,
}

impl Guest {
    fn new() -> Self {
        let mut store = Store::default();
        let module = Module::new(&store, wat::parse_str(WAT).unwrap()).unwrap();
        let ctx = NapiCtx::default();
        let hooks = ctx.runtime_hooks();
        let (mut imports, state) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .unwrap();
        imports.define(
            "test",
            "exit",
            Function::new_typed(&mut store, || -> Result<(), WasiError> {
                Err(WasiError::Exit(37i32.into()))
            }),
        );
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        let memory = instance.exports.get_memory("memory").unwrap().clone();
        hooks
            .configure_instance(
                &module,
                &mut store.as_store_mut(),
                &instance,
                Some(&memory),
                state,
            )
            .unwrap();
        let mut guest = Self {
            store,
            instance,
            memory,
        };
        guest
            .typed::<(), ()>("create")
            .call(&mut guest.store)
            .unwrap();
        let source = "Object.defineProperty(globalThis, 'Buffer', \
                      { get: from, configurable: true });";
        guest
            .memory
            .view(&guest.store)
            .write(u64::from(SOURCE_PTR), source.as_bytes())
            .unwrap();
        assert_eq!(
            guest
                .typed::<(i32, i32), i32>("eval")
                .call(&mut guest.store, SOURCE_PTR as i32, source.len() as i32)
                .unwrap(),
            0
        );
        guest
            .typed::<(), ()>("prepare")
            .call(&mut guest.store)
            .unwrap();
        guest
    }

    fn typed<A: wasmer::WasmTypeList, R: wasmer::WasmTypeList>(
        &self,
        name: &str,
    ) -> TypedFunction<A, R> {
        self.instance
            .exports
            .get_typed_function(&self.store, name)
            .unwrap()
    }

    fn call0(&mut self, name: &str) -> i32 {
        self.typed::<(), i32>(name).call(&mut self.store).unwrap()
    }

    fn cache(&mut self, name: &str, mode: i32) -> Result<i32, RuntimeError> {
        self.typed::<i32, i32>(name).call(&mut self.store, mode)
    }

    fn output(&self) -> u32 {
        let mut bytes = [0; 4];
        self.memory
            .view(&self.store)
            .read(OUTPUT_PTR, &mut bytes)
            .unwrap();
        u32::from_le_bytes(bytes)
    }
}

#[test]
fn cache_imports_never_run_guest_code() {
    let mut guest = Guest::new();
    // Mode 0 returns from the getter, 1 throws from it, 2 exits the guest.
    for mode in 0..3 {
        for export in EXPORTS {
            assert_eq!(guest.cache(export, mode).unwrap(), 0, "{export} mode {mode}");
            assert_ne!(guest.output(), 0, "{export} mode {mode}");
            assert_ne!(guest.output(), UNWRITTEN, "{export} mode {mode}");
        }
    }
    assert_eq!(guest.call0("calls"), 0);
    assert_eq!(guest.call0("release"), 0);
}
