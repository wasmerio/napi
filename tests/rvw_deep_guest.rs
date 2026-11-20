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

use wasmer::{AsStoreMut, Function, Imports, Instance, Memory, Module, Store, TypedFunction};
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
  (import "env" "probe" (func $probe (result i64)))
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
  (func $eval_int (export "eval_int") (param $ptr i32) (param $len i32) (result i32)
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
  (global $sp (mut i64) (i64.const 0))
  (func (export "last_sp") (result i64) (global.get $sp))
  (func $deep (export "deep") (param $n i32) (param $ptr i32) (param $len i32) (result i32)
    (if (result i32) (i32.eqz (local.get $n))
      (then
        (global.set $sp (call $probe))
        (call $eval_int (local.get $ptr) (local.get $len)))
      (else
        (i32.add (i32.const 1)
          (i32.sub
            (call $deep (i32.sub (local.get $n) (i32.const 1)) (local.get $ptr) (local.get $len))
            (i32.const 1))))))
)"#;

struct Guest {
    store: Store,
    instance: Instance,
    memory: Memory,
}

#[inline(never)]
fn probe() -> i64 {
    let x = 0u8;
    std::hint::black_box(&x) as *const u8 as i64
}

impl Guest {
    fn new() -> Self {
        let mut store = Store::default();
        let module = Module::new(&store, wat::parse_str(WAT).unwrap()).unwrap();
        let ctx = NapiCtx::default();
        let hooks = ctx.runtime_hooks();
        let (mut imports, state): (Imports, _) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .expect("failed to create N-API imports");
        imports.define("env", "probe", Function::new_typed(&mut store, probe));
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

    fn deep(&mut self, n: i32, source: &str) -> Result<(i32, i64), String> {
        self.memory
            .view(&self.store)
            .write(u64::from(SOURCE_PTR), source.as_bytes())
            .unwrap();
        let deep: TypedFunction<(i32, i32, i32), i32> = self
            .instance
            .exports
            .get_typed_function(&self.store, "deep")
            .unwrap();
        let last_sp: TypedFunction<(), i64> = self
            .instance
            .exports
            .get_typed_function(&self.store, "last_sp")
            .unwrap();
        let r = deep
            .call(&mut self.store, n, SOURCE_PTR as i32, source.len() as i32)
            .map_err(|e| e.to_string())?;
        let sp = last_sp.call(&mut self.store).unwrap();
        Ok((r, sp))
    }
}

fn env_usize(name: &str, default: usize) -> usize {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

/// RVW_DEPTH_KIB: how deep (KiB of native stack) the guest recurses before it
/// runs a script. RVW_STACK_MIB: stack of the thread it all runs on.
/// RVW_MUTUAL=1: instead, JS <-> guest mutual recursion until it stops.
#[test]
#[ignore]
fn deep_guest_then_javascript() {
    let stack = env_usize("RVW_STACK_MIB", 16) << 20;
    std::thread::Builder::new()
        .stack_size(stack)
        .spawn(|| {
            let mut guest = Guest::new();
            let top = probe();
            println!("RVW host probe at {top:#x}");
            if env_usize("RVW_MUTUAL", 0) == 1 {
                let r = guest.deep(
                    0,
                    "globalThis.d = 0; globalThis.m = 0; \
                     globalThis.thrower = function () { d++; if (d > m) m = d; const r = h(); d--; return r; }; \
                     let o; try { o = h(); } catch (e) { o = -9; } m",
                );
                println!("RVW mutual max depth result {r:?}");
                let r = guest.deep(0, "globalThis.thrower = function () { return 5; }; h()");
                println!("RVW after mutual {r:?}");
                return;
            }
            // Bytes of native stack per guest frame.
            let (_, sp0) = guest.deep(0, "1").unwrap();
            let (_, sp1) = guest.deep(1000, "1").unwrap();
            let per_frame = ((sp0 - sp1) / 1000) as usize;
            println!("RVW per frame {per_frame} bytes; entry is {} bytes below the host probe (other stack if huge)", top - sp0);
            let kib = env_usize("RVW_DEPTH_KIB", 0);
            let n = (kib << 10) / per_frame.max(1);
            println!("RVW depth {kib} KiB = {n} frames");
            let r = guest.deep(n as i32, "(function f(k) { return k ? 1 + f(k - 1) : 0; })(50)");
            println!("RVW deep result {r:?}");
            if let Ok((_, sp)) = r {
                println!("RVW bottom is {} KiB below the entry", (sp0 - sp) >> 10);
            }
            // The guest goes on afterwards?
            let r = guest.deep(0, "40 + 2");
            println!("RVW after {r:?}");
        })
        .unwrap()
        .join()
        .unwrap();
}
