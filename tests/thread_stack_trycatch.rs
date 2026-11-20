//! fast-wasmer prototype: an exception thrown by JavaScript that a guest
//! callback called through `napi_call_function` must be caught by that
//! import's `v8::TryCatch`, although a JavaScript frame (the one that called
//! the guest callback) lies between it and the outer script. V8 decides who
//! catches by comparing stack addresses, so this only holds when the nested
//! activations lie at descending addresses: on the native stack, or with the
//! guest on the thread's own stack (`WASMER_WASM_ON_THREAD_STACK=1`). It was
//! what broke `util.types.isRegExp({})` with imports in place on Wasm
//! coroutine stacks.

#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Imports, Instance, Memory, Module, Store, TypedFunction};
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
  (import "napi" "napi_get_undefined" (func $get_undefined (param i32 i32) (result i32)))
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_named_property (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_named_property"
    (func $get_named (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_function"
    (func $create_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_call_function"
    (func $call_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_is_exception_pending" (func $pending (param i32 i32) (result i32)))
  (import "napi" "napi_get_and_clear_last_exception"
    (func $clear (param i32 i32) (result i32)))

  (memory (export "memory") 2)
  (table (export "__indirect_function_table") 4 funcref)
  (global $env (mut i32) (i32.const 0))
  (global $scope (mut i32) (i32.const 0))
  (global $status (mut i32) (i32.const 0))
  (data (i32.const 100) "h\00")
  (data (i32.const 112) "thrower\00")

  (func (export "create")
    (if (call $configure (i32.const 0)) (then unreachable))
    (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
      (then unreachable))
    (global.set $env (i32.load (i32.const 8)))
    (global.set $scope (i32.load (i32.const 12)))
    (if (call $create_function (global.get $env) (i32.const 100) (i32.const 1)
          (i32.const 1) (i32.const 0) (i32.const 52))
      (then unreachable))
    (if (call $get_global (global.get $env) (i32.const 48)) (then unreachable))
    (if (call $set_named_property (global.get $env) (i32.load (i32.const 48))
          (i32.const 100) (i32.load (i32.const 52)))
      (then unreachable)))

  (func (export "release") (result i32)
    (call $release_env (global.get $scope) (i32.const 0)))

  ;; Runs the script and returns its result as an int32, or -1 - status.
  (func (export "eval_int") (param $ptr i32) (param $len i32) (result i32)
    (local $s i32)
    (if (call $create_string (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28))
      (then unreachable))
    (local.set $s (call $run_script (global.get $env) (i32.load (i32.const 28)) (i32.const 32)))
    (if (local.get $s) (then (return (i32.sub (i32.const -1) (local.get $s)))))
    (if (call $get_int32 (global.get $env) (i32.load (i32.const 32)) (i32.const 72))
      (then (return (i32.const -1000))))
    (i32.load (i32.const 72)))

  ;; h(): calls globalThis.thrower(), which throws, and returns
  ;; 100 * (status of napi_call_function) + (whether an exception is pending
  ;; right after), having cleared the exception.
  (func $h (param $env i32) (param $info i32) (result i32)
    (if (call $get_global (local.get $env) (i32.const 48)) (then unreachable))
    (if (call $get_named (local.get $env) (i32.load (i32.const 48)) (i32.const 112)
          (i32.const 120))
      (then unreachable))
    (if (call $get_undefined (local.get $env) (i32.const 88)) (then unreachable))
    (global.set $status
      (call $call_function (local.get $env) (i32.load (i32.const 88))
        (i32.load (i32.const 120)) (i32.const 0) (i32.const 0) (i32.const 84)))
    (i32.store (i32.const 124) (i32.const 0))
    (if (call $pending (local.get $env) (i32.const 124)) (then unreachable))
    (if (i32.load8_u (i32.const 124))
      (then (if (call $clear (local.get $env) (i32.const 128)) (then unreachable))))
    (if (call $create_int32 (local.get $env)
          (i32.add (i32.mul (global.get $status) (i32.const 100))
                   (i32.load8_u (i32.const 124)))
          (i32.const 56))
      (then unreachable))
    (i32.load (i32.const 56)))
  (elem (i32.const 1) $h)
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
        let create: TypedFunction<(), ()> =
            instance.exports.get_typed_function(&store, "create").unwrap();
        create.call(&mut store).expect("create trapped");
        Self {
            store,
            instance,
            memory,
        }
    }

    fn eval_int(&mut self, source: &str) -> i32 {
        self.memory
            .view(&self.store)
            .write(u64::from(SOURCE_PTR), source.as_bytes())
            .unwrap();
        let eval: TypedFunction<(i32, i32), i32> = self
            .instance
            .exports
            .get_typed_function(&self.store, "eval_int")
            .unwrap();
        eval.call(&mut self.store, SOURCE_PTR as i32, source.len() as i32)
            .expect("eval trapped")
    }
}

#[test]
fn a_nested_import_catches_the_exception_of_the_function_it_called() {
    const NAPI_PENDING_EXCEPTION: i32 = 10;
    let mut guest = Guest::new();
    assert_eq!(
        guest.eval_int("globalThis.thrower = function () { throw new Error('boom'); }; 1"),
        1
    );
    // The import reports the exception to the guest callback, which clears
    // it: the outer script goes on.
    for _ in 0..3 {
        assert_eq!(guest.eval_int("h()"), NAPI_PENDING_EXCEPTION * 100 + 1);
    }
    // In a JavaScript try block too, whose handler must not see it.
    assert_eq!(
        guest.eval_int("(() => { try { return h(); } catch (e) { return -7; } })()"),
        NAPI_PENDING_EXCEPTION * 100 + 1
    );
    // One level deeper: JS -> h -> JS thrower2 -> h -> JS thrower (throws),
    // where the middle JavaScript frame has a handler of its own.
    assert_eq!(
        guest.eval_int(
            "globalThis.inner = thrower; \
             globalThis.thrower = function () { \
               globalThis.thrower = inner; \
               let r; try { r = h(); } catch (e) { r = -7; } \
               globalThis.thrower = arguments.callee; \
               if (r !== 1001) throw new Error('inner ' + r); \
               throw new Error('outer'); \
             }; h()"
        ),
        NAPI_PENDING_EXCEPTION * 100 + 1
    );
    // JavaScript that overflows its stack still gets a RangeError: V8's
    // stack limit holds where the guest and its imports run.
    assert_eq!(
        guest.eval_int(
            "globalThis.thrower = inner; \
             (function r(n) { try { return r(n + 1); } \
               catch (e) { if (!(e instanceof RangeError)) return -5; throw e; } })(0)"
        ),
        -1 - NAPI_PENDING_EXCEPTION
    );
    // And the guest goes on afterwards.
    assert_eq!(guest.eval_int("h()"), NAPI_PENDING_EXCEPTION * 100 + 1);
    let release: TypedFunction<(), i32> = guest
        .instance
        .exports
        .get_typed_function(&guest.store, "release")
        .unwrap();
    assert_eq!(release.call(&mut guest.store).unwrap(), 0);
}
