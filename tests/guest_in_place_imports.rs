//! Which N-API imports run on the Wasm stack they were called from, and that
//! the ones that do not keep V8 on the thread's own stack: V8 15.9 aborts
//! with `Check failed: IsOnCentralStack()` when it collects garbage or throws
//! anywhere else.

#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Extern, Imports, Instance, Memory, Module, Store, TypedFunction};
use wasmer_napi::NapiCtx;

/// The imports `napi_import_runs_in_place` lists (src/guest/napi.rs). Kept
/// here too, so that widening the list takes a second, deliberate edit.
const IN_PLACE: &[&str] = &[
    "napi_get_undefined",
    "napi_get_null",
    "napi_get_boolean",
    "napi_get_global",
    "napi_get_value_double",
    "napi_get_value_int32",
    "napi_get_value_uint32",
    "napi_get_value_int64",
    "napi_get_value_bool",
    "napi_get_value_bigint_int64",
    "napi_get_value_bigint_uint64",
    "napi_get_date_value",
    "napi_get_value_external",
    "napi_typeof",
    "napi_is_array",
    "napi_is_error",
    "napi_is_arraybuffer",
    "napi_is_typedarray",
    "napi_is_dataview",
    "napi_is_date",
    "napi_is_promise",
    "napi_is_buffer",
    "napi_strict_equals",
    "napi_get_array_length",
    "napi_get_cb_info",
    "napi_get_new_target",
    "napi_open_handle_scope",
    "napi_close_handle_scope",
    "napi_open_escapable_handle_scope",
    "napi_close_escapable_handle_scope",
    "napi_escape_handle",
    "napi_get_reference_value",
    "napi_is_exception_pending",
    // In place for a small integer only.
    "napi_create_int32",
    "napi_create_uint32",
    "napi_create_int64",
    "napi_create_double",
];

