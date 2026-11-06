//! Imports reading and writing guest memory through real Wasmer dispatch,
//! the bridge and V8, at the edges the guest-heap fast path checks: ranges
//! ending exactly at the end of memory, in pages the guest grew after the
//! env was set up; ranges one byte past it; names on both sides of the
//! in-place C-string scan; unaligned addresses and lengths.

#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Imports, Instance, Memory, Module, Store, TypedFunction};
use wasmer_napi::NapiCtx;

/// `napi_invalid_arg`.
const NAPI_INVALID_ARG: i32 = 1;
/// Where [`Guest::number`] places script text: odd, so every copy starts
/// unaligned.
const SOURCE_PTR: u32 = 4097;
/// Where property names go: odd too, and clear of the script text.
const NAME_PTR: u32 = 16385;

const WAT: &str = r#"(module
  (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
    (func $configure (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
    (func $create_env (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
    (func $release_env (param i32 i32) (result i32)))
  (import "napi" "napi_create_string_utf8"
    (func $create_string (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_run_script" (func $run_script (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_int32" (func $get_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_string_utf8"
    (func $get_string (param i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_int32" (func $create_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_named_property (param i32 i32 i32 i32) (result i32)))

  ;; 8 env, 12 scope, 28 string, 32 script result, 48 global, 52 int value,
  ;; 72 int out.
  (memory (export "memory") 2)
  (global $env (mut i32) (i32.const 0))
  (global $scope (mut i32) (i32.const 0))

  (func (export "create")
    (if (call $configure (i32.const 0)) (then unreachable))
    (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
      (then unreachable))
    (global.set $env (i32.load (i32.const 8)))
    (global.set $scope (i32.load (i32.const 12))))

  (func (export "release") (result i32)
    (call $release_env (global.get $scope) (i32.const 0)))

  (func (export "grow") (param $pages i32) (result i32)
    (memory.grow (local.get $pages)))

  ;; Runs the script at [ptr, ptr + len); returns the first failing status.
  (func (export "eval") (param $ptr i32) (param $len i32) (result i32)
    (local $status i32)
    (local.set $status
      (call $create_string (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28)))
    (if (local.get $status) (then (return (local.get $status))))
    (call $run_script (global.get $env) (i32.load (i32.const 28)) (i32.const 32)))

  (func (export "result_int") (result i32)
    (if (call $get_int32 (global.get $env) (i32.load (i32.const 32)) (i32.const 72))
      (then (return (i32.const -999999))))
    (i32.load (i32.const 72)))

  ;; Copies the last script result, a string, to [buf, buf + size) and its
  ;; length to len_out.
  (func (export "result_string") (param $buf i32) (param $size i32) (param $len_out i32)
    (result i32)
    (call $get_string (global.get $env) (i32.load (i32.const 32)) (local.get $buf)
      (local.get $size) (local.get $len_out)))

  ;; globalThis[<C string at name>] = value.
  (func (export "set_global") (param $name i32) (param $value i32) (result i32)
    (if (call $get_global (global.get $env) (i32.const 48)) (then unreachable))
    (if (call $create_int32 (global.get $env) (local.get $value) (i32.const 52))
      (then unreachable))
    (call $set_named_property (global.get $env) (i32.load (i32.const 48)) (local.get $name)
      (i32.load (i32.const 52))))
)"#;

struct Guest {
    store: Store,
    instance: Instance,
    memory: Memory,
}

impl Guest {
    fn new(ctx: &NapiCtx) -> Self {
        let mut store = Store::default();
        let module = Module::new(&store, wat::parse_str(WAT).unwrap()).unwrap();
        let hooks = ctx.runtime_hooks();
        let (imports, state): (Imports, _) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .expect("failed to create N-API imports");
        let instance = Instance::new(&mut store, &module, &imports).expect("instantiate");
        let memory = instance.exports.get_memory("memory").unwrap().clone();
        hooks
            .configure_instance(
                &module,
                &mut store.as_store_mut(),
                &instance,
                Some(&memory),
                state,
            )
            .expect("configure instance");
        let mut guest = Self {
            store,
            instance,
            memory,
        };
        let create: TypedFunction<(), ()> = guest.typed("create");
        create.call(&mut guest.store).expect("create trapped");
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

    /// Grows the memory from inside the guest, as its own allocator would.
    fn grow(&mut self, pages: i32) {
        let grow: TypedFunction<i32, i32> = self.typed("grow");
        assert_ne!(grow.call(&mut self.store, pages).unwrap(), -1, "grow");
    }

    /// The memory's current length in bytes.
    fn end(&self) -> u32 {
        u32::try_from(self.memory.view(&self.store).data_size()).unwrap()
    }

    fn write(&self, at: u32, bytes: &[u8]) {
        self.memory
            .view(&self.store)
            .write(u64::from(at), bytes)
            .unwrap();
    }

    fn read(&self, at: u32, len: usize) -> Vec<u8> {
        let mut out = vec![0; len];
        self.memory
            .view(&self.store)
            .read(u64::from(at), &mut out)
            .unwrap();
        out
    }

    /// `napi_create_string_utf8` over `[at, at + len)`, then
    /// `napi_run_script`; returns the first failing status.
    fn eval_range(&mut self, at: u32, len: u32) -> i32 {
        let eval: TypedFunction<(i32, i32), i32> = self.typed("eval");
        eval.call(&mut self.store, at as i32, len as i32)
            .expect("eval trapped")
    }

    /// Places `source` at `at` and runs it; returns the status.
    fn eval_at(&mut self, at: u32, source: &str) -> i32 {
        self.write(at, source.as_bytes());
        self.eval_range(at, source.len() as u32)
    }

    /// Runs `source` and returns its int32 result.
    fn number(&mut self, source: &str) -> i32 {
        assert_eq!(self.eval_at(SOURCE_PTR, source), 0, "script failed");
        let result: TypedFunction<(), i32> = self.typed("result_int");
        result.call(&mut self.store).unwrap()
    }

    /// `napi_get_value_string_utf8` of the last script result into
    /// `[buf, buf + size)`, its length to `len_out`; returns the status.
    fn result_string(&mut self, buf: u32, size: u32, len_out: u32) -> i32 {
        let call: TypedFunction<(i32, i32, i32), i32> = self.typed("result_string");
        call.call(&mut self.store, buf as i32, size as i32, len_out as i32)
            .unwrap()
    }

    /// `globalThis[<C string at name>] = value`; returns the status.
    fn set_global(&mut self, name: u32, value: i32) -> i32 {
        let call: TypedFunction<(i32, i32), i32> = self.typed("set_global");
        call.call(&mut self.store, name as i32, value).unwrap()
    }

    fn release(&mut self) -> i32 {
        let release: TypedFunction<(), i32> = self.typed("release");
        release.call(&mut self.store).expect("release trapped")
    }
}

/// Ranges that end exactly at the end of memory are read and written whole,
/// in a page the guest grew after the env was set up. A range one byte past
/// the end is refused, and a refused write leaves the bytes before the end
/// as they were.
#[test]
fn ranges_up_to_the_end_of_memory() {
    let ctx = NapiCtx::default();
    let mut guest = Guest::new(&ctx);
    guest.grow(1);
    let end = guest.end();

    // A script in the last bytes of memory.
    let source = "'edge' + 1";
    assert_eq!(guest.eval_at(end - source.len() as u32, source), 0);
    // Its result into the last 6 bytes, the length to an unaligned slot.
    assert_eq!(guest.result_string(end - 6, 6, 201), 0);
    assert_eq!(guest.read(end - 6, 6), b"edge1\0");
    assert_eq!(guest.read(201, 4), 5u32.to_le_bytes());

    // A name in the last bytes, shorter than the in-place scan.
    guest.write(end - 5, b"edge\0");
    assert_eq!(guest.set_global(end - 5, 7), 0);
    assert_eq!(guest.number("edge"), 7);

    // One byte past the end: the script, and a name with no terminator
    // before the end.
    guest.write(end - 4, b"1 + ");
    assert_eq!(guest.eval_range(end - 4, 5), NAPI_INVALID_ARG);
    guest.write(end - 3, b"abc");
    assert_eq!(guest.set_global(end - 3, 1), NAPI_INVALID_ARG);

    // "edge1" and its terminator do not fit in the last 3 bytes; nothing
    // is written there.
    assert_eq!(guest.eval_at(SOURCE_PTR, source), 0);
    guest.result_string(end - 3, 6, 0);
    assert_eq!(guest.read(end - 3, 3), b"abc");

    // The guest grows again: the old end is now in the middle.
    guest.grow(1);
    assert_eq!(guest.end(), end + 65536);
    assert_eq!(guest.eval_at(end - 4, "40 + 2"), 0);
    let result: TypedFunction<(), i32> = guest.typed("result_int");
    assert_eq!(result.call(&mut guest.store).unwrap(), 42);

    assert_eq!(guest.release(), 0);
}

/// Property names shorter than, as long as, and longer than the 256 bytes
/// the bridge scans in place arrive whole.
#[test]
fn names_on_both_sides_of_the_in_place_scan() {
    let ctx = NapiCtx::default();
    let mut guest = Guest::new(&ctx);
    for len in [1usize, 7, 8, 9, 255, 256, 257, 300, 4000] {
        let name = format!("{}{}", "n".repeat(len - 1), len % 10);
        guest.write(NAME_PTR, name.as_bytes());
        guest.write(NAME_PTR + len as u32, b"\0");
        assert_eq!(guest.set_global(NAME_PTR, len as i32), 0, "name of {len}");
        let read_back = format!("globalThis['{name}']");
        assert_eq!(guest.number(&read_back), len as i32, "name of {len}");
    }
    assert_eq!(guest.release(), 0);
}
