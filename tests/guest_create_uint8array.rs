//! `unofficial_napi_create_uint8array` through real Wasmer dispatch: one
//! import makes a zeroed Uint8Array with the given prototype (an embedder's
//! Buffer.prototype), and the data pointer it returns is guest memory that
//! the array itself views.

#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Instance, Memory, Module, Store, TypedFunction};
use wasmer_napi::NapiCtx;

const SOURCE_PTR: u32 = 4096;
const DATA_OUT: u64 = 40;

const WAT: &str = r#"(module
  (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
    (func $configure (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
    (func $create_env (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
    (func $release_env (param i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_uint8array"
    (func $create_uint8array (param i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_string_utf8"
    (func $create_string (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_run_script" (func $run_script (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_property (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_int32"
    (func $get_int32 (param i32 i32 i32) (result i32)))

  (memory (export "memory") 2)
  (global $env (mut i32) (i32.const 0))
  (global $proto (mut i32) (i32.const 0))
  (data (i32.const 400) "made\00")

  (func (export "create")
    (if (call $configure (i32.const 0)) (then unreachable))
    (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
      (then unreachable))
    (global.set $env (i32.load (i32.const 8))))
  (func (export "eval") (param $ptr i32) (param $len i32) (result i32)
    (if (call $create_string (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28))
      (then unreachable))
    (call $run_script (global.get $env) (i32.load (i32.const 28)) (i32.const 32)))
  ;; Keeps the last eval's result as the prototype to use.
  (func (export "keep_proto") (global.set $proto (i32.load (i32.const 32))))
  (func (export "make") (param $len i32) (param $with_proto i32) (result i32)
    (call $create_uint8array (global.get $env) (local.get $len)
      (select (global.get $proto) (i32.const 0) (local.get $with_proto))
      (i32.const 40) (i32.const 44)))
  (func (export "publish") (result i32)
    (if (call $get_global (global.get $env) (i32.const 48)) (then unreachable))
    (call $set_property (global.get $env) (i32.load (i32.const 48)) (i32.const 400)
      (i32.load (i32.const 44))))
  (func (export "int_result") (result i32)
    (if (call $get_int32 (global.get $env) (i32.load (i32.const 32)) (i32.const 52))
      (then unreachable))
    (i32.load (i32.const 52)))
  (func (export "release") (result i32)
    (call $release_env (i32.load (i32.const 12)) (i32.const 0)))
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
        let (imports, state) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .unwrap();
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

    fn eval(&mut self, source: &str) -> i32 {
        self.memory
            .view(&self.store)
            .write(u64::from(SOURCE_PTR), source.as_bytes())
            .unwrap();
        self.typed::<(i32, i32), i32>("eval")
            .call(&mut self.store, SOURCE_PTR as i32, source.len() as i32)
            .unwrap()
    }

    fn eval_int(&mut self, source: &str) -> i32 {
        assert_eq!(self.eval(source), 0, "{source}");
        self.call0("int_result")
    }

    fn make(&mut self, len: u32, with_proto: bool) -> i32 {
        self.typed::<(i32, i32), i32>("make")
            .call(&mut self.store, len as i32, i32::from(with_proto))
            .unwrap()
    }

    fn read_u32(&self, offset: u64) -> u32 {
        let mut bytes = [0; 4];
        self.memory
            .view(&self.store)
            .read(offset, &mut bytes)
            .unwrap();
        u32::from_le_bytes(bytes)
    }
}

#[test]
fn create_uint8array_is_zeroed_guest_memory_with_its_prototype() {
    let mut guest = Guest::new();
    assert_eq!(
        guest.eval(
            "globalThis.bufferReads = 0;\
             Object.defineProperty(globalThis, 'Buffer', { configurable: true,\
               get() { bufferReads++; throw new Error('Buffer read'); } });\
             globalThis.proto = Object.create(Uint8Array.prototype, { tag: { value: 'b' } })"
        ),
        0
    );
    guest
        .typed::<(), ()>("keep_proto")
        .call(&mut guest.store)
        .unwrap();

    assert_eq!(guest.make(16, true), 0);
    let data = guest.read_u32(DATA_OUT);
    assert_ne!(data, 0);
    let mut bytes = [0xffu8; 16];
    guest
        .memory
        .view(&guest.store)
        .read(u64::from(data), &mut bytes)
        .unwrap();
    assert_eq!(bytes, [0; 16]);
    // The guest writes through its pointer; JS sees it in the array.
    guest
        .memory
        .view(&guest.store)
        .write(u64::from(data) + 3, &[7])
        .unwrap();
    assert_eq!(guest.call0("publish"), 0);
    assert_eq!(
        guest.eval_int(
            "Object.getPrototypeOf(made) === proto && made.tag === 'b' &&\
             made.length === 16 && made[3] === 7 ? 1 : 0"
        ),
        1
    );

    assert_eq!(guest.make(0, false), 0);
    assert_eq!(guest.call0("publish"), 0);
    assert_eq!(
        guest.eval_int(
            "Object.getPrototypeOf(made) === Uint8Array.prototype && made.length === 0 ? 1 : 0"
        ),
        1
    );
    assert_eq!(guest.eval_int("bufferReads"), 0);
    assert_eq!(guest.call0("release"), 0);
}
