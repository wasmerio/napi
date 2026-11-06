//! Function and class names, and private symbol descriptions, that a guest
//! passes as a pointer and a length, through real Wasmer dispatch, the bridge
//! and V8. An explicit length may hold NULs, as in Node, and the engine must
//! get exactly those bytes. It used to get a one-byte empty C string with the
//! guest's length whenever the bytes held a NUL, and read past its end.

#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Imports, Instance, Memory, Module, Store, TypedFunction};
use wasmer_napi::NapiCtx;

const AUTO: u32 = u32::MAX;
const NAME_PTR: u32 = 1024;
const SCRIPT_PTR: u32 = 512;
const BUFFER_PTR: u32 = 65536;
const NAPI_OK: i32 = 0;
const NAPI_INVALID_ARG: i32 = 1;

const WAT: &str = r#"(module
  (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
    (func $configure (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
    (func $create_env (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
    (func $release_env (param i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_private_symbol"
    (func $private_symbol (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_function"
    (func $create_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_define_class"
    (func $define_class (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_named_property (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_string_utf8"
    (func $create_string (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_run_script" (func $run_script (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_string_utf8"
    (func $get_utf8 (param i32 i32 i32 i32 i32) (result i32)))

  ;; 8 env, 12 scope, 16 global, 20 subject, 24 script source, 28 script
  ;; result, 32 length out, 40 "subject", 48 "m", 256 a property descriptor
  ;; for a method `m` (utf8name at +0, method at +8). Scripts from 512, names
  ;; from 1024, the read-back buffer from 64 KiB.
  (memory (export "memory") 4)
  (table (export "__indirect_function_table") 2 funcref)
  (global $env (mut i32) (i32.const 0))
  (global $scope (mut i32) (i32.const 0))
  (data (i32.const 40) "subject\00")
  (data (i32.const 48) "m\00")
  (data (i32.const 256) "\30\00\00\00" "\00\00\00\00" "\01\00\00\00")

  (func $callback (param i32 i32) (result i32) (i32.const 0))
  (elem (i32.const 1) $callback)

  (func (export "create")
    (if (call $configure (i32.const 0)) (then unreachable))
    (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
      (then unreachable))
    (global.set $env (i32.load (i32.const 8)))
    (global.set $scope (i32.load (i32.const 12))))

  (func (export "release") (result i32)
    (call $release_env (global.get $scope) (i32.const 0)))

  ;; The value made last becomes the global `subject`.
  (func $publish (param $status i32) (result i32)
    (if (local.get $status) (then (return (local.get $status))))
    (if (call $get_global (global.get $env) (i32.const 16)) (then unreachable))
    (if (call $set_named_property (global.get $env) (i32.load (i32.const 16))
          (i32.const 40) (i32.load (i32.const 20)))
      (then unreachable))
    (i32.const 0))

  (func (export "function") (param $ptr i32) (param $len i32) (result i32)
    (call $publish
      (call $create_function (global.get $env) (local.get $ptr) (local.get $len)
        (i32.const 1) (i32.const 0) (i32.const 20))))

  (func (export "class") (param $ptr i32) (param $len i32) (result i32)
    (call $publish
      (call $define_class (global.get $env) (local.get $ptr) (local.get $len)
        (i32.const 1) (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 20))))

  ;; A class with a method, which the import builds through another call.
  (func (export "class_with_method") (param $ptr i32) (param $len i32) (result i32)
    (call $publish
      (call $define_class (global.get $env) (local.get $ptr) (local.get $len)
        (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 256) (i32.const 20))))

  (func (export "private_symbol") (param $ptr i32) (param $len i32) (result i32)
    (call $publish
      (call $private_symbol (global.get $env) (local.get $ptr) (local.get $len)
        (i32.const 20))))

  ;; Runs the script of $len bytes at $ptr.
  (func (export "eval") (param $ptr i32) (param $len i32) (result i32)
    (if (call $create_string (global.get $env) (local.get $ptr) (local.get $len)
          (i32.const 24))
      (then unreachable))
    (call $run_script (global.get $env) (i32.load (i32.const 24)) (i32.const 28)))

  ;; The script's result, as UTF-8 into $buf (at most $size bytes with the
  ;; NUL); its length goes to 32.
  (func (export "read") (param $buf i32) (param $size i32) (result i32)
    (call $get_utf8 (global.get $env) (i32.load (i32.const 28)) (local.get $buf)
      (local.get $size) (i32.const 32)))
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

    fn size(&self) -> u32 {
        u32::try_from(self.memory.view(&self.store).data_size()).unwrap()
    }

    fn write(&self, ptr: u32, bytes: &[u8]) {
        self.memory
            .view(&self.store)
            .write(u64::from(ptr), bytes)
            .unwrap();
    }

    /// Makes a function, class or private symbol named by the `len` bytes at
    /// `ptr` (`AUTO`: up to its NUL) and makes it the global `subject`; the
    /// status.
    fn make(&mut self, export: &str, ptr: u32, len: u32) -> i32 {
        let make: TypedFunction<(i32, i32), i32> = self.typed(export);
        make.call(&mut self.store, ptr as i32, len as i32)
            .expect("trapped")
    }

    /// Runs `script` and returns its string result as UTF-8.
    fn eval_string(&mut self, script: &str) -> Vec<u8> {
        self.write(SCRIPT_PTR, script.as_bytes());
        let eval: TypedFunction<(i32, i32), i32> = self.typed("eval");
        assert_eq!(
            eval.call(&mut self.store, SCRIPT_PTR as i32, script.len() as i32)
                .unwrap(),
            NAPI_OK,
            "{script}"
        );
        let read: TypedFunction<(i32, i32), i32> = self.typed("read");
        // Its length first (no buffer), then its bytes.
        assert_eq!(
            read.call(&mut self.store, 0, 0).unwrap(),
            NAPI_OK,
            "{script}"
        );
        let view = self.memory.view(&self.store);
        let mut len = [0u8; 4];
        view.read(32, &mut len).unwrap();
        let len = u32::from_le_bytes(len);
        assert!(BUFFER_PTR + len < self.size(), "read-back buffer too small");
        assert_eq!(
            read.call(&mut self.store, BUFFER_PTR as i32, len as i32 + 1)
                .unwrap(),
            NAPI_OK
        );
        let mut bytes = vec![0u8; len as usize];
        self.memory
            .view(&self.store)
            .read(u64::from(BUFFER_PTR), &mut bytes)
            .unwrap();
        bytes
    }

    /// The name of the function or class made last.
    fn name(&mut self) -> Vec<u8> {
        self.eval_string("subject.name")
    }

    fn release(&mut self) -> i32 {
        let release: TypedFunction<(), i32> = self.typed("release");
        release.call(&mut self.store).expect("release trapped")
    }
}

/// ASCII text of `len` bytes, no NUL.
fn text(len: usize) -> Vec<u8> {
    (0..len).map(|i| b'a' + (i % 26) as u8).collect()
}

/// Names whose explicit length holds NULs: first, last, inside, alone, and
/// one past the first kilobytes. Each is written after a run of `x` so that
/// a read past it would find bytes the name does not have.
fn names_with_nuls() -> Vec<Vec<u8>> {
    let mut long = text(10_000);
    long[5_000] = 0;
    vec![
        b"ab\0cd".to_vec(),
        b"\0xyz".to_vec(),
        b"name\0".to_vec(),
        b"\0".to_vec(),
        b"a\0\0b".to_vec(),
        long,
    ]
}

#[test]
fn function_names_keep_nuls_inside_an_explicit_length() {
    let mut guest = Guest::new();
    for name in names_with_nuls() {
        guest.write(NAME_PTR, &vec![b'x'; name.len() + 64]);
        guest.write(NAME_PTR, &name);
        assert_eq!(guest.make("function", NAME_PTR, name.len() as u32), NAPI_OK);
        assert!(
            guest.name() == name,
            "function named by {} bytes with a NUL",
            name.len()
        );
    }
    assert_eq!(guest.release(), NAPI_OK);
}

#[test]
fn class_names_keep_nuls_inside_an_explicit_length() {
    let mut guest = Guest::new();
    for name in names_with_nuls() {
        guest.write(NAME_PTR, &vec![b'x'; name.len() + 64]);
        guest.write(NAME_PTR, &name);
        for export in ["class", "class_with_method"] {
            assert_eq!(guest.make(export, NAME_PTR, name.len() as u32), NAPI_OK);
            assert!(
                guest.name() == name,
                "{export} named by {} bytes with a NUL",
                name.len()
            );
        }
        assert_eq!(guest.eval_string("typeof subject.prototype.m"), b"function");
    }
    assert_eq!(guest.release(), NAPI_OK);
}

#[test]
fn private_symbol_descriptions_keep_nuls_inside_an_explicit_length() {
    let mut guest = Guest::new();
    for name in names_with_nuls() {
        guest.write(NAME_PTR, &vec![b'x'; name.len() + 64]);
        guest.write(NAME_PTR, &name);
        assert_eq!(
            guest.make("private_symbol", NAME_PTR, name.len() as u32),
            NAPI_OK
        );
        assert!(
            guest.eval_string("subject.description") == name,
            "private symbol described by {} bytes with a NUL",
            name.len()
        );
    }
    assert_eq!(guest.release(), NAPI_OK);
}

#[test]
fn names_without_nuls_and_auto_lengths_are_unchanged() {
    let mut guest = Guest::new();
    guest.write(NAME_PTR, b"plainName\0rest\0");
    for export in ["function", "class", "class_with_method"] {
        // An explicit length, a shorter one, and NAPI_AUTO_LENGTH, which
        // stops at the first NUL.
        assert_eq!(guest.make(export, NAME_PTR, 9), NAPI_OK, "{export}");
        assert_eq!(guest.name(), b"plainName", "{export}");
        assert_eq!(guest.make(export, NAME_PTR, 5), NAPI_OK, "{export}");
        assert_eq!(guest.name(), b"plain", "{export}");
        assert_eq!(guest.make(export, NAME_PTR, AUTO), NAPI_OK, "{export}");
        assert_eq!(guest.name(), b"plainName", "{export}");
        // Empty.
        assert_eq!(guest.make(export, NAME_PTR, 0), NAPI_OK, "{export}");
        assert_eq!(guest.name(), b"", "{export}");
    }
    assert_eq!(guest.make("private_symbol", NAME_PTR, 9), NAPI_OK);
    assert_eq!(guest.eval_string("subject.description"), b"plainName");
    assert_eq!(guest.make("private_symbol", NAME_PTR, AUTO), NAPI_OK);
    assert_eq!(guest.eval_string("subject.description"), b"plainName");
    assert_eq!(guest.release(), NAPI_OK);
}

#[test]
fn null_names_and_lengths_past_memory() {
    let mut guest = Guest::new();
    // A NULL name with a length: Node ignores the length for a function,
    // and refuses a class or a private symbol.
    assert_eq!(guest.make("function", 0, 9), NAPI_OK);
    assert_eq!(guest.name(), b"");
    assert_eq!(guest.make("class", 0, 9), NAPI_INVALID_ARG);
    assert_eq!(guest.make("class_with_method", 0, 9), NAPI_INVALID_ARG);
    assert_eq!(guest.make("private_symbol", 0, 9), NAPI_INVALID_ARG);
    // An explicit length past the memory's end is refused, not read.
    let size = guest.size();
    for export in ["function", "class", "class_with_method", "private_symbol"] {
        assert_eq!(
            guest.make(export, size - 4, 8),
            NAPI_INVALID_ARG,
            "{export}"
        );
    }
    assert_eq!(guest.release(), NAPI_OK);
}
