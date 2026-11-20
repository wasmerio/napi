//! The imports' short paths through guest memory, against the checked paths
//! they stand in for, through real Wasmer dispatch, the bridge and V8:
//! strings copied into the import's own frame or written by the engine
//! straight into the guest's buffer, call arguments read without an
//! allocation, and `napi_get_cb_info` with more arguments than its inline
//! room. Each case sits on both sides of a limit (the inline size, the end of
//! guest memory, the memory's growth), where the two paths meet.

#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Imports, Instance, Memory, Module, Pages, Store, TypedFunction};
use wasmer_napi::NapiCtx;

const AUTO: u32 = u32::MAX;
const SCRIPT_PTR: u32 = 4096;
const TEXT_PTR: u32 = 16384;
const ARGV_PTR: u32 = 32768;
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
  (import "napi" "napi_create_string_utf8"
    (func $create_utf8 (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_string_latin1"
    (func $create_latin1 (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_string_utf8"
    (func $get_utf8 (param i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_string_latin1"
    (func $get_latin1 (param i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_string_utf16"
    (func $get_utf16 (param i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_run_script" (func $run_script (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_int32" (func $get_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_int32" (func $create_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_undefined" (func $get_undefined (param i32 i32) (result i32)))
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_named_property (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_named_property"
    (func $get_named_property (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_function"
    (func $create_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_cb_info"
    (func $get_cb_info (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_call_function"
    (func $call_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_new_instance"
    (func $new_instance (param i32 i32 i32 i32 i32) (result i32)))

  ;; 8 env, 12 scope, 16 global, 20 function, 24 the subject (the value made
  ;; or returned last), 28 script source, 32 number or length out, 36
  ;; undefined, 40 "f", 44 "subject", 60 argc in and out, 64 this, 68 a
  ;; scratch int, 72 callback result. The callback's argv from 2048. Scripts
  ;; from 4096, text from 16 KiB, call arguments from 32 KiB, read-back
  ;; buffers from 64 KiB.
  (memory (export "memory") 4)
  (table (export "__indirect_function_table") 2 funcref)
  (global $env (mut i32) (i32.const 0))
  (global $scope (mut i32) (i32.const 0))
  (global $wanted (mut i32) (i32.const 0))
  (data (i32.const 40) "f\00")
  (data (i32.const 44) "subject\00")

  ;; f(...): asks for $wanted arguments and returns the sum of those that
  ;; are numbers. The count the import wrote back stays at 60.
  (func $f (param $env i32) (param $info i32) (result i32)
    (local $i i32) (local $sum i32)
    (i32.store (i32.const 60) (global.get $wanted))
    (if (call $get_cb_info (local.get $env) (local.get $info) (i32.const 60) (i32.const 2048)
          (i32.const 64) (i32.const 0))
      (then unreachable))
    (block $done
      (loop $next
        (br_if $done (i32.ge_u (local.get $i) (global.get $wanted)))
        (if (i32.eqz (call $get_int32 (local.get $env)
              (i32.load (i32.add (i32.const 2048) (i32.shl (local.get $i) (i32.const 2))))
              (i32.const 68)))
          (then (local.set $sum (i32.add (local.get $sum) (i32.load (i32.const 68))))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $next)))
    (if (call $create_int32 (local.get $env) (local.get $sum) (i32.const 72))
      (then unreachable))
    (i32.load (i32.const 72)))
  (elem (i32.const 1) $f)

  (func (export "create")
    (if (call $configure (i32.const 0)) (then unreachable))
    (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
      (then unreachable))
    (global.set $env (i32.load (i32.const 8)))
    (global.set $scope (i32.load (i32.const 12)))
    (if (call $get_undefined (global.get $env) (i32.const 36)) (then unreachable))
    (if (call $get_global (global.get $env) (i32.const 16)) (then unreachable))
    (if (call $create_function (global.get $env) (i32.const 40) (i32.const 1)
          (i32.const 1) (i32.const 0) (i32.const 20))
      (then unreachable))
    (if (call $set_named_property (global.get $env) (i32.load (i32.const 16))
          (i32.const 40) (i32.load (i32.const 20)))
      (then unreachable)))

  (func (export "release") (result i32)
    (call $release_env (global.get $scope) (i32.const 0)))

  (func (export "want") (param $n i32) (global.set $wanted (local.get $n)))

  (func (export "utf8") (param $ptr i32) (param $len i32) (result i32)
    (call $create_utf8 (global.get $env) (local.get $ptr) (local.get $len) (i32.const 24)))
  (func (export "latin1") (param $ptr i32) (param $len i32) (result i32)
    (call $create_latin1 (global.get $env) (local.get $ptr) (local.get $len) (i32.const 24)))

  ;; The subject as a string into $buf of $size units; its length goes to 32.
  (func (export "read_utf8") (param $buf i32) (param $size i32) (result i32)
    (call $get_utf8 (global.get $env) (i32.load (i32.const 24)) (local.get $buf)
      (local.get $size) (i32.const 32)))
  (func (export "read_latin1") (param $buf i32) (param $size i32) (result i32)
    (call $get_latin1 (global.get $env) (i32.load (i32.const 24)) (local.get $buf)
      (local.get $size) (i32.const 32)))
  (func (export "read_utf16") (param $buf i32) (param $size i32) (result i32)
    (call $get_utf16 (global.get $env) (i32.load (i32.const 24)) (local.get $buf)
      (local.get $size) (i32.const 32)))

  ;; Runs the script of $len bytes at $ptr; its result is the subject.
  (func (export "eval") (param $ptr i32) (param $len i32) (result i32)
    (if (call $create_utf8 (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28))
      (then unreachable))
    (call $run_script (global.get $env) (i32.load (i32.const 28)) (i32.const 24)))

  ;; The subject as an int32, to 32.
  (func (export "int") (result i32)
    (call $get_int32 (global.get $env) (i32.load (i32.const 24)) (i32.const 32)))

  ;; The global named by the NUL-terminated string at $name is the subject.
  (func (export "global") (param $name i32) (result i32)
    (call $get_named_property (global.get $env) (i32.load (i32.const 16)) (local.get $name)
      (i32.const 24)))

  ;; $count int32 values 1, 2, ... as ids from $argv.
  (func (export "ints") (param $argv i32) (param $count i32)
    (local $i i32)
    (block $done
      (loop $next
        (br_if $done (i32.ge_u (local.get $i) (local.get $count)))
        (if (call $create_int32 (global.get $env) (i32.add (local.get $i) (i32.const 1))
              (i32.add (local.get $argv) (i32.shl (local.get $i) (i32.const 2))))
          (then unreachable))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $next))))

  ;; Calls the subject, or makes an instance of it, with the $argc ids at
  ;; $argv; the result is the subject.
  (func (export "call") (param $argc i32) (param $argv i32) (result i32)
    (call $call_function (global.get $env) (i32.load (i32.const 36))
      (i32.load (i32.const 24)) (local.get $argc) (local.get $argv) (i32.const 24)))
  (func (export "new") (param $argc i32) (param $argv i32) (result i32)
    (call $new_instance (global.get $env) (i32.load (i32.const 24)) (local.get $argc)
      (local.get $argv) (i32.const 24)))
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

    fn read(&self, ptr: u32, len: usize) -> Vec<u8> {
        let mut bytes = vec![0; len];
        self.memory
            .view(&self.store)
            .read(u64::from(ptr), &mut bytes)
            .unwrap();
        bytes
    }

    fn u32_at(&self, ptr: u32) -> u32 {
        u32::from_le_bytes(self.read(ptr, 4).try_into().unwrap())
    }

    /// Calls the export `name(a, b) -> status`.
    fn call2(&mut self, name: &str, a: u32, b: u32) -> i32 {
        let f: TypedFunction<(i32, i32), i32> = self.typed(name);
        f.call(&mut self.store, a as i32, b as i32)
            .expect("trapped")
    }

    fn eval(&mut self, script: &str) -> i32 {
        self.write(SCRIPT_PTR, script.as_bytes());
        self.call2("eval", SCRIPT_PTR, script.len() as u32)
    }

    /// The subject as an int32.
    fn int(&mut self) -> i32 {
        let f: TypedFunction<(), i32> = self.typed("int");
        assert_eq!(f.call(&mut self.store).unwrap(), NAPI_OK);
        self.u32_at(32) as i32
    }

    /// The global `name` becomes the subject, looked up by a name the guest
    /// holds at `ptr`.
    fn global(&mut self, name: &str, ptr: u32) {
        let mut bytes = name.as_bytes().to_vec();
        bytes.push(0);
        self.write(ptr, &bytes);
        let f: TypedFunction<i32, i32> = self.typed("global");
        assert_eq!(f.call(&mut self.store, ptr as i32).unwrap(), NAPI_OK);
    }

    /// Grows the memory by a page of the test's own (the pages before it
    /// are the guest heap's arena) and returns the memory's new size.
    fn own_end(&mut self) -> u32 {
        self.memory.grow(&mut self.store, Pages(1)).unwrap();
        self.size()
    }

    /// The subject string, read back as UTF-8 through a large buffer.
    fn subject_utf8(&mut self) -> Vec<u8> {
        assert_eq!(self.call2("read_utf8", BUFFER_PTR, 60_000), NAPI_OK);
        let len = self.u32_at(32) as usize;
        let bytes = self.read(BUFFER_PTR, len + 1);
        assert_eq!(bytes[len], 0, "the string's NUL");
        bytes[..len].to_vec()
    }
}

/// Text of `len` bytes that tells its bytes apart.
fn text(len: usize) -> Vec<u8> {
    (0..len).map(|i| b'a' + (i % 23) as u8).collect()
}

/// A string made from an explicit length arrives whole on both sides of the
/// inline room (256 bytes), with NULs as bytes, at any alignment and at the
/// very end of guest memory; one past the end is refused.
#[test]
fn explicit_length_strings_are_exact_around_the_inline_room() {
    let mut guest = Guest::new();
    for len in [0usize, 1, 7, 8, 9, 127, 128, 255, 256, 257, 300, 5000] {
        for at in [0u32, 1, 3] {
            let mut bytes = text(len);
            if len > 4 {
                bytes[2] = 0;
            }
            let ptr = TEXT_PTR + at;
            guest.write(ptr, &bytes);
            // A byte past the string that must not become part of it.
            guest.write(ptr + len as u32, b"!");
            assert_eq!(
                guest.call2("utf8", ptr, len as u32),
                NAPI_OK,
                "{len} at +{at}"
            );
            assert_eq!(guest.subject_utf8(), bytes, "{len} at +{at}");
        }
    }
    // The env has not seen the new page before the first round, and has
    // before the second.
    let size = guest.own_end();
    for round in 0..2 {
        for len in [1u32, 8, 255, 256, 257] {
            let bytes = text(len as usize);
            guest.write(size - len, &bytes);
            assert_eq!(
                guest.call2("utf8", size - len + 1, len),
                NAPI_INVALID_ARG,
                "{len} past the end, round {round}"
            );
            assert_eq!(
                guest.call2("utf8", size - len, len),
                NAPI_OK,
                "{len} at the end, round {round}"
            );
            assert_eq!(guest.subject_utf8(), bytes, "{len} at the end");
        }
        assert_eq!(guest.call2("utf8", size, 1), NAPI_INVALID_ARG);
        assert_eq!(guest.call2("utf8", u32::MAX - 2, 8), NAPI_INVALID_ARG);
    }
}

/// A `NAPI_AUTO_LENGTH` string ends at its first NUL on both sides of the
/// inline room and in the last bytes of guest memory, where the inline
/// window does not fit; with no NUL before the end it is refused.
#[test]
fn auto_length_strings_end_at_their_nul() {
    let mut guest = Guest::new();
    for len in [0usize, 1, 7, 8, 9, 15, 16, 254, 255, 256, 257, 4000] {
        for at in [0u32, 1, 5] {
            let mut bytes = text(len);
            bytes.extend_from_slice(b"\0tail");
            let ptr = TEXT_PTR + at;
            guest.write(ptr, &bytes);
            assert_eq!(guest.call2("utf8", ptr, AUTO), NAPI_OK, "{len} at +{at}");
            assert_eq!(guest.subject_utf8(), text(len), "{len} at +{at}");
            assert_eq!(guest.call2("latin1", ptr, AUTO), NAPI_OK, "{len} at +{at}");
            assert_eq!(guest.subject_utf8(), text(len), "latin1 {len} at +{at}");
        }
    }
    let size = guest.own_end();
    for round in 0..2 {
        for len in [0u32, 1, 7, 100, 254, 255, 300] {
            let mut bytes = text(len as usize);
            bytes.push(0);
            let ptr = size - len - 1;
            guest.write(ptr, &bytes);
            assert_eq!(
                guest.call2("utf8", ptr, AUTO),
                NAPI_OK,
                "{len} at the end, round {round}"
            );
            assert_eq!(guest.subject_utf8(), text(len as usize), "{len} at the end");
        }
        // No NUL from here to the end of the memory.
        guest.write(size - 300, &[b'x'; 300]);
        assert_eq!(guest.call2("utf8", size - 300, AUTO), NAPI_INVALID_ARG);
        assert_eq!(guest.call2("utf8", size - 1, AUTO), NAPI_INVALID_ARG);
        assert_eq!(guest.call2("utf8", size, AUTO), NAPI_INVALID_ARG);
    }
}

/// Latin-1 bytes above 0x7f and NULs arrive as the code points they are.
#[test]
fn latin1_strings_keep_every_byte() {
    let mut guest = Guest::new();
    for len in [3usize, 256, 257] {
        let mut bytes = text(len);
        bytes[0] = 0xe9;
        bytes[1] = 0;
        bytes[2] = 0xff;
        guest.write(TEXT_PTR, &bytes);
        assert_eq!(guest.call2("latin1", TEXT_PTR, len as u32), NAPI_OK);
        let expected: String = bytes.iter().map(|&b| char::from(b)).collect();
        assert_eq!(guest.subject_utf8(), expected.as_bytes(), "{len}");
        // And back out as Latin-1, straight into the guest's buffer.
        assert_eq!(guest.call2("read_latin1", BUFFER_PTR, 1000), NAPI_OK);
        assert_eq!(guest.u32_at(32) as usize, len);
        let back = guest.read(BUFFER_PTR, len + 1);
        assert_eq!(&back[..len], &bytes[..]);
        assert_eq!(back[len], 0);
    }
}

/// Reading a string writes it and its NUL into the guest's buffer and
/// nothing past them: whole, truncated to the buffer, into a buffer that
/// ends with the memory, and through the checked path for a buffer that does
/// not fit the memory. No buffer asks for the length.
#[test]
fn reading_a_string_writes_only_the_string() {
    let mut guest = Guest::new();
    let script = "'h\\u00e9llo w\\u00f6rld'";
    assert_eq!(guest.eval(script), NAPI_OK);
    let utf8 = "héllo wörld".as_bytes();

    // The length alone.
    assert_eq!(guest.call2("read_utf8", 0, 0), NAPI_OK);
    assert_eq!(guest.u32_at(32) as usize, utf8.len());

    for size in [1u32, 2, 3, 5, 13, 14, 64, 4096] {
        guest.write(BUFFER_PTR, &[0xaa; 5000]);
        assert_eq!(guest.call2("read_utf8", BUFFER_PTR, size), NAPI_OK, "{size}");
        let written = guest.u32_at(32) as usize;
        assert!(written < size as usize, "{size}");
        let back = guest.read(BUFFER_PTR, 5000);
        // A truncated copy never splits a character.
        assert!(utf8.starts_with(&back[..written]), "{size}");
        assert!(std::str::from_utf8(&back[..written]).is_ok(), "{size}");
        if size as usize > utf8.len() {
            assert_eq!(written, utf8.len(), "{size}");
        }
        assert_eq!(back[written], 0, "{size}");
        assert!(
            back[written + 1..].iter().all(|&b| b == 0xaa),
            "{size}: bytes past the NUL were written"
        );
    }

    // A buffer that ends exactly with the memory, and one that claims a byte
    // more: the string still fits the part that exists.
    let end = guest.own_end();
    for _ in 0..2 {
        guest.write(end - 64, &[0xaa; 64]);
        assert_eq!(guest.call2("read_utf8", end - 64, 64), NAPI_OK);
        assert_eq!(guest.read(end - 64, utf8.len() + 1)[..utf8.len()], *utf8);
        guest.write(end - 64, &[0xaa; 64]);
        assert_eq!(guest.call2("read_utf8", end - 64, 65), NAPI_OK);
        assert_eq!(guest.read(end - 64, utf8.len() + 1)[..utf8.len()], *utf8);
    }
    // A size no memory holds.
    assert_eq!(guest.call2("read_utf8", BUFFER_PTR, u32::MAX), NAPI_INVALID_ARG);

    // UTF-16, into an aligned buffer (in place) and an odd one (copied).
    let utf16: Vec<u8> = "héllo wörld"
        .encode_utf16()
        .flat_map(u16::to_le_bytes)
        .collect();
    for (at, size) in [(0u32, 64u32), (1, 64), (0, 6), (1, 6), (0, 1), (1, 1)] {
        guest.write(BUFFER_PTR, &[0xaa; 400]);
        assert_eq!(
            guest.call2("read_utf16", BUFFER_PTR + at, size),
            NAPI_OK,
            "utf16 +{at} {size}"
        );
        let written = guest.u32_at(32) as usize;
        assert_eq!(written, (utf16.len() / 2).min(size as usize - 1));
        let back = guest.read(BUFFER_PTR + at, 300);
        assert_eq!(&back[..2 * written], &utf16[..2 * written], "utf16 +{at} {size}");
        assert_eq!(&back[2 * written..2 * written + 2], &[0, 0]);
        assert!(back[2 * written + 2..].iter().all(|&b| b == 0xaa));
    }

    // Not a string: the status, and nothing written.
    assert_eq!(guest.eval("42"), NAPI_OK);
    guest.write(BUFFER_PTR, &[0xaa; 64]);
    guest.write(32, &7u32.to_le_bytes());
    assert_ne!(guest.call2("read_utf8", BUFFER_PTR, 64), NAPI_OK);
    assert!(guest.read(BUFFER_PTR, 64).iter().all(|&b| b == 0xaa));
    assert_eq!(guest.u32_at(32), 7);
}

/// Memory the guest grew after the env first looked is as good as the rest:
/// strings come from it and go to it, and results are written there.
#[test]
fn memory_grown_later_is_reached() {
    let mut guest = Guest::new();
    // Use the imports first, so the env has seen the memory's first length.
    assert_eq!(guest.eval("1 + 1"), NAPI_OK);
    assert_eq!(guest.int(), 2);
    let old = guest.size();
    guest.memory.grow(&mut guest.store, Pages(3)).unwrap();
    let new = guest.size();
    assert_eq!(new, old + 3 * 65536);

    let bytes = text(200);
    guest.write(new - 200, &bytes);
    assert_eq!(guest.call2("utf8", new - 200, 200), NAPI_OK);
    assert_eq!(guest.call2("read_utf8", new - 400, 201), NAPI_OK);
    assert_eq!(guest.read(new - 400, 200), bytes);
    assert_eq!(guest.call2("utf8", new - 199, 200), NAPI_INVALID_ARG);

    // Call arguments from the new pages.
    let argv = new - 64;
    let ints: TypedFunction<(i32, i32), ()> = guest.typed("ints");
    ints.call(&mut guest.store, argv as i32, 3).unwrap();
    assert_eq!(guest.eval("(function (a, b, c) { return a * 100 + b * 10 + c; })"), NAPI_OK);
    assert_eq!(guest.call2("call", 3, argv), NAPI_OK);
    assert_eq!(guest.int(), 123);
}

/// A call's arguments arrive in order, on both sides of the inline room for
/// their ids (8), from any alignment, and from the end of guest memory; ids
/// past the end are refused.
#[test]
fn call_arguments_arrive_in_order() {
    let mut guest = Guest::new();
    let sum = "(function () { let s = arguments.length * 1000000; \
               for (let i = 0; i < arguments.length; i++) s += (i + 1) * arguments[i]; \
               return s; })";
    for argc in [0u32, 1, 2, 7, 8, 9, 16, 40] {
        for at in [0u32, 1, 2] {
            let argv = ARGV_PTR + at;
            let ints: TypedFunction<(i32, i32), ()> = guest.typed("ints");
            ints.call(&mut guest.store, argv as i32, argc as i32).unwrap();
            let expected = (argc * 1_000_000 + (1..=argc).map(|i| i * i).sum::<u32>()) as i32;
            assert_eq!(guest.eval(sum), NAPI_OK);
            assert_eq!(guest.call2("call", argc, argv), NAPI_OK, "{argc} at +{at}");
            assert_eq!(guest.int(), expected, "{argc} at +{at}");

            assert_eq!(
                guest.eval(&format!(
                    "(function () {{ globalThis.made = {sum}.apply(null, arguments); }})"
                )),
                NAPI_OK
            );
            assert_eq!(guest.call2("new", argc, argv), NAPI_OK, "new {argc} at +{at}");
            assert_eq!(guest.eval("globalThis.made"), NAPI_OK);
            assert_eq!(guest.int(), expected, "new {argc} at +{at}");
        }
    }
    let end = guest.own_end();
    for round in 0..2 {
        assert_eq!(guest.eval(sum), NAPI_OK);
        assert_eq!(guest.call2("call", 3, end - 8), NAPI_INVALID_ARG, "round {round}");
        assert_eq!(guest.call2("call", 2, end - 4), NAPI_INVALID_ARG, "round {round}");
        assert_eq!(guest.call2("call", 9, end - 8), NAPI_INVALID_ARG, "round {round}");
        let ints: TypedFunction<(i32, i32), ()> = guest.typed("ints");
        ints.call(&mut guest.store, (end - 8) as i32, 2).unwrap();
        assert_eq!(guest.call2("call", 2, end - 8), NAPI_OK, "round {round}");
        assert_eq!(guest.int(), 2_000_005);
    }
    // No arguments: the pointer is not looked at.
    assert_eq!(guest.eval(sum), NAPI_OK);
    assert_eq!(guest.call2("call", 0, end + 100), NAPI_OK);
    assert_eq!(guest.int(), 0);
}

/// A guest function gets the arguments it asked for, on both sides of the
/// inline room of `napi_get_cb_info` (16): the count JavaScript passed is
/// written back, and arguments past it are `undefined`.
#[test]
fn callback_arguments_are_read_around_the_inline_room() {
    let mut guest = Guest::new();
    let want: TypedFunction<i32, ()> = guest.typed("want");
    for wanted in [0u32, 1, 2, 15, 16, 17, 40] {
        for passed in [0u32, 1, 16, 17, 30] {
            want.call(&mut guest.store, wanted as i32).unwrap();
            let args: Vec<String> = (1..=passed).map(|i| i.to_string()).collect();
            assert_eq!(guest.eval(&format!("f({})", args.join(", "))), NAPI_OK);
            let counted = wanted.min(passed);
            assert_eq!(
                guest.int() as u32,
                counted * (counted + 1) / 2,
                "wanted {wanted}, passed {passed}"
            );
            assert_eq!(guest.u32_at(60), passed, "wanted {wanted}, passed {passed}");
        }
    }
}

/// Property names are read on both sides of their inline room (128 bytes
/// with the NUL), at any alignment.
#[test]
fn named_properties_are_found_by_names_of_any_length() {
    let mut guest = Guest::new();
    for len in [1usize, 7, 8, 9, 126, 127, 128, 129, 300] {
        let name: String = (0..len)
            .map(|i| char::from(b'a' + (i % 26) as u8))
            .collect();
        assert_eq!(
            guest.eval(&format!("globalThis['{name}'] = {len}; 0")),
            NAPI_OK
        );
        for at in [0u32, 1, 4] {
            guest.global(&name, TEXT_PTR + at);
            assert_eq!(guest.int() as usize, len, "name of {len} bytes at +{at}");
        }
    }
    // A name in the last bytes of the memory, where the inline window does
    // not fit.
    let end = guest.own_end();
    assert_eq!(guest.eval("globalThis.tail = 77; 0"), NAPI_OK);
    for _ in 0..2 {
        guest.global("tail", end - 5);
        assert_eq!(guest.int(), 77);
    }
}
