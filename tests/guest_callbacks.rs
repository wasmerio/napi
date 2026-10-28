//! JavaScript calling guest functions through real Wasmer dispatch, the
//! bridge and V8: nested callbacks, scopes a callback leaves open, return
//! values created in them, argument counts, missing arguments, and envs
//! created one after another on one thread.

#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Engine, Imports, Instance, Memory, Module, Store, TypedFunction};
use wasmer_napi::NapiCtx;

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
  (import "napi" "napi_create_int32" (func $create_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_named_property (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_function"
    (func $create_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_cb_info"
    (func $get_cb_info (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_call_function"
    (func $call_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_open_handle_scope" (func $open_scope (param i32 i32) (result i32)))
  (import "napi" "napi_close_handle_scope" (func $close_scope (param i32 i32) (result i32)))
  (import "napi" "napi_open_escapable_handle_scope"
    (func $open_esc (param i32 i32) (result i32)))
  (import "napi" "napi_escape_handle" (func $escape (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_typeof" (func $typeof (param i32 i32 i32) (result i32)))

  ;; 8 env, 12 scope, 28 string, 32 script result, 48 global, 52 function,
  ;; 72 int out, 200..228 scratch. Callbacks take 64- or 256-byte frames from
  ;; a stack starting at 32768 so nested calls keep their own out-slots.
  (memory (export "memory") 2)
  (table (export "__indirect_function_table") 16 funcref)
  (global $env (mut i32) (i32.const 0))
  (global $scope (mut i32) (i32.const 0))
  (global $sp (mut i32) (i32.const 32768))
  (global $err (mut i32) (i32.const 0))
  (global $stale_id (mut i32) (i32.const 0))
  (data (i32.const 100) "rec\00")
  (data (i32.const 108) "leak\00")
  (data (i32.const 116) "many20\00")
  (data (i32.const 124) "stale\00")
  (data (i32.const 132) "probe\00")
  (data (i32.const 156) "many16\00")
  (data (i32.const 164) "undef\00")
  (data (i32.const 172) "badret\00")

  ;; Records the first failing step; the host reads it through `err`.
  (func $fail (param $code i32)
    (if (i32.eqz (global.get $err)) (then (global.set $err (local.get $code)))))

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
    (call $define (i32.const 108) (i32.const 2))
    (call $define (i32.const 116) (i32.const 3))
    (call $define (i32.const 124) (i32.const 4))
    (call $define (i32.const 132) (i32.const 5))
    (call $define (i32.const 156) (i32.const 6))
    (call $define (i32.const 164) (i32.const 7))
    (call $define (i32.const 172) (i32.const 8)))

  (func (export "release") (result i32)
    (call $release_env (global.get $scope) (i32.const 0)))

  (func (export "err") (result i32) (global.get $err))

  (func (export "eval") (param $ptr i32) (param $len i32) (result i32)
    (if (call $create_string (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28))
      (then unreachable))
    (call $run_script (global.get $env) (i32.load (i32.const 28)) (i32.const 32)))

  (func (export "result_int") (result i32)
    (if (call $get_int32 (global.get $env) (i32.load (i32.const 32)) (i32.const 72))
      (then (return (i32.const -999999))))
    (i32.load (i32.const 72)))

  ;; rec(n, step): inside its own scope, calls step(n - 1), which calls rec
  ;; again; then re-reads its own callback info after the inner callbacks
  ;; ended and checks its first ids still resolve. Returns n + step(n - 1).
  (func $rec (param $env i32) (param $info i32) (result i32)
    (local $f i32) (local $n i32)
    (local.set $f (global.get $sp))
    (global.set $sp (i32.add (global.get $sp) (i32.const 64)))
    (i32.store (local.get $f) (i32.const 2))
    (if (call $get_cb_info (local.get $env) (local.get $info) (local.get $f)
          (i32.add (local.get $f) (i32.const 4)) (i32.add (local.get $f) (i32.const 12)) (i32.const 0))
      (then (call $fail (i32.const 1))))
    (if (i32.ne (i32.load (local.get $f)) (i32.const 2)) (then (call $fail (i32.const 2))))
    (if (call $get_int32 (local.get $env) (i32.load offset=4 (local.get $f))
          (i32.add (local.get $f) (i32.const 16)))
      (then (call $fail (i32.const 3))))
    (local.set $n (i32.load offset=16 (local.get $f)))
    (if (i32.gt_s (local.get $n) (i32.const 0))
      (then
        (if (call $open_scope (local.get $env) (i32.add (local.get $f) (i32.const 56)))
          (then (call $fail (i32.const 4))))
        (if (call $create_int32 (local.get $env) (i32.sub (local.get $n) (i32.const 1))
              (i32.add (local.get $f) (i32.const 28)))
          (then (call $fail (i32.const 5))))
        (if (call $call_function (local.get $env) (i32.load offset=12 (local.get $f))
              (i32.load offset=8 (local.get $f)) (i32.const 1)
              (i32.add (local.get $f) (i32.const 28)) (i32.add (local.get $f) (i32.const 20)))
          (then (call $fail (i32.const 6))))
        (if (call $get_int32 (local.get $env) (i32.load offset=20 (local.get $f))
              (i32.add (local.get $f) (i32.const 24)))
          (then (call $fail (i32.const 7))))
        (i32.store offset=32 (local.get $f) (i32.const 2))
        (if (call $get_cb_info (local.get $env) (local.get $info) (i32.add (local.get $f) (i32.const 32))
              (i32.add (local.get $f) (i32.const 36)) (i32.const 0) (i32.const 0))
          (then (call $fail (i32.const 8))))
        (if (i32.ne (i32.load offset=32 (local.get $f)) (i32.const 2)) (then (call $fail (i32.const 9))))
        (if (call $get_int32 (local.get $env) (i32.load offset=36 (local.get $f))
              (i32.add (local.get $f) (i32.const 44)))
          (then (call $fail (i32.const 10))))
        (if (i32.ne (i32.load offset=44 (local.get $f)) (local.get $n)) (then (call $fail (i32.const 11))))
        (if (call $get_int32 (local.get $env) (i32.load offset=4 (local.get $f))
              (i32.add (local.get $f) (i32.const 44)))
          (then (call $fail (i32.const 12))))
        (if (i32.ne (i32.load offset=44 (local.get $f)) (local.get $n)) (then (call $fail (i32.const 13))))
        (if (call $close_scope (local.get $env) (i32.load offset=56 (local.get $f)))
          (then (call $fail (i32.const 15))))
        (if (call $create_int32 (local.get $env)
              (i32.add (local.get $n) (i32.load offset=24 (local.get $f)))
              (i32.add (local.get $f) (i32.const 48)))
          (then (call $fail (i32.const 14)))))
      (else
        (if (call $create_int32 (local.get $env) (i32.const 0) (i32.add (local.get $f) (i32.const 48)))
          (then (call $fail (i32.const 16))))))
    (global.set $sp (local.get $f))
    (i32.load offset=48 (local.get $f)))

  ;; leak(x): returns 2x through an escapable scope it never closes, and
  ;; leaves two more plain scopes open; the bridge closes all three.
  (func $leak (param $env i32) (param $info i32) (result i32)
    (local $f i32)
    (local.set $f (global.get $sp))
    (global.set $sp (i32.add (global.get $sp) (i32.const 64)))
    (i32.store (local.get $f) (i32.const 1))
    (if (call $get_cb_info (local.get $env) (local.get $info) (local.get $f)
          (i32.add (local.get $f) (i32.const 4)) (i32.const 0) (i32.const 0))
      (then (call $fail (i32.const 21))))
    (if (call $get_int32 (local.get $env) (i32.load offset=4 (local.get $f))
          (i32.add (local.get $f) (i32.const 8)))
      (then (call $fail (i32.const 22))))
    (if (call $open_esc (local.get $env) (i32.add (local.get $f) (i32.const 12)))
      (then (call $fail (i32.const 23))))
    (if (call $create_int32 (local.get $env) (i32.mul (i32.load offset=8 (local.get $f)) (i32.const 2))
          (i32.add (local.get $f) (i32.const 16)))
      (then (call $fail (i32.const 24))))
    (if (call $escape (local.get $env) (i32.load offset=12 (local.get $f))
          (i32.load offset=16 (local.get $f)) (i32.add (local.get $f) (i32.const 20)))
      (then (call $fail (i32.const 25))))
    (if (call $open_scope (local.get $env) (i32.add (local.get $f) (i32.const 24)))
      (then (call $fail (i32.const 26))))
    (if (call $create_int32 (local.get $env) (i32.const 99) (i32.add (local.get $f) (i32.const 28)))
      (then (call $fail (i32.const 27))))
    (global.set $stale_id (i32.load offset=28 (local.get $f)))
    (if (call $open_scope (local.get $env) (i32.add (local.get $f) (i32.const 32)))
      (then (call $fail (i32.const 28))))
    (if (call $create_int32 (local.get $env) (i32.const 98) (i32.add (local.get $f) (i32.const 36)))
      (then (call $fail (i32.const 29))))
    (global.set $sp (local.get $f))
    (i32.load offset=20 (local.get $f)))

  ;; stale(): 1 if the id leak() minted in a scope it left open no longer
  ;; resolves.
  (func $stale (param $env i32) (param $info i32) (result i32)
    (local $s i32)
    (local.set $s (call $get_int32 (local.get $env) (global.get $stale_id) (i32.const 200)))
    (if (call $create_int32 (local.get $env) (i32.ne (local.get $s) (i32.const 0)) (i32.const 204))
      (then (call $fail (i32.const 41))))
    (i32.load (i32.const 204)))

  ;; Sums the first min(actual, want) int arguments, asking for `want`.
  (func $sum_args (param $env i32) (param $info i32) (param $want i32) (result i32)
    (local $f i32) (local $i i32) (local $sum i32) (local $n i32)
    (local.set $f (global.get $sp))
    (global.set $sp (i32.add (global.get $sp) (i32.const 256)))
    (i32.store (local.get $f) (local.get $want))
    (if (call $get_cb_info (local.get $env) (local.get $info) (local.get $f)
          (i32.add (local.get $f) (i32.const 4)) (i32.add (local.get $f) (i32.const 200)) (i32.const 0))
      (then (call $fail (i32.const 31))))
    (local.set $n (i32.load (local.get $f)))
    (if (i32.gt_u (local.get $n) (local.get $want)) (then (local.set $n (local.get $want))))
    (block $done
      (loop $next
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (if (call $get_int32 (local.get $env)
              (i32.load offset=4 (i32.add (local.get $f) (i32.shl (local.get $i) (i32.const 2))))
              (i32.add (local.get $f) (i32.const 204)))
          (then (call $fail (i32.const 33))))
        (local.set $sum (i32.add (local.get $sum) (i32.load offset=204 (local.get $f))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $next)))
    (if (call $create_int32 (local.get $env) (local.get $sum) (i32.add (local.get $f) (i32.const 208)))
      (then (call $fail (i32.const 34))))
    (global.set $sp (local.get $f))
    (i32.load offset=208 (local.get $f)))
  (func $many20 (param $env i32) (param $info i32) (result i32)
    (call $sum_args (local.get $env) (local.get $info) (i32.const 20)))
  (func $many16 (param $env i32) (param $info i32) (result i32)
    (call $sum_args (local.get $env) (local.get $info) (i32.const 16)))

  ;; probe(): napi_get_cb_info with every out pointer null, then all set;
  ;; returns the argument count.
  (func $probe (param $env i32) (param $info i32) (result i32)
    (if (call $get_cb_info (local.get $env) (local.get $info) (i32.const 0) (i32.const 0)
          (i32.const 0) (i32.const 0))
      (then (call $fail (i32.const 51))))
    (i32.store (i32.const 212) (i32.const 1))
    (if (call $get_cb_info (local.get $env) (local.get $info) (i32.const 212) (i32.const 216)
          (i32.const 220) (i32.const 224))
      (then (call $fail (i32.const 52))))
    (if (call $create_int32 (local.get $env) (i32.load (i32.const 212)) (i32.const 228))
      (then (call $fail (i32.const 53))))
    (i32.load (i32.const 228)))

  ;; undef(x): asks for 4 arguments into slots prefilled with -1. Returns
  ;; argc * 100 + the napi_valuetype of argv[1] * 10 + that of argv[3], or
  ;; -1 if either slot is not a valid value.
  (func $undef (param $env i32) (param $info i32) (result i32)
    (local $t1 i32) (local $t3 i32)
    (i32.store (i32.const 400) (i32.const 4))
    (i32.store (i32.const 404) (i32.const -1))
    (i32.store (i32.const 408) (i32.const -1))
    (i32.store (i32.const 412) (i32.const -1))
    (i32.store (i32.const 416) (i32.const -1))
    (if (call $get_cb_info (local.get $env) (local.get $info) (i32.const 400) (i32.const 404)
          (i32.const 0) (i32.const 0))
      (then (call $fail (i32.const 81))))
    (local.set $t1 (i32.const -1))
    (local.set $t3 (i32.const -1))
    (if (i32.eqz (call $typeof (local.get $env) (i32.load (i32.const 408)) (i32.const 420)))
      (then (local.set $t1 (i32.load (i32.const 420)))))
    (if (i32.eqz (call $typeof (local.get $env) (i32.load (i32.const 416)) (i32.const 420)))
      (then (local.set $t3 (i32.load (i32.const 420)))))
    (if (call $create_int32 (local.get $env)
          (select
            (i32.add (i32.mul (i32.load (i32.const 400)) (i32.const 100))
              (i32.add (i32.mul (local.get $t1) (i32.const 10)) (local.get $t3)))
            (i32.const -1)
            (i32.and (i32.ge_s (local.get $t1) (i32.const 0)) (i32.ge_s (local.get $t3) (i32.const 0))))
          (i32.const 424))
      (then (call $fail (i32.const 82))))
    (i32.load (i32.const 424)))

  ;; badret(): fills a scope it leaves open with enough handles to need new
  ;; handle blocks, then returns a value created in it.
  (func $badret (param $env i32) (param $info i32) (result i32)
    (local $i i32)
    (if (call $open_scope (local.get $env) (i32.const 300)) (then (call $fail (i32.const 71))))
    (block $done
      (loop $next
        (br_if $done (i32.ge_u (local.get $i) (i32.const 3000)))
        (drop (call $create_int32 (local.get $env) (local.get $i) (i32.const 304)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $next)))
    (if (call $create_int32 (local.get $env) (i32.const 1234) (i32.const 304))
      (then (call $fail (i32.const 72))))
    (i32.load (i32.const 304)))
  (elem (i32.const 1) $rec $leak $many20 $stale $probe $many16 $undef $badret)
)"#;

struct Guest {
    store: Store,
    instance: Instance,
    memory: Memory,
}

impl Guest {
    fn new(ctx: &NapiCtx, engine: &Engine, module: &Module) -> Self {
        let mut store = Store::new(engine.clone());
        let hooks = ctx.runtime_hooks();
        let (imports, state): (Imports, _) = hooks
            .additional_imports(module, &mut store.as_store_mut())
            .expect("failed to create N-API imports");
        let instance = Instance::new(&mut store, module, &imports).expect("instantiate");
        let memory = instance.exports.get_memory("memory").unwrap().clone();
        hooks
            .configure_instance(
                module,
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

    /// Runs `source`; returns its status, its int32 result and the first
    /// failing step a callback recorded (0 if none).
    fn eval(&mut self, source: &str) -> (i32, i32, i32) {
        self.memory
            .view(&self.store)
            .write(u64::from(SOURCE_PTR), source.as_bytes())
            .unwrap();
        let eval: TypedFunction<(i32, i32), i32> = self.typed("eval");
        let status = eval
            .call(&mut self.store, SOURCE_PTR as i32, source.len() as i32)
            .expect("eval trapped");
        let result: TypedFunction<(), i32> = self.typed("result_int");
        let value = if status == 0 {
            result.call(&mut self.store).unwrap()
        } else {
            -777
        };
        let err: TypedFunction<(), i32> = self.typed("err");
        (status, value, err.call(&mut self.store).unwrap())
    }

    fn release(&mut self) -> i32 {
        let release: TypedFunction<(), i32> = self.typed("release");
        release.call(&mut self.store).expect("release trapped")
    }
}

fn setup() -> (NapiCtx, Engine, Module) {
    let store = Store::default();
    let module = Module::new(&store, wat::parse_str(WAT).unwrap()).unwrap();
    (NapiCtx::default(), store.engine().clone(), module)
}

/// Returns 300 if every round passed, or a negative code for the first
/// failed check.
const ROUNDS: &str = r#"(() => {
  function step(m) { return rec(m, step); }
  function step2(m) { leak(m); stale(); return rec(m, step2); }
  let ok = 0;
  for (let i = 0; i < 300; i++) {
    if (rec(40, step) !== 820) return -1;
    if (rec(30, step2) !== 465) return -11;
    if (leak(i) !== 2 * i) return -2;
    if (stale() !== 1) return -3;
    if (many20(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20) !== 210) return -4;
    if (many20(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17) !== 153) return -5;
    if (many20(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16) !== 136) return -6;
    if (many16(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16) !== 136) return -7;
    if (many16(1,2,3) !== 6) return -8;
    if (many16(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18) !== 136) return -9;
    if (probe(5, 6) !== 2) return -10;
    ok++;
  }
  return ok;
})()"#;

/// Callbacks nested 40 deep through JavaScript keep their own callback info
/// and value ids; scopes a callback leaves open are closed and their ids stop
/// resolving; argument counts on both sides of the 16-entry inline buffers
/// arrive intact; null out pointers are accepted.
#[test]
fn nested_callbacks_scopes_and_arguments() {
    let (ctx, engine, module) = setup();
    let mut guest = Guest::new(&ctx, &engine, &module);
    assert_eq!(guest.eval(ROUNDS), (0, 300, 0));
    assert_eq!(guest.release(), 0);
}

/// Asking for more arguments than were passed reports the actual count and
/// fills the extra slots with undefined (`napi_undefined` is 0).
#[test]
fn missing_arguments_are_undefined() {
    let (ctx, engine, module) = setup();
    let mut guest = Guest::new(&ctx, &engine, &module);
    assert_eq!(guest.eval("undef(1)"), (0, 100, 0));
    assert_eq!(guest.eval("undef()"), (0, 0, 0));
    assert_eq!(guest.release(), 0);
}

/// A callback returns a value created in a scope it left open, after enough
/// values to fill more than one handle block.
#[test]
fn a_return_value_from_a_scope_left_open_survives() {
    let (ctx, engine, module) = setup();
    let mut guest = Guest::new(&ctx, &engine, &module);
    let script = "(() => { let s = 0; for (let i = 0; i < 200; i++) s += badret(); return s; })()";
    assert_eq!(guest.eval(script), (0, 200 * 1234, 0));
    assert_eq!(guest.release(), 0);
}

/// Envs created and released one after another on one thread: each new env
/// works, and a released one is refused.
#[test]
fn envs_created_one_after_another_on_one_thread() {
    let (ctx, engine, module) = setup();
    for round in 0..6 {
        let mut guest = Guest::new(&ctx, &engine, &module);
        assert_eq!(guest.eval(ROUNDS), (0, 300, 0), "round {round}");
        assert_eq!(guest.release(), 0);
        // `eval` traps when the bridge refuses the released env.
        let eval: TypedFunction<(i32, i32), i32> = guest.typed("eval");
        let after = eval.call(&mut guest.store, SOURCE_PTR as i32, 1);
        assert!(after.is_err(), "released env still ran: {after:?}");
    }
}
