//! Property names a guest passes as a NUL-terminated string, to
//! `napi_set_named_property`, `napi_get_named_property`,
//! `napi_has_named_property` and a `napi_define_properties` descriptor,
//! through real Wasmer dispatch, the bridge and V8. The engine makes them
//! internalized strings, and the three named-property imports copy a short
//! name without allocating. Neither may change which property a name is: for
//! every name here the property is the one a script reaches with the string
//! `napi_create_string_utf8` makes from the same bytes.

#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Imports, Instance, Memory, Module, Store, TypedFunction};
use wasmer_napi::NapiCtx;

const SCRIPT_PTR: u32 = 4096;
const NAME_PTR: u32 = 1 << 20;
const BUFFER_PTR: u32 = 3 << 20;
const NAPI_OK: i32 = 0;
const NAPI_INVALID_ARG: i32 = 1;

const WAT: &str = r#"(module
  (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
    (func $configure (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
    (func $create_env (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
    (func $release_env (param i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_get_heap_statistics"
    (func $heap_statistics (param i32 i32) (result i32)))
  (import "napi" "napi_open_handle_scope" (func $open_scope (param i32 i32) (result i32)))
  (import "napi" "napi_close_handle_scope" (func $close_scope (param i32 i32) (result i32)))
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_named (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_named_property"
    (func $get_named (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_has_named_property"
    (func $has_named (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_define_properties"
    (func $define_properties (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_string_utf8"
    (func $create_string (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_run_script" (func $run_script (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_string_utf8"
    (func $get_utf8 (param i32 i32 i32 i32 i32) (result i32)))

  ;; 8 env, 12 scope, 16 global, 20 the object `o`, 24 script source, 28
  ;; script result, 32 length out, 36 has-result, 40 key string, 44 the value
  ;; "v", 52 a property read, 64.. the names below, 256 a property descriptor
  ;; (utf8name at +0, value at +20, attributes at +24), 96 the engine flags
  ;; and 120 the runtime options that name them, 1024 heap statistics (136
  ;; bytes, version 1; used_heap_size at +48). Scripts from 4 KiB, names from
  ;; 1 MiB, the read-back buffer from 3 MiB.
  (memory (export "memory") 64)
  (table (export "__indirect_function_table") 1 funcref)
  (global $env (mut i32) (i32.const 0))
  (global $scope (mut i32) (i32.const 0))
  (data (i32.const 64) "o\00")
  (data (i32.const 72) "v\00")
  (data (i32.const 80) "k\00")
  (data (i32.const 88) "g\00")
  (data (i32.const 96) "--expose-gc")
  (data (i32.const 120) "\10\00\00\00" "\01\00\00\00" "\60\00\00\00" "\0b\00\00\00")

  (func (export "create")
    (if (call $configure (i32.const 120)) (then unreachable))
    (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
      (then unreachable))
    (global.set $env (i32.load (i32.const 8)))
    (global.set $scope (i32.load (i32.const 12))))

  (func (export "release") (result i32)
    (call $release_env (global.get $scope) (i32.const 0)))

  ;; Takes the global `o` as the object the calls below work on.
  (func (export "bind")
    (if (call $get_global (global.get $env) (i32.const 16)) (then unreachable))
    (if (call $get_named (global.get $env) (i32.load (i32.const 16)) (i32.const 64)
          (i32.const 20))
      (then unreachable))
    (if (call $create_string (global.get $env) (i32.const 72) (i32.const 1) (i32.const 44))
      (then unreachable)))

  ;; The global `k` becomes the string napi_create_string_utf8 makes of the
  ;; NUL-terminated bytes at $name.
  (func (export "key") (param $name i32) (result i32)
    (if (call $create_string (global.get $env) (local.get $name) (i32.const -1)
          (i32.const 40))
      (then (return (i32.const 99))))
    (call $set_named (global.get $env) (i32.load (i32.const 16)) (i32.const 80)
      (i32.load (i32.const 40))))

  (func (export "set") (param $name i32) (result i32)
    (call $set_named (global.get $env) (i32.load (i32.const 20)) (local.get $name)
      (i32.load (i32.const 44))))

  ;; Reads the property; the global `g` becomes what was read.
  (func (export "get") (param $name i32) (result i32)
    (local $status i32)
    (if (local.tee $status
          (call $get_named (global.get $env) (i32.load (i32.const 20)) (local.get $name)
            (i32.const 52)))
      (then (return (local.get $status))))
    (call $set_named (global.get $env) (i32.load (i32.const 16)) (i32.const 88)
      (i32.load (i32.const 52))))

  ;; 0 or 1, or 100 + the status.
  (func (export "has") (param $name i32) (result i32)
    (local $status i32)
    (if (local.tee $status
          (call $has_named (global.get $env) (i32.load (i32.const 20)) (local.get $name)
            (i32.const 36)))
      (then (return (i32.add (i32.const 100) (local.get $status)))))
    (i32.load8_u (i32.const 36)))

  ;; A writable, enumerable, configurable data property with the value "v".
  (func (export "define") (param $name i32) (result i32)
    (i32.store (i32.const 256) (local.get $name))
    (i32.store (i32.const 260) (i32.const 0))
    (i32.store (i32.const 264) (i32.const 0))
    (i32.store (i32.const 268) (i32.const 0))
    (i32.store (i32.const 272) (i32.const 0))
    (i32.store (i32.const 276) (i32.load (i32.const 44)))
    (i32.store (i32.const 280) (i32.const 7))
    (i32.store (i32.const 284) (i32.const 0))
    (call $define_properties (global.get $env) (i32.load (i32.const 20)) (i32.const 1)
      (i32.const 256)))

  ;; A handle scope (at 100) for what `bind` and the calls after it make.
  (func (export "open_scope") (result i32)
    (call $open_scope (global.get $env) (i32.const 100)))
  (func (export "close_scope") (result i32)
    (call $close_scope (global.get $env) (i32.load (i32.const 100))))

  ;; V8's used heap size, in bytes.
  (func (export "heap_used") (result i64)
    (i32.store (i32.const 1024) (i32.const 136))
    (i32.store (i32.const 1028) (i32.const 1))
    (if (call $heap_statistics (global.get $env) (i32.const 1024)) (then unreachable))
    (i64.load (i32.const 1072)))

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

/// What scripts compare with: `same(a, b)` is "ok" when two objects have the
/// same own property names in the same order with the same values, the same
/// prototype and the same `length`.
const PRELUDE: &str = r#"
globalThis.same = (a, b) => {
  const na = Object.getOwnPropertyNames(a), nb = Object.getOwnPropertyNames(b);
  const ok = na.length === nb.length && na.every((name, i) => name === nb[i] && a[name] === b[name])
    && Object.getPrototypeOf(a) === Object.getPrototypeOf(b) && a.length === b.length;
  return ok ? 'ok' : 'differ: ' + JSON.stringify([na.map(n => n.slice(0, 20)), nb.map(n => n.slice(0, 20))]);
};
'ok'"#;

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
        assert_eq!(guest.eval_string(PRELUDE), b"ok");
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

    /// One of the exports that take a name.
    fn call(&mut self, export: &str, name_ptr: u32) -> i32 {
        let function: TypedFunction<i32, i32> = self.typed(export);
        function
            .call(&mut self.store, name_ptr as i32)
            .expect("trapped")
    }

    /// Runs `script` and returns its string result as UTF-8.
    fn eval_string(&mut self, script: &str) -> Vec<u8> {
        assert!(SCRIPT_PTR as usize + script.len() < NAME_PTR as usize);
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

    fn eval(&mut self, script: &str) -> String {
        String::from_utf8(self.eval_string(script)).unwrap()
    }

    /// New `o` (what the guest works on) and `t` (what a script does the
    /// same to), both made by `make`.
    fn fresh(&mut self, make: &str) {
        let script = format!("globalThis.o = {make}; globalThis.t = {make}; 'ok'");
        assert_eq!(self.eval(&script), "ok");
        let bind: TypedFunction<(), ()> = self.typed("bind");
        bind.call(&mut self.store).expect("bind trapped");
    }

    /// V8's used heap after a full collection.
    fn heap_after_gc(&mut self) -> i64 {
        assert_eq!(self.eval("gc(); gc(); 'ok'"), "ok");
        let used: TypedFunction<(), i64> = self.typed("heap_used");
        used.call(&mut self.store).expect("heap_used trapped")
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

/// Every operation on the NUL-terminated name at `ptr`, each against what a
/// script does with the key `k` (the string `napi_create_string_utf8` makes
/// from the same bytes), on a plain object and on an array.
fn check_name(guest: &mut Guest, ptr: u32, case: &str) {
    for make in ["{}", "[]", "Object.create(null)"] {
        let case = format!("{case} on {make}");
        guest.fresh(make);
        assert_eq!(guest.call("key", ptr), NAPI_OK, "{case}");

        // Before anything is set: inherited names answer as `in` does.
        let expected = guest.eval("String(k in o)");
        let has = guest.call("has", ptr);
        assert_eq!(has == 1, expected == "true", "{case}: has before");
        assert!(has <= 1, "{case}: has status {has}");
        assert_eq!(guest.call("get", ptr), NAPI_OK, "{case}");
        assert_eq!(
            guest.eval("g === o[k] ? 'ok' : 'differ'"),
            "ok",
            "{case}: get before"
        );

        // Set: the same property a script's assignment makes (`__proto__`
        // makes none, an array index grows the array).
        assert_eq!(guest.call("set", ptr), NAPI_OK, "{case}");
        assert_eq!(guest.eval("t[k] = 'v'; same(o, t)"), "ok", "{case}: set");
        assert_eq!(
            guest.call("has", ptr) == 1,
            guest.eval("String(k in t)") == "true",
            "{case}: has"
        );
        assert_eq!(guest.call("get", ptr), NAPI_OK, "{case}");
        assert_eq!(
            guest.eval("g === t[k] ? 'ok' : 'differ'"),
            "ok",
            "{case}: get"
        );

        // A descriptor's utf8name: an own property, `__proto__` included.
        guest.fresh(make);
        assert_eq!(guest.call("define", ptr), NAPI_OK, "{case}");
        assert_eq!(
            guest.eval(
                "Object.defineProperty(t, k, {value: 'v', writable: true, enumerable: true, \
                 configurable: true}); same(o, t)"
            ),
            "ok",
            "{case}: define"
        );
        assert_eq!(
            guest.eval("Object.getOwnPropertyNames(o).includes(k) && o[k] === 'v' ? 'ok' : 'no'"),
            "ok",
            "{case}: defined"
        );
    }
}

#[test]
fn names_are_the_keys_scripts_use() {
    let mut guest = Guest::new();
    let names: Vec<Vec<u8>> = [
        &b""[..],
        b"a",
        b"name",
        b"getOwnPropertyDescriptor",
        // Not ASCII: Latin-1 range, two-byte strings, outside the BMP.
        "ma\u{f1}ana".as_bytes(),
        "\u{65e5}\u{672c}\u{8a9e}".as_bytes(),
        "smile\u{1f600}".as_bytes(),
        // Not UTF-8: a stray byte, a cut sequence, a surrogate.
        b"a\xffb",
        b"cut\xc3",
        b"\xed\xa0\x80",
        b"\xf0\x9f\x98",
        // Array indices and their neighbors.
        b"0",
        b"1",
        b"7",
        b"-0",
        b"01",
        b"1.0",
        b"4294967294",
        b"4294967295",
        b"4294967296",
        // Names with a meaning.
        b"__proto__",
        b"constructor",
        b"toString",
        b"hasOwnProperty",
    ]
    .iter()
    .map(|name| name.to_vec())
    .collect();
    for name in &names {
        guest.write(NAME_PTR, &[&name[..], b"\0tail\0"].concat());
        check_name(
            &mut guest,
            NAME_PTR,
            &format!("{:?}", String::from_utf8_lossy(name)),
        );
    }
    // Spot checks that do not go through napi_create_string_utf8.
    guest.fresh("{}");
    guest.write(NAME_PTR, "ma\u{f1}ana\0".as_bytes());
    assert_eq!(guest.call("set", NAME_PTR), NAPI_OK);
    assert_eq!(guest.eval("o['ma\\u00f1ana'] === 'v' ? 'ok' : 'no'"), "ok");
    guest.fresh("[]");
    guest.write(NAME_PTR, b"4294967294\0");
    assert_eq!(guest.call("set", NAME_PTR), NAPI_OK);
    assert_eq!(guest.eval("String(o.length)"), "4294967295");
    guest.write(NAME_PTR, b"4294967295\0");
    assert_eq!(guest.call("set", NAME_PTR), NAPI_OK);
    assert_eq!(
        guest.eval("o.length === 4294967295 && o['4294967295'] === 'v' ? 'ok' : 'no'"),
        "ok"
    );
    assert_eq!(guest.release(), NAPI_OK);
}

/// Lengths around the inline copy (127 bytes and a NUL), the first chunk of
/// the long path (256), the old 64 KiB scan limit, and a megabyte.
#[test]
fn names_of_every_length() {
    let mut guest = Guest::new();
    for len in [
        1, 7, 8, 9, 63, 64, 126, 127, 128, 129, 255, 256, 257, 4095, 4096, 65535, 65536, 65537,
        1_000_000,
    ] {
        let name = text(len);
        guest.write(NAME_PTR, &[&name[..], b"\0tail\0"].concat());
        check_name(&mut guest, NAME_PTR, &format!("{len} bytes"));
        guest.fresh("{}");
        assert_eq!(guest.call("set", NAME_PTR), NAPI_OK);
        assert_eq!(
            guest.eval("String(Object.getOwnPropertyNames(o)[0].length)"),
            len.to_string()
        );
    }
    // Two-byte and non-BMP names across the inline limit, in bytes.
    for unit in ["\u{e9}", "\u{65e5}", "\u{1f600}"] {
        for count in [30, 31, 32, 42, 43, 63, 64, 65] {
            let name = unit.repeat(count);
            guest.write(NAME_PTR, &[name.as_bytes(), b"\0tail\0"].concat());
            check_name(
                &mut guest,
                NAME_PTR,
                &format!("{count} x U+{:X}", unit.chars().next().unwrap() as u32),
            );
        }
    }
    assert_eq!(guest.release(), NAPI_OK);
}

/// The name is read where it is when the call is made: the same address with
/// other bytes is another property, and another address with the same bytes
/// is the same one.
#[test]
fn names_are_read_by_content() {
    let mut guest = Guest::new();
    guest.fresh("{}");
    guest.write(NAME_PTR, b"first\0");
    assert_eq!(guest.call("set", NAME_PTR), NAPI_OK);
    guest.write(NAME_PTR, b"other\0");
    assert_eq!(guest.call("set", NAME_PTR), NAPI_OK);
    assert_eq!(guest.call("has", NAME_PTR), 1);
    guest.write(NAME_PTR, b"third\0");
    assert_eq!(guest.call("has", NAME_PTR), 0);
    guest.write(NAME_PTR + 4096, b"first\0");
    assert_eq!(guest.call("has", NAME_PTR + 4096), 1);
    assert_eq!(
        guest.eval("Object.getOwnPropertyNames(o).join()"),
        "first,other"
    );
    assert_eq!(guest.release(), NAPI_OK);
}

/// Names that end at the last byte of guest memory are read, on the inline
/// path's fallback too (its window would run past the end); names with no
/// NUL before the end are refused with nothing done.
#[test]
fn names_at_the_end_of_memory() {
    let mut guest = Guest::new();
    let size = guest.size();
    for len in [0usize, 1, 5, 126, 127, 128, 300] {
        let name = text(len);
        let ptr = size - len as u32 - 1;
        guest.write(ptr, &[&name[..], b"\0"].concat());
        check_name(&mut guest, ptr, &format!("{len} bytes ending at the end"));
    }
    for len in [1usize, 5, 127, 128, 300] {
        let ptr = size - len as u32;
        guest.write(ptr, &text(len));
        guest.fresh("{}");
        let case = format!("{len} unterminated bytes");
        assert_eq!(guest.call("set", ptr), NAPI_INVALID_ARG, "{case}");
        assert_eq!(guest.call("get", ptr), NAPI_INVALID_ARG, "{case}");
        assert_eq!(guest.call("has", ptr), 100 + NAPI_INVALID_ARG, "{case}");
        assert_eq!(
            guest.eval("String(Object.getOwnPropertyNames(o).length)"),
            "0",
            "{case}"
        );
    }
    // Past the end.
    guest.fresh("{}");
    assert_eq!(guest.call("set", size), NAPI_INVALID_ARG);
    assert_eq!(guest.call("has", size), 100 + NAPI_INVALID_ARG);
    assert_eq!(guest.release(), NAPI_OK);
}

/// 100,000 different names set through the import, then deleted: every one
/// is its own property, and none is left. Nor are their strings: the string
/// table holds an internalized string weakly, so once the object is gone a
/// collection frees the names (about 5 MiB of them with their dictionary).
#[test]
fn many_different_names() {
    let mut guest = Guest::new();
    const COUNT: usize = 100_000;
    let before = guest.heap_after_gc();
    // The guest's handle to `o` lives as long as its scope.
    let scope: TypedFunction<(), i32> = guest.typed("open_scope");
    assert_eq!(scope.call(&mut guest.store).unwrap(), NAPI_OK);
    guest.fresh("{}");
    for index in 0..COUNT {
        guest.write(NAME_PTR, format!("dynamicName{index}\0").as_bytes());
        assert_eq!(guest.call("set", NAME_PTR), NAPI_OK);
    }
    assert_eq!(
        guest.eval(
            "(() => { const names = Object.getOwnPropertyNames(o); \
             return String(names.length) + ' ' + names.every((name, i) => name === 'dynamicName' + i); })()"
        ),
        format!("{COUNT} true")
    );
    for index in (0..COUNT).step_by(9973) {
        guest.write(NAME_PTR, format!("dynamicName{index}\0").as_bytes());
        assert_eq!(guest.call("has", NAME_PTR), 1);
    }
    assert_eq!(
        guest.eval(
            "for (const name of Object.getOwnPropertyNames(o)) delete o[name]; \
             String(Object.getOwnPropertyNames(o).length)"
        ),
        "0"
    );
    guest.write(NAME_PTR, b"dynamicName5\0");
    assert_eq!(guest.call("has", NAME_PTR), 0);
    let with_names = {
        let used: TypedFunction<(), i64> = guest.typed("heap_used");
        used.call(&mut guest.store).unwrap()
    };
    let scope: TypedFunction<(), i32> = guest.typed("close_scope");
    assert_eq!(scope.call(&mut guest.store).unwrap(), NAPI_OK);
    assert_eq!(
        guest.eval("globalThis.o = globalThis.t = globalThis.g = undefined; 'ok'"),
        "ok"
    );
    let after = guest.heap_after_gc();
    eprintln!(
        "used heap: {before} before, {with_names} with the names, {after} after a collection"
    );
    assert!(
        after - before < 512 * 1024,
        "names were kept: {before} bytes before, {after} after"
    );
    assert_eq!(guest.release(), NAPI_OK);
}