const SOURCE_PTR: u32 = 4096;

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
  (import "napi" "napi_get_value_double" (func $get_double (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_int32" (func $create_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_double" (func $create_double (param i32 f64 i32) (result i32)))
  (import "napi" "napi_get_value_uint32" (func $get_uint32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_uint32" (func $create_uint32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_int64" (func $get_int64 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_int64" (func $create_int64 (param i32 i64 i32) (result i32)))
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_get_undefined" (func $get_undefined (param i32 i32) (result i32)))
  (import "napi" "napi_typeof" (func $typeof (param i32 i32 i32) (result i32)))
  (import "napi" "napi_strict_equals" (func $strict_equals (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_named_property (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_function"
    (func $create_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_cb_info"
    (func $get_cb_info (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_reference"
    (func $create_reference (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_reference_value"
    (func $get_reference_value (param i32 i32 i32) (result i32)))
  (import "napi" "napi_open_handle_scope" (func $open_scope (param i32 i32) (result i32)))
  (import "napi" "napi_close_handle_scope" (func $close_scope (param i32 i32) (result i32)))
  (import "napi" "napi_open_escapable_handle_scope"
    (func $open_escapable (param i32 i32) (result i32)))
  (import "napi" "napi_close_escapable_handle_scope"
    (func $close_escapable (param i32 i32) (result i32)))
  (import "napi" "napi_escape_handle" (func $escape (param i32 i32 i32 i32) (result i32)))

  ;; 8 env, 12 scope, 28 string, 32 script result, 48 global, 52 function,
  ;; 56 result handle, 60 argc, 64 argv[2], 72 this, 76 scope id, 80 double,
  ;; 88 handle, 92 reference, 96 bool/type out.
  (memory (export "memory") 2)
  (table (export "__indirect_function_table") 8 funcref)
  (global $env (mut i32) (i32.const 0))
  (global $scope (mut i32) (i32.const 0))
  (data (i32.const 100) "f\00")
  (data (i32.const 104) "same\00")
  (data (i32.const 112) "d\00")
  (data (i32.const 116) "u\00")
  (data (i32.const 120) "l\00")

  (func $define (param $name i32) (param $index i32)
    (if (call $create_function (global.get $env) (local.get $name) (i32.const -1)
          (local.get $index) (i32.const 0) (i32.const 52))
      (then unreachable))
    (if (call $get_global (global.get $env) (i32.const 48)) (then unreachable))
    (if (call $set_named_property (global.get $env) (i32.load (i32.const 48))
          (local.get $name) (i32.load (i32.const 52)))
      (then unreachable)))

  (func (export "create")
    (if (call $configure (i32.const 0)) (then unreachable))
    (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
      (then unreachable))
    (global.set $env (i32.load (i32.const 8)))
    (global.set $scope (i32.load (i32.const 12)))
    (call $define (i32.const 100) (i32.const 1))
    (call $define (i32.const 104) (i32.const 2))
    (call $define (i32.const 112) (i32.const 3))
    (call $define (i32.const 116) (i32.const 4))
    (call $define (i32.const 120) (i32.const 5))
    ;; A strong reference to the global object, for `same`.
    (if (call $get_global (global.get $env) (i32.const 48)) (then unreachable))
    (if (call $create_reference (global.get $env) (i32.load (i32.const 48)) (i32.const 1)
          (i32.const 92))
      (then unreachable)))

  (func (export "release") (result i32)
    (call $release_env (global.get $scope) (i32.const 0)))

  (func (export "eval") (param $ptr i32) (param $len i32) (result i32)
    (if (call $create_string (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28))
      (then unreachable))
    (call $run_script (global.get $env) (i32.load (i32.const 28)) (i32.const 32)))

  ;; n x napi_create_double of a value that is not a small integer: each one
  ;; allocates a heap number, so V8 collects the young generation many times.
  (func (export "loop_create_double") (param $n i32)
    (local $i i32)
    (loop $outer
      (if (call $open_scope (global.get $env) (i32.const 76)) (then unreachable))
      (local.set $i (i32.const 256))
      (loop $inner
        (if (call $create_double (global.get $env) (f64.const 1.5) (i32.const 88))
          (then unreachable))
        (if (call $get_double (global.get $env) (i32.load (i32.const 88)) (i32.const 80))
          (then unreachable))
        (if (f64.ne (f64.load (i32.const 80)) (f64.const 1.5)) (then unreachable))
        (local.set $n (i32.sub (local.get $n) (i32.const 1)))
        (local.set $i (i32.sub (local.get $i) (i32.const 1)))
        (br_if $inner (i32.and (i32.ne (local.get $i) (i32.const 0))
                               (i32.gt_s (local.get $n) (i32.const 0)))))
      (if (call $close_scope (global.get $env) (i32.load (i32.const 76))) (then unreachable))
      (br_if $outer (i32.gt_s (local.get $n) (i32.const 0)))))

  ;; Recurses $n deep, then runs $work x (napi_get_undefined, napi_create_double,
  ;; napi_get_value_double) at the bottom of the stack. Returns the depth.
  (func $deep (export "deep") (param $n i32) (param $work i32) (result i32)
    (if (result i32) (local.get $n)
      (then
        ;; Not a tail call: every level keeps its frame.
        (i32.add (i32.const 1)
          (call $deep (i32.sub (local.get $n) (i32.const 1)) (local.get $work))))
      (else
        (if (local.get $work)
          (then
            (if (call $open_scope (global.get $env) (i32.const 76)) (then unreachable))
            (loop $again
              (if (call $get_undefined (global.get $env) (i32.const 88)) (then unreachable))
              (if (call $create_double (global.get $env) (f64.const 2.5) (i32.const 88))
                (then unreachable))
              (if (call $get_double (global.get $env) (i32.load (i32.const 88)) (i32.const 80))
                (then unreachable))
              (if (f64.ne (f64.load (i32.const 80)) (f64.const 2.5)) (then unreachable))
              (br_if $again (local.tee $work (i32.sub (local.get $work) (i32.const 1)))))
            (if (call $close_scope (global.get $env) (i32.load (i32.const 76)))
              (then unreachable))))
        (i32.const 0))))

  ;; f(x): returns x as a new int32. The argument is read in place; the
  ;; result allocates when x is not a small integer.
  (func $f (param $env i32) (param $info i32) (result i32)
    (i32.store (i32.const 60) (i32.const 1))
    (if (call $get_cb_info (local.get $env) (local.get $info) (i32.const 60) (i32.const 64)
          (i32.const 72) (i32.const 0))
      (then unreachable))
    (if (call $get_int32 (local.get $env) (i32.load (i32.const 64)) (i32.const 80))
      (then unreachable))
    (if (call $create_int32 (local.get $env) (i32.load (i32.const 80)) (i32.const 56))
      (then unreachable))
    (i32.load (i32.const 56)))

  ;; same(a, b): 1 if `this` is the referenced global object, a and b are
  ;; strictly equal and of the same type, else 0; returned through an
  ;; escapable scope.
  (func $same (param $env i32) (param $info i32) (result i32)
    (local $ok i32)
    (if (call $open_escapable (local.get $env) (i32.const 76)) (then unreachable))
    (i32.store (i32.const 60) (i32.const 2))
    (if (call $get_cb_info (local.get $env) (local.get $info) (i32.const 60) (i32.const 64)
          (i32.const 72) (i32.const 0))
      (then unreachable))
    (if (i32.ne (i32.load (i32.const 60)) (i32.const 2)) (then unreachable))
    (if (call $strict_equals (local.get $env) (i32.load (i32.const 64))
          (i32.load (i32.const 68)) (i32.const 96))
      (then unreachable))
    (local.set $ok (i32.load8_u (i32.const 96)))
    (if (call $typeof (local.get $env) (i32.load (i32.const 64)) (i32.const 96))
      (then unreachable))
    (local.set $info (i32.load (i32.const 96)))
    (if (call $typeof (local.get $env) (i32.load (i32.const 68)) (i32.const 96))
      (then unreachable))
    (local.set $ok (i32.and (local.get $ok)
                            (i32.eq (local.get $info) (i32.load (i32.const 96)))))
    (if (call $get_reference_value (local.get $env) (i32.load (i32.const 92)) (i32.const 88))
      (then unreachable))
    (if (call $strict_equals (local.get $env) (i32.load (i32.const 72))
          (i32.load (i32.const 88)) (i32.const 96))
      (then unreachable))
    (local.set $ok (i32.and (local.get $ok) (i32.load8_u (i32.const 96))))
    (if (call $create_int32 (local.get $env) (local.get $ok) (i32.const 56))
      (then unreachable))
    (if (call $escape (local.get $env) (i32.load (i32.const 76)) (i32.load (i32.const 56))
          (i32.const 56))
      (then unreachable))
    (if (call $close_escapable (local.get $env) (i32.load (i32.const 76))) (then unreachable))
    (i32.load (i32.const 56)))

  ;; d(x), u(x), l(x): x read as a double, a uint32 or an int64 and made
  ;; into a number of that kind again.
  (func $arg (param $env i32) (param $info i32)
    (i32.store (i32.const 60) (i32.const 1))
    (if (call $get_cb_info (local.get $env) (local.get $info) (i32.const 60) (i32.const 64)
          (i32.const 72) (i32.const 0))
      (then unreachable)))
  (func $d (param $env i32) (param $info i32) (result i32)
    (call $arg (local.get $env) (local.get $info))
    (if (call $get_double (local.get $env) (i32.load (i32.const 64)) (i32.const 80))
      (then unreachable))
    (if (call $create_double (local.get $env) (f64.load (i32.const 80)) (i32.const 56))
      (then unreachable))
    (i32.load (i32.const 56)))
  (func $u (param $env i32) (param $info i32) (result i32)
    (call $arg (local.get $env) (local.get $info))
    (if (call $get_uint32 (local.get $env) (i32.load (i32.const 64)) (i32.const 80))
      (then unreachable))
    (if (call $create_uint32 (local.get $env) (i32.load (i32.const 80)) (i32.const 56))
      (then unreachable))
    (i32.load (i32.const 56)))
  (func $l (param $env i32) (param $info i32) (result i32)
    (call $arg (local.get $env) (local.get $info))
    (if (call $get_int64 (local.get $env) (i32.load (i32.const 64)) (i32.const 80))
      (then unreachable))
    (if (call $create_int64 (local.get $env) (i64.load (i32.const 80)) (i32.const 56))
      (then unreachable))
    (i32.load (i32.const 56)))
  (elem (i32.const 1) $f $same $d $u $l)
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

    fn eval(&mut self, source: &str) -> i32 {
        self.memory
            .view(&self.store)
            .write(u64::from(SOURCE_PTR), source.as_bytes())
            .unwrap();
        let eval: TypedFunction<(i32, i32), i32> = self.typed("eval");
        eval.call(&mut self.store, SOURCE_PTR as i32, source.len() as i32)
            .expect("eval trapped")
    }

    fn release(&mut self) {
        let release: TypedFunction<(), i32> = self.typed("release");
        assert_eq!(release.call(&mut self.store).expect("release trapped"), 0);
    }
}

#[test]
fn only_the_audited_imports_run_in_place() {
    let mut store = Store::default();
    let module = Module::new(&store, wat::parse_str(WAT).unwrap()).unwrap();
    let ctx = NapiCtx::default();
    let (imports, _state): (Imports, _) = ctx
        .runtime_hooks()
        .additional_imports(&module, &mut store.as_store_mut())
        .expect("failed to create N-API imports");
    let mut in_place = Vec::new();
    let mut functions = 0;
    for (module, name, import) in imports.iter() {
        if !module.starts_with("napi") {
            continue;
        }
        let Extern::Function(function) = import else {
            continue;
        };
        functions += 1;
        if !function.calls_on_native_stack(&store) {
            assert_eq!(module, "napi", "{name} runs in place");
            in_place.push(name.to_string());
        }
    }
    assert!(functions > 200, "only {functions} N-API imports");
    in_place.sort();
    let mut expected: Vec<String> = IN_PLACE.iter().map(|name| name.to_string()).collect();
    expected.sort();
    assert_eq!(in_place, expected);
}

/// An import that allocates on the V8 heap triggers collections; on the Wasm
/// stack V8 would abort on the first one.
#[test]
fn allocating_imports_collect_garbage_on_the_thread_stack() {
    let mut guest = Guest::new();
    let run: TypedFunction<i32, ()> = guest.typed("loop_create_double");
    run.call(&mut guest.store, 3_000_000).unwrap();
    guest.release();
}

/// A callback whose argument reads run in place and whose result allocates,
/// called often enough that V8 collects garbage between them.
#[test]
fn callbacks_mix_in_place_and_allocating_imports() {
    let mut guest = Guest::new();
    let script = "(() => {
        const base = 0x40000000, n = 2000000;
        let sum = 0;
        for (let i = 0; i < n; i++) sum += f(base + i) - base;
        if (sum !== n * (n - 1) / 2) throw new Error('f: ' + sum);
        for (let i = 0; i < 1000; i++) {
            if (f(i) !== i || f(-i) !== -i) throw new Error('f small: ' + i);
        }
        // Numbers on both sides of V8's small integers, each made by every
        // import that can make it.
        const is = Object.is;
        for (const x of [0, 1, -1, 5, 0x3fffffff, 0x40000000, -0x40000000, -0x40000001,
                         0x7fffffff, -0x80000000]) {
            if (!is(f(x), x) || !is(d(x), x) || !is(l(x), x)) throw new Error('int32: ' + x);
        }
        for (const x of [0, 1, 0x3fffffff, 0x40000000, 0x7fffffff, 0x80000000, 0xffffffff]) {
            if (!is(u(x), x) || !is(d(x), x) || !is(l(x), x)) throw new Error('uint32: ' + x);
        }
        for (const x of [1.5, -0, NaN, Infinity, -Infinity, 2 ** 32, -(2 ** 31) - 1, 1e300,
                         -1e-300, 0.1]) {
            if (!is(d(x), x)) throw new Error('double: ' + x);
        }
        for (const x of [2 ** 32, -(2 ** 32), 2 ** 53 - 1, -(2 ** 53) + 1]) {
            if (!is(l(x), x)) throw new Error('int64: ' + x);
        }
        const o = {};
        const g = globalThis;
        if (same.call(g, o, o) !== 1) throw new Error('same object');
        if (same.call(g, o, {}) !== 0) throw new Error('other object');
        // A rope against a flat string.
        if (same.call(g, 'abcdefghijklmnopqrstuvwxyz' + n, 'abcdefghijklmnopqrstuvwxyz2000000') !== 1) {
            throw new Error('same string');
        }
        if (same.call(g, 1, '1') !== 0) throw new Error('number and string');
        if (same.call(g, 1.5, 1.5) !== 1) throw new Error('same number');
        if (same.call({}, o, o) !== 0) throw new Error('other receiver');
    })()";
    assert_eq!(guest.eval(script), 0);
    guest.release();
}

/// With little of the Wasm stack left, Wasmer runs every import on the
/// thread's stack, also the ones that would run in place.
#[test]
fn imports_work_at_the_bottom_of_a_nearly_full_wasm_stack() {
    let mut guest = Guest::new();
    let deep: TypedFunction<(i32, i32), i32> = guest.typed("deep");
    // The deepest recursion the Wasm stack takes: an overflow is a trap the
    // store survives.
    let (mut fits, mut overflows) = (0, 1024);
    while deep.call(&mut guest.store, overflows, 0).is_ok() {
        fits = overflows;
        overflows *= 2;
        assert!(overflows < 1 << 28, "the recursion never overflows");
    }
    while overflows - fits > 1 {
        let middle = fits + (overflows - fits) / 2;
        if deep.call(&mut guest.store, middle, 0).is_ok() {
            fits = middle;
        } else {
            overflows = middle;
        }
    }
    // A thirty-second of the stack is left at the first depth: far less than
    // the 256 KiB an in-place call needs, unless the stack is over 8 MiB.
    //
    // A guest on the thread's own stack (`WASMER_WASM_ON_THREAD_STACK=1`) has
    // no other stack to run an import on: there, a host call with less than
    // the reserve left is the guest's stack overflow, a trap the store
    // survives, and the shallower depths work as everywhere.
    let on_thread_stack = std::env::var_os("WASMER_WASM_ON_THREAD_STACK").is_some_and(|v| v == "1");
    for depth in [fits / 32 * 31, fits / 2, 8] {
        match deep.call(&mut guest.store, depth, 2_000) {
            Ok(reached) => assert_eq!(reached, depth),
            Err(trap) => {
                assert!(on_thread_stack && depth == fits / 32 * 31, "{trap}");
                assert!(
                    trap.to_string().contains("stack"),
                    "a stack overflow trap, not {trap}"
                );
            }
        }
    }
    guest.release();
}

/// Reviewer test: two isolates on one thread, where the first bridge call of
/// each exported function is an in-place import, so the resident isolate is
/// switched (Isolate::Exit/Enter) on the Wasm stack; then allocation,
/// collections and a JavaScript stack overflow must still behave.
#[test]
fn reviewer_two_isolates_switch_inside_in_place_imports() {
    let mut a = Guest::new();
    let mut b = Guest::new();
    let script = "(() => {
        let depth = 0;
        function r() { depth++; r(); }
        try { r(); } catch (e) { if (!(e instanceof RangeError)) throw e; }
        if (depth < 100) throw new Error('depth ' + depth);
        let s = 0;
        const n = 20000, base = 0x40000000;
        for (let i = 0; i < n; i++) s += f(base + i) - base;
        if (s !== n * (n - 1) / 2) throw new Error('sum ' + s);
    })()";
    for round in 0..100 {
        let (first, second) = if round % 2 == 0 {
            (&mut a, &mut b)
        } else {
            (&mut b, &mut a)
        };
        let deep: TypedFunction<(i32, i32), i32> = first.typed("deep");
        assert_eq!(deep.call(&mut first.store, 8, 500).unwrap(), 8);
        let run: TypedFunction<i32, ()> = second.typed("loop_create_double");
        run.call(&mut second.store, 20_000).unwrap();
        let deep: TypedFunction<(i32, i32), i32> = first.typed("deep");
        assert_eq!(deep.call(&mut first.store, 8, 500).unwrap(), 8);
        assert_eq!(second.eval(script), 0);
        let run: TypedFunction<i32, ()> = first.typed("loop_create_double");
        run.call(&mut first.store, 20_000).unwrap();
        assert_eq!(first.eval(script), 0);
    }
    a.release();
    b.release();
}
