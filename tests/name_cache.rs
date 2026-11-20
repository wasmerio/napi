//! The named-property imports with the host's name cache, and the calls whose
//! provider paths changed with it, through real Wasmer dispatch, the bridge
//! and V8.
//!
//! A guest passes a property name as a C string in its own memory. The host
//! remembers names by guest address and gives the provider a number for each,
//! so that neither reads the name again. These tests check that a remembered
//! name is never trusted: the bytes the guest has now decide.

#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Imports, Instance, Memory, Module, Store, TypedFunction};
use wasmer_napi::NapiCtx;

const NAMES_PTR: u32 = 4096;
const SCRIPT_PTR: u32 = 65536;
const PROGRAM_PTR: u32 = 256;
const NAPI_OK: i32 = 0;

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
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_get_undefined" (func $get_undefined (param i32 i32) (result i32)))
  (import "napi" "napi_create_int32" (func $create_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_int32" (func $get_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_uint32" (func $get_uint32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_int64" (func $get_int64 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_double" (func $get_double (param i32 i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_named (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_named_property"
    (func $get_named (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_has_named_property"
    (func $has_named (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_function"
    (func $create_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_call_function"
    (func $call_function (param i32 i32 i32 i32 i32 i32) (result i32)))

  ;; 8 env, 12 scope, 16 global, 20 the `touch` function, 24 script source,
  ;; 28 script result, 36 a get's or a call's result, 40 a has's result,
  ;; 44 an int32, 48 an int64 or a double, 56 a new int32, 60 undefined,
  ;; 64..192 a call's arguments, 196 a get made by `touch`, 200 "touch",
  ;; 256 the names `touch` gets: a count, then that many name pointers.
  (memory (export "memory") 4)
  (table (export "__indirect_function_table") 2 funcref)
  (global $env (mut i32) (i32.const 0))
  (global $scope (mut i32) (i32.const 0))
  (global $target (mut i32) (i32.const 0))
  (data (i32.const 200) "touch\00")

  ;; touch(): gets each name of the program at 256 from the global object.
  (func $touch (param $env i32) (param $info i32) (result i32)
    (local $i i32)
    (block $done
      (loop $next
        (br_if $done (i32.ge_u (local.get $i) (i32.load (i32.const 256))))
        (drop (call $get_named (local.get $env) (i32.load (i32.const 16))
          (i32.load (i32.add (i32.const 260) (i32.shl (local.get $i) (i32.const 2))))
          (i32.const 196)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $next)))
    (i32.const 0))
  (elem (i32.const 1) $touch)

  (func (export "create")
    (if (call $configure (i32.const 0)) (then unreachable))
    (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
      (then unreachable))
    (global.set $env (i32.load (i32.const 8)))
    (global.set $scope (i32.load (i32.const 12)))
    (if (call $get_global (global.get $env) (i32.const 16)) (then unreachable))
    (global.set $target (i32.load (i32.const 16)))
    (if (call $get_undefined (global.get $env) (i32.const 60)) (then unreachable))
    (if (call $create_function (global.get $env) (i32.const 200) (i32.const 5)
          (i32.const 1) (i32.const 0) (i32.const 20))
      (then unreachable))
    (if (call $set_named (global.get $env) (i32.load (i32.const 16)) (i32.const 200)
          (i32.load (i32.const 20)))
      (then unreachable)))

  (func (export "release") (result i32)
    (call $release_env (global.get $scope) (i32.const 0)))

  (func (export "eval") (param $ptr i32) (param $len i32) (result i32)
    (if (call $create_string (global.get $env) (local.get $ptr) (local.get $len)
          (i32.const 24))
      (then unreachable))
    (call $run_script (global.get $env) (i32.load (i32.const 24)) (i32.const 28)))

  ;; The named-property calls act on the global object, or on the last
  ;; script's result.
  (func (export "target_global") (global.set $target (i32.load (i32.const 16))))
  (func (export "target_result") (global.set $target (i32.load (i32.const 28))))

  (func (export "get") (param $name i32) (result i32)
    (call $get_named (global.get $env) (global.get $target) (local.get $name)
      (i32.const 36)))
  (func (export "set") (param $name i32) (param $value i32) (result i32)
    (if (call $create_int32 (global.get $env) (local.get $value) (i32.const 56))
      (then unreachable))
    (call $set_named (global.get $env) (global.get $target) (local.get $name)
      (i32.load (i32.const 56))))
  (func (export "has") (param $name i32) (result i32)
    (call $has_named (global.get $env) (global.get $target) (local.get $name)
      (i32.const 40)))

  ;; The value at 36 (a get's or a call's result) as an int32, at 44.
  (func (export "got_int32") (result i32)
    (call $get_int32 (global.get $env) (i32.load (i32.const 36)) (i32.const 44)))

  ;; The last script's result as a number: 0 int32 and 1 uint32 at 44,
  ;; 2 int64 and 3 double at 48.
  (func (export "number") (param $kind i32) (result i32)
    (if (i32.eq (local.get $kind) (i32.const 0))
      (then (return (call $get_int32 (global.get $env) (i32.load (i32.const 28))
        (i32.const 44)))))
    (if (i32.eq (local.get $kind) (i32.const 1))
      (then (return (call $get_uint32 (global.get $env) (i32.load (i32.const 28))
        (i32.const 44)))))
    (if (i32.eq (local.get $kind) (i32.const 2))
      (then (return (call $get_int64 (global.get $env) (i32.load (i32.const 28))
        (i32.const 48)))))
    (call $get_double (global.get $env) (i32.load (i32.const 28)) (i32.const 48)))

  ;; Calls the last script's result with the arguments 1..=argc.
  (func (export "call") (param $argc i32) (result i32)
    (local $i i32)
    (block $done
      (loop $next
        (br_if $done (i32.ge_u (local.get $i) (local.get $argc)))
        (if (call $create_int32 (global.get $env)
              (i32.add (local.get $i) (i32.const 1))
              (i32.add (i32.const 64) (i32.shl (local.get $i) (i32.const 2))))
          (then unreachable))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $next)))
    (call $call_function (global.get $env) (i32.load (i32.const 60))
      (i32.load (i32.const 28)) (local.get $argc) (i32.const 64) (i32.const 36)))
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

    fn write(&self, ptr: u32, bytes: &[u8]) {
        self.memory
            .view(&self.store)
            .write(u64::from(ptr), bytes)
            .unwrap();
    }

    fn read<const N: usize>(&self, ptr: u32) -> [u8; N] {
        let mut bytes = [0u8; N];
        self.memory
            .view(&self.store)
            .read(u64::from(ptr), &mut bytes)
            .unwrap();
        bytes
    }

    /// Writes `name` and its NUL at `ptr`.
    fn name(&self, ptr: u32, name: &str) {
        self.write(ptr, name.as_bytes());
        self.write(ptr + name.len() as u32, &[0]);
    }

    fn eval(&mut self, source: &str) {
        self.write(SCRIPT_PTR, source.as_bytes());
        let eval: TypedFunction<(i32, i32), i32> = self.typed("eval");
        let status = eval
            .call(&mut self.store, SCRIPT_PTR as i32, source.len() as i32)
            .expect("eval trapped");
        assert_eq!(status, NAPI_OK, "script failed: {source}");
    }

    fn target_global(&mut self) {
        let f: TypedFunction<(), ()> = self.typed("target_global");
        f.call(&mut self.store).unwrap();
    }

    fn target_result(&mut self) {
        let f: TypedFunction<(), ()> = self.typed("target_result");
        f.call(&mut self.store).unwrap();
    }

    /// `napi_get_named_property` of the name at `ptr`, as an int32.
    fn get(&mut self, ptr: u32) -> i32 {
        let get: TypedFunction<i32, i32> = self.typed("get");
        assert_eq!(get.call(&mut self.store, ptr as i32).unwrap(), NAPI_OK);
        self.got_int32()
    }

    fn got_int32(&mut self) -> i32 {
        let got: TypedFunction<(), i32> = self.typed("got_int32");
        assert_eq!(got.call(&mut self.store).unwrap(), NAPI_OK);
        i32::from_le_bytes(self.read(44))
    }

    fn set(&mut self, ptr: u32, value: i32) {
        let set: TypedFunction<(i32, i32), i32> = self.typed("set");
        assert_eq!(
            set.call(&mut self.store, ptr as i32, value).unwrap(),
            NAPI_OK
        );
    }

    fn has(&mut self, ptr: u32) -> bool {
        let has: TypedFunction<i32, i32> = self.typed("has");
        assert_eq!(has.call(&mut self.store, ptr as i32).unwrap(), NAPI_OK);
        self.read::<1>(40)[0] != 0
    }

    fn number(&mut self, kind: i32) -> i32 {
        let number: TypedFunction<i32, i32> = self.typed("number");
        number.call(&mut self.store, kind).unwrap()
    }

    fn call(&mut self, argc: u32) -> i32 {
        let call: TypedFunction<i32, i32> = self.typed("call");
        assert_eq!(call.call(&mut self.store, argc as i32).unwrap(), NAPI_OK);
        self.got_int32()
    }

    /// The names `touch()` gets from the global object.
    fn program(&self, names: &[u32]) {
        self.write(PROGRAM_PTR, &(names.len() as u32).to_le_bytes());
        for (index, name) in names.iter().enumerate() {
            self.write(PROGRAM_PTR + 4 + 4 * index as u32, &name.to_le_bytes());
        }
    }

    fn release(mut self) {
        let release: TypedFunction<(), i32> = self.typed("release");
        assert_eq!(release.call(&mut self.store).unwrap(), NAPI_OK);
    }
}

/// The cache set of a name at `guest_ptr`, as `NameCache::set_of` computes
/// it (src/guest/util.rs). Only to build collisions; nothing here depends on
/// its value otherwise.
fn cache_set(guest_ptr: u32) -> u32 {
    guest_ptr.wrapping_mul(0x9E37_79B1) >> 24
}

/// `count` addresses from `from` on, 64 bytes apart, in one cache set.
fn colliding_names(from: u32, count: usize) -> Vec<u32> {
    let set = cache_set(from);
    (0..)
        .map(|index| from + 64 * index)
        .filter(|&ptr| cache_set(ptr) == set)
        .take(count)
        .collect()
}

/// The same name at the same address is read right on every call: the first
/// reads it, the second numbers it, the later ones find its string.
#[test]
fn a_repeated_name_keeps_meaning_the_same_property() {
    let mut guest = Guest::new();
    guest.eval("globalThis.alpha = 11; globalThis.beta = 22;");
    guest.name(NAMES_PTR, "alpha");
    guest.name(NAMES_PTR + 64, "beta");
    for _ in 0..6 {
        assert_eq!(guest.get(NAMES_PTR), 11);
        assert_eq!(guest.get(NAMES_PTR + 64), 22);
        assert!(guest.has(NAMES_PTR));
    }
    for round in 0..6 {
        guest.set(NAMES_PTR, 100 + round);
        assert_eq!(guest.get(NAMES_PTR), 100 + round);
        assert_eq!(guest.get(NAMES_PTR + 64), 22);
    }
    guest.eval("alpha * 1000 + beta");
    assert_eq!(guest.number(0), NAPI_OK);
    assert_eq!(i32::from_le_bytes(guest.read(44)), 105_022);
    guest.release();
}

/// The guest changes the bytes at an address it passed before: each call
/// acts on the name that is there now, whatever the host remembered.
#[test]
fn a_name_rewritten_in_place_is_read_again() {
    let mut guest = Guest::new();
    guest.eval(
        "globalThis.ab = 1; globalThis.abc = 2; globalThis.xb = 3; globalThis.a = 4; \
         globalThis[''] = 5;",
    );
    for _ in 0..3 {
        // Each name long enough to be numbered before it is replaced.
        for (name, value) in [("ab", 1), ("abc", 2), ("xb", 3), ("a", 4), ("", 5), ("ab", 1)] {
            guest.name(NAMES_PTR, name);
            for _ in 0..4 {
                assert_eq!(guest.get(NAMES_PTR), value, "name {name:?}");
                assert!(guest.has(NAMES_PTR), "name {name:?}");
            }
        }
    }
    // A name the object does not have, at the address of one it has.
    guest.name(NAMES_PTR, "ab");
    for _ in 0..4 {
        assert!(guest.has(NAMES_PTR));
    }
    guest.name(NAMES_PTR, "ac");
    for _ in 0..4 {
        assert!(!guest.has(NAMES_PTR));
    }
    // A store through the rewritten name makes that property, not the old one.
    for _ in 0..4 {
        guest.set(NAMES_PTR, 77);
    }
    guest.eval("globalThis.ac * 100 + globalThis.ab");
    assert_eq!(guest.number(0), NAPI_OK);
    assert_eq!(i32::from_le_bytes(guest.read(44)), 7701);
    guest.release();
}

/// A buffer the guest formats a different name into on every call.
#[test]
fn a_name_that_changes_on_every_call_is_read_every_time() {
    let mut guest = Guest::new();
    guest.eval("for (let i = 0; i < 300; i++) globalThis['k' + i] = i;");
    for round in 0..3 {
        for index in 0..300 {
            guest.name(NAMES_PTR, &format!("k{index}"));
            assert_eq!(guest.get(NAMES_PTR), index, "round {round}");
        }
    }
    guest.release();
}

/// More names than the cache holds, and many per cache set: a name pushed
/// out is read again.
#[test]
fn names_beyond_the_cache_stay_right() {
    let mut guest = Guest::new();
    guest.eval("for (let i = 0; i < 1500; i++) globalThis['p' + i] = i;");
    let ptr = |index: u32| NAMES_PTR + 16 * index;
    for index in 0..1500 {
        guest.name(ptr(index), &format!("p{index}"));
    }
    for round in 0..4 {
        for index in 0..1500 {
            assert_eq!(guest.get(ptr(index)), index as i32, "round {round}");
        }
    }
    // Each name several times in a row, so every one is numbered and then
    // pushed out by a later one.
    for index in 0..1500 {
        for _ in 0..3 {
            assert_eq!(guest.get(ptr(index)), index as i32);
        }
    }
    for index in (0..1500).rev() {
        assert_eq!(guest.get(ptr(index)), index as i32);
    }
    guest.release();
}

/// Names of the lengths around what the cache holds, and far past it.
#[test]
fn long_names_are_read_whole() {
    let mut guest = Guest::new();
    let mut ptr = NAMES_PTR;
    let lens = [1usize, 7, 8, 9, 15, 16, 38, 39, 40, 41, 127, 128, 129, 300];
    for len in lens {
        let name = "n".repeat(len);
        guest.eval(&format!("globalThis['{name}'] = {len};"));
        guest.name(ptr, &name);
        for _ in 0..4 {
            assert_eq!(guest.get(ptr), len as i32);
            assert!(guest.has(ptr));
        }
        // One byte shorter is another property, at the same address: one
        // the object has only if an earlier round made it.
        guest.write(ptr + len as u32 - 1, &[0]);
        for _ in 0..4 {
            assert_eq!(guest.has(ptr), lens.contains(&(len - 1)), "length {len}");
        }
        ptr += 512;
    }
    guest.release();
}

/// A get runs a getter, which calls the guest, which gets other names that
/// want the cache slot the outer name is being read through. The outer call
/// still gets its own property, and so does every later one.
#[test]
fn a_nested_call_does_not_take_the_name_of_the_call_around_it() {
    let mut guest = Guest::new();
    let names = colliding_names(NAMES_PTR, 3);
    let (outer, first, second) = (names[0], names[1], names[2]);
    guest.name(outer, "trap");
    guest.name(first, "one");
    guest.name(second, "two");
    guest.eval(
        "globalThis.one = 1; globalThis.two = 2; globalThis.calls = 0; \
         globalThis.subject = { \
           get trap() { calls++; touch(); return 7; }, \
           set trap(v) { calls++; touch(); this.stored = v; } }; \
         subject",
    );
    // Number the outer name on the plain object first.
    guest.target_result();
    guest.program(&[]);
    for _ in 0..4 {
        assert_eq!(guest.get(outer), 7);
    }
    // In the getter: number `first` in the set's other way, then `second`,
    // which can only go where `outer` is.
    guest.program(&[first, first, first, second, second, second, first, second]);
    for _ in 0..4 {
        assert_eq!(guest.get(outer), 7);
    }
    for value in [31, 32, 33] {
        guest.set(outer, value);
        guest.eval("subject.stored");
        assert_eq!(guest.number(0), NAPI_OK);
        assert_eq!(i32::from_le_bytes(guest.read(44)), value);
        guest.eval("subject");
        guest.target_result();
    }
    // The names the nested calls used mean what they say afterwards.
    guest.target_global();
    for _ in 0..3 {
        assert_eq!(guest.get(first), 1);
        assert_eq!(guest.get(second), 2);
    }
    guest.eval("calls");
    assert_eq!(guest.number(0), NAPI_OK);
    assert_eq!(i32::from_le_bytes(guest.read(44)), 11);
    guest.release();
}

/// Two guests, each with its own isolate, on one thread: the same address
/// and cache slot hold different names in each.
#[test]
fn two_guests_number_their_names_apart() {
    let mut a = Guest::new();
    let mut b = Guest::new();
    a.eval("globalThis.left = 1; globalThis.right = 2;");
    b.eval("globalThis.left = 10; globalThis.right = 20;");
    a.name(NAMES_PTR, "left");
    b.name(NAMES_PTR, "right");
    for _ in 0..5 {
        assert_eq!(a.get(NAMES_PTR), 1);
        assert_eq!(b.get(NAMES_PTR), 20);
    }
    a.name(NAMES_PTR, "right");
    b.name(NAMES_PTR, "left");
    for _ in 0..5 {
        assert_eq!(a.get(NAMES_PTR), 2);
        assert_eq!(b.get(NAMES_PTR), 10);
    }
    a.release();
    b.release();
}

/// `napi_call_function` hands its arguments to V8 in place, from the stack
/// for the usual counts.
#[test]
fn a_call_passes_every_argument() {
    let mut guest = Guest::new();
    for argc in [0u32, 1, 2, 7, 8, 9, 20, 32] {
        guest.eval(
            "(function () { let sum = 0; for (const a of arguments) sum += a; \
             return arguments.length * 1000 + sum; })",
        );
        let expected = argc * 1000 + argc * (argc + 1) / 2;
        for _ in 0..3 {
            assert_eq!(guest.call(argc), expected as i32, "argc {argc}");
        }
    }
    guest.release();
}

/// The number getters, for values V8 keeps in the handle (31-bit integers)
/// and for those it boxes.
#[test]
fn numbers_convert_as_n_api_says() {
    let mut guest = Guest::new();
    let cases: [(&str, i32, u32, i64, f64); 14] = [
        ("0", 0, 0, 0, 0.0),
        ("42", 42, 42, 42, 42.0),
        ("-1", -1, u32::MAX, -1, -1.0),
        ("1073741823", 1_073_741_823, 1_073_741_823, 1_073_741_823, 1_073_741_823.0),
        ("1073741824", 1_073_741_824, 1_073_741_824, 1_073_741_824, 1_073_741_824.0),
        ("-1073741824", -1_073_741_824, 3_221_225_472, -1_073_741_824, -1_073_741_824.0),
        ("-1073741825", -1_073_741_825, 3_221_225_471, -1_073_741_825, -1_073_741_825.0),
        ("2147483647", i32::MAX, 2_147_483_647, 2_147_483_647, 2_147_483_647.0),
        ("2147483648", i32::MIN, 2_147_483_648, 2_147_483_648, 2_147_483_648.0),
        ("4294967301", 5, 5, 4_294_967_301, 4_294_967_301.0),
        ("3.7", 3, 3, 3, 3.7),
        ("-3.7", -3, 4_294_967_293, -3, -3.7),
        ("-0", 0, 0, 0, -0.0),
        ("NaN", 0, 0, 0, f64::NAN),
    ];
    for (source, int32, uint32, int64, double) in cases {
        guest.eval(source);
        assert_eq!(guest.number(0), NAPI_OK);
        assert_eq!(i32::from_le_bytes(guest.read(44)), int32, "int32 of {source}");
        assert_eq!(guest.number(1), NAPI_OK);
        assert_eq!(u32::from_le_bytes(guest.read(44)), uint32, "uint32 of {source}");
        assert_eq!(guest.number(2), NAPI_OK);
        assert_eq!(i64::from_le_bytes(guest.read(48)), int64, "int64 of {source}");
        assert_eq!(guest.number(3), NAPI_OK);
        let got = f64::from_le_bytes(guest.read(48));
        assert!(
            got.to_bits() == double.to_bits() || (got.is_nan() && double.is_nan()),
            "double of {source}: {got}"
        );
    }
    // Not a number: napi_number_expected (6), from every getter.
    guest.eval("'42'");
    for kind in 0..4 {
        assert_eq!(guest.number(kind), 6);
    }
    guest.release();
}
