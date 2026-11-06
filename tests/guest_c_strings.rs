//! Guest strings made with `napi_create_string_utf8` / `_latin1`, through
//! real Wasmer dispatch, the bridge and V8: NUL-terminated ones
//! (`NAPI_AUTO_LENGTH`) of every length up to megabytes, ones ending at the
//! last byte of guest memory, ones with no NUL before the memory's end, and
//! explicit lengths with NULs inside.
//!
//! `string_costs` (ignored) is not a correctness test: it prints the cost of
//! a call by length, with and without `NAPI_AUTO_LENGTH`. Run it with
//! `./cargo-standalone.sh test --release --test guest_c_strings -- --ignored --nocapture`.
//! `PERF_ROUNDS` (default 5) sets the rounds per case, of which the median is
//! printed.

#![cfg(not(target_arch = "wasm32"))]

use std::time::Instant;

use wasmer::{AsStoreMut, Imports, Instance, Memory, Module, Store, TypedFunction};
use wasmer_napi::NapiCtx;

const AUTO: u32 = u32::MAX;
const STRING_PTR: u32 = 65536;
const BUFFER_PTR: u32 = 4 << 20;
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

  ;; 8 env, 12 scope, 28 string handle, 32 length out. Strings from 64 KiB,
  ;; read-back buffer from 4 MiB (10 MiB in all).
  (memory (export "memory") 160)
  (table (export "__indirect_function_table") 1 funcref)
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

  (func (export "utf8") (param $ptr i32) (param $len i32) (result i32)
    (call $create_utf8 (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28)))
  (func (export "latin1") (param $ptr i32) (param $len i32) (result i32)
    (call $create_latin1 (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28)))

  ;; The last string made, as UTF-8 into $buf (at most $size bytes with the
  ;; NUL); its length goes to 32.
  (func (export "read") (param $buf i32) (param $size i32) (result i32)
    (call $get_utf8 (global.get $env) (i32.load (i32.const 28)) (local.get $buf)
      (local.get $size) (i32.const 32)))

  ;; n x napi_create_string_utf8 of one string.
  (func (export "loop_utf8") (param $ptr i32) (param $len i32) (param $n i32)
    (loop $again
      (if (call $create_utf8 (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28))
        (then unreachable))
      (br_if $again (local.tee $n (i32.sub (local.get $n) (i32.const 1))))))
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

    /// Makes a string of `len` bytes at `ptr` (`AUTO`: up to its NUL); its
    /// status.
    fn make(&mut self, export: &str, ptr: u32, len: u32) -> i32 {
        let make: TypedFunction<(i32, i32), i32> = self.typed(export);
        make.call(&mut self.store, ptr as i32, len as i32)
            .expect("trapped")
    }

    /// The last string made, as UTF-8.
    fn read_back(&mut self) -> Vec<u8> {
        let read: TypedFunction<(i32, i32), i32> = self.typed("read");
        // Its length first (no buffer), then its bytes.
        assert_eq!(read.call(&mut self.store, 0, 0).unwrap(), NAPI_OK);
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

    /// Writes `text` (ASCII) with a NUL after it at `ptr`, and checks that
    /// both UTF-8 and Latin-1 strings made from it with `NAPI_AUTO_LENGTH`
    /// arrive whole. Failures name `case` and give lengths, not the bytes.
    fn check_auto(&mut self, ptr: u32, text: &[u8], case: &str) {
        let mut bytes = text.to_vec();
        bytes.push(0);
        self.write(ptr, &bytes);
        for export in ["utf8", "latin1"] {
            assert_eq!(self.make(export, ptr, AUTO), NAPI_OK, "{export}, {case}");
            let back = self.read_back();
            assert_eq!(back.len(), text.len(), "{export}, {case}");
            assert!(back == text, "{export}, {case}: the bytes differ");
        }
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

#[test]
fn auto_length_strings_of_every_size() {
    let mut guest = Guest::new();
    // Around the first scanned chunk (256), the 8-byte words, and past the
    // old 64 KiB scan bound, at word-aligned and unaligned starts.
    for len in [
        0, 1, 7, 8, 9, 15, 16, 17, 254, 255, 256, 257, 263, 1000, 4095, 4096, 65535, 65536, 70_000,
        1_000_000, 3_000_000,
    ] {
        for offset in [0, 3] {
            guest.check_auto(
                STRING_PTR + offset,
                &text(len),
                &format!("length {len} at +{offset}"),
            );
        }
    }
    assert_eq!(guest.release(), NAPI_OK);
}

#[test]
fn auto_length_ends_at_the_end_of_memory() {
    let mut guest = Guest::new();
    // A page of its own at the end, which no allocation of the bridge holds.
    guest.memory.grow(&mut guest.store, 1).unwrap();
    let size = guest.size();
    // The NUL is the memory's last byte: short (in the first chunk), and past
    // the first chunk.
    for len in [0usize, 3, 255, 300, 4000] {
        let ptr = size - len as u32 - 1;
        guest.check_auto(
            ptr,
            &text(len),
            &format!("length {len} ending at the memory's end"),
        );
    }
    // No NUL before the end: refused, not read past it.
    guest.write(size - 4096, &text(4096));
    for start in [size - 4096, size - 300, size - 9, size - 1] {
        assert_eq!(
            guest.make("utf8", start, AUTO),
            NAPI_INVALID_ARG,
            "unterminated at {start}"
        );
        assert_eq!(
            guest.make("latin1", start, AUTO),
            NAPI_INVALID_ARG,
            "unterminated latin1 at {start}"
        );
    }
    // Starting at or past the end.
    for start in [size, size + 1, u32::MAX - 1] {
        assert_eq!(
            guest.make("utf8", start, AUTO),
            NAPI_INVALID_ARG,
            "start {start}"
        );
    }
    assert_eq!(guest.release(), NAPI_OK);
}

#[test]
fn explicit_lengths_keep_nuls_and_empty_strings() {
    let mut guest = Guest::new();
    guest.write(STRING_PTR, b"a\0b\0\0c");
    assert_eq!(guest.make("utf8", STRING_PTR, 6), NAPI_OK);
    assert_eq!(guest.read_back(), b"a\0b\0\0c");
    // Latin-1 with NULs and a byte above 0x7f.
    guest.write(STRING_PTR, b"x\0\xe9\0");
    assert_eq!(guest.make("latin1", STRING_PTR, 4), NAPI_OK);
    assert_eq!(guest.read_back(), "x\0\u{e9}\0".as_bytes());
    // NAPI_AUTO_LENGTH stops at the first NUL.
    guest.write(STRING_PTR, b"a\0b\0");
    assert_eq!(guest.make("utf8", STRING_PTR, AUTO), NAPI_OK);
    assert_eq!(guest.read_back(), b"a");
    assert_eq!(guest.make("latin1", STRING_PTR, AUTO), NAPI_OK);
    assert_eq!(guest.read_back(), b"a");
    // Empty: an explicit 0, and a NUL first.
    assert_eq!(guest.make("utf8", STRING_PTR, 0), NAPI_OK);
    assert_eq!(guest.read_back(), b"");
    guest.write(STRING_PTR, b"\0");
    assert_eq!(guest.make("utf8", STRING_PTR, AUTO), NAPI_OK);
    assert_eq!(guest.read_back(), b"");
    assert_eq!(guest.make("latin1", STRING_PTR, AUTO), NAPI_OK);
    assert_eq!(guest.read_back(), b"");
    // A large explicit length past 64 KiB, with a NUL in it.
    let mut large = text(200_000);
    large[100_000] = 0;
    guest.write(STRING_PTR, &large);
    assert_eq!(guest.make("utf8", STRING_PTR, large.len() as u32), NAPI_OK);
    assert_eq!(guest.read_back(), large);
    // An explicit length past the memory's end.
    let size = guest.size();
    assert_eq!(guest.make("utf8", size - 4, 8), NAPI_INVALID_ARG);
    assert_eq!(guest.release(), NAPI_OK);
}

#[test]
#[ignore]
fn string_costs() {
    let mut guest = Guest::new();
    let rounds: usize = std::env::var("PERF_ROUNDS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(5);
    for len in [16usize, 200, 1024, 8192, 32768, 65000] {
        let mut bytes = text(len);
        bytes.push(0);
        guest.write(STRING_PTR, &bytes);
        let calls = (4_000_000 / (len + 64)).clamp(200, 200_000) as i32;
        for (name, length) in [("auto", AUTO), ("explicit", len as u32)] {
            let run: TypedFunction<(i32, i32, i32), ()> = guest.typed("loop_utf8");
            run.call(
                &mut guest.store,
                STRING_PTR as i32,
                length as i32,
                calls / 10 + 1,
            )
            .unwrap();
            let mut samples = Vec::new();
            for _ in 0..rounds {
                let began = Instant::now();
                run.call(&mut guest.store, STRING_PTR as i32, length as i32, calls)
                    .unwrap();
                samples.push(began.elapsed().as_nanos() as f64 / f64::from(calls));
            }
            samples.sort_by(f64::total_cmp);
            println!(
                "create_string_utf8 {name:>8} {len:>6} B: {:>10.1} ns/call (median of {rounds})",
                samples[rounds / 2]
            );
        }
    }
    assert_eq!(guest.release(), NAPI_OK);
}
