//! A minimal WAT guest that drives the N-API imports through real Wasmer
//! dispatch, for tests that need several guests (one Wasmer store each) on
//! several OS threads at once. No WASIX C toolchain is needed.
//!
//! Each [`Guest`] owns one store, one instance and at most one N-API
//! environment, created on the thread that calls [`Guest::create`]. Scripts
//! can call `signal()`, which reaches the host through the `test.signal`
//! import, so a test knows that a guest is executing JavaScript right now.

#![allow(dead_code)]

use std::sync::mpsc::Sender;

use wasmer::{
    AsStoreMut, Engine, Function, Instance, Memory, Module, Store, TypedFunction, WasmTypeList,
};
use wasmer_napi::NapiCtx;

/// `napi_invalid_arg`.
pub const NAPI_INVALID_ARG: i32 = 1;
/// `napi_pending_exception`, which is what a terminated script reports.
pub const NAPI_PENDING_EXCEPTION: i32 = 10;
/// The `data` argument passed by the guest's `interrupt` export.
pub const INTERRUPT_DATA: i32 = 777;

/// Where [`Guest::eval`] and friends place script text in guest memory.
const SOURCE_PTR: u32 = 4096;

const WAT: &str = r#"(module
  (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
    (func $configure (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
    (func $create_env (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
    (func $release_env (param i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_contextify_run_script"
    (func $contextify_run_script
      (param i32 i32 i32 i32 i32 i32 i64 i32 i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_event_loop_checkpoint"
    (func $checkpoint (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_message_create"
    (func $message_create (param i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_message_take"
    (func $message_take (param i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_message_drop"
    (func $message_drop (param i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_terminate_execution"
    (func $terminate (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_cancel_terminate_execution"
    (func $cancel (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_request_interrupt"
    (func $request_interrupt (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_string_utf8"
    (func $create_string (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_run_script" (func $run_script (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_int32" (func $get_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_undefined" (func $get_undefined (param i32 i32) (result i32)))
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_named_property (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_function"
    (func $create_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_object" (func $create_object (param i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_module_wrap_create"
    (func $module_create (param i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_module_wrap_link"
    (func $module_link (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_module_wrap_instantiate"
    (func $module_instantiate (param i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_module_wrap_evaluate"
    (func $module_evaluate (param i32 i32 i64 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_module_wrap_destroy"
    (func $module_destroy (param i32 i32) (result i32)))
  (import "test" "signal" (func $host_signal))

  ;; Out-slots: 8 env id, 12 scope id, 16..28 js source, 28 string,
  ;; 32 script result, 36 message token, 40 taken value, 44 int32,
  ;; 48 global, 52 function, 56 filename, 60 checkpoint state,
  ;; 128..168 module create options, 168..177 module create result.
  (memory (export "memory") 2)
  (table (export "__indirect_function_table") 3 funcref)
  (global $env (mut i32) (i32.const 0))
  (global $scope (mut i32) (i32.const 0))
  (global $interrupted (mut i32) (i32.const 0))
  (data (i32.const 100) "shared\00")
  (data (i32.const 108) "signal\00")
  (data (i32.const 116) "test.js\00")
  (data (i32.const 180) "test.mjs\00")

  (func $string (param $ptr i32) (param $len i32) (result i32)
    (if (call $create_string (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28))
      (then unreachable))
    (i32.load (i32.const 28)))

  (func (export "create") (result i32)
    (if (call $configure (i32.const 0)) (then unreachable))
    (if (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12))
      (then unreachable))
    (global.set $env (i32.load (i32.const 8)))
    (global.set $scope (i32.load (i32.const 12)))
    ;; globalThis.signal = <guest function calling test.signal>
    (if (call $create_function (global.get $env) (i32.const 108) (i32.const 6)
          (i32.const 2) (i32.const 0) (i32.const 52))
      (then unreachable))
    (if (call $get_global (global.get $env) (i32.const 48)) (then unreachable))
    (if (call $set_named_property (global.get $env) (i32.load (i32.const 48))
          (i32.const 108) (i32.load (i32.const 52)))
      (then unreachable))
    (global.get $env))

  (func (export "scope") (result i32) (global.get $scope))
  (func (export "release") (result i32)
    (call $release_env (global.get $scope) (i32.const 0)))
  (func (export "release_scope") (param $scope i32) (result i32)
    (call $release_env (local.get $scope) (i32.const 0)))

  (func (export "eval") (param $ptr i32) (param $len i32) (result i32)
    (call $run_script (global.get $env) (call $string (local.get $ptr) (local.get $len))
      (i32.const 32)))
  (func (export "eval_contextify") (param $ptr i32) (param $len i32) (result i32)
    (i32.store (i32.const 16) (i32.const 0))
    (i32.store (i32.const 20) (call $string (local.get $ptr) (local.get $len)))
    (i32.store (i32.const 24) (i32.const 0))
    (i32.store (i32.const 56) (call $string (i32.const 116) (i32.const 7)))
    (call $contextify_run_script (global.get $env) (i32.const 0) (i32.const 16)
      (i32.load (i32.const 56)) (i32.const 0) (i32.const 0) (i64.const -1)
      (i32.const 1) (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 32)))
  ;; Creates, links, instantiates, evaluates and destroys a source text
  ;; module. Returns 0, or 1000 * step + status for the first failing step.
  (func (export "eval_module") (param $ptr i32) (param $len i32) (result i32)
    (local $status i32) (local $module i32)
    (i32.store (i32.const 16) (i32.const 0))
    (i32.store (i32.const 20) (call $string (local.get $ptr) (local.get $len)))
    (i32.store (i32.const 24) (i32.const 0))
    (if (call $create_object (global.get $env) (i32.const 52)) (then unreachable))
    (i32.store (i32.const 128) (i32.const 40))
    (i32.store (i32.const 132) (i32.const 1))
    (i32.store (i32.const 136) (i32.const 1))
    (i32.store (i32.const 140) (i32.load (i32.const 52)))
    (i32.store (i32.const 144) (call $string (i32.const 180) (i32.const 8)))
    (i32.store (i32.const 148) (i32.const 0))
    (i32.store (i32.const 152) (i32.const 16))
    (i32.store (i32.const 156) (i32.const 0))
    (i32.store (i32.const 160) (i32.const 0))
    (i32.store (i32.const 164) (i32.const 0))
    (local.set $status (call $module_create (global.get $env) (i32.const 128) (i32.const 168)))
    (if (local.get $status) (then (return (i32.add (i32.const 1000) (local.get $status)))))
    (local.set $module (i32.load (i32.const 168)))
    (local.set $status
      (call $module_link (global.get $env) (local.get $module) (i32.const 0) (i32.const 0)))
    (if (local.get $status) (then (return (i32.add (i32.const 2000) (local.get $status)))))
    (local.set $status (call $module_instantiate (global.get $env) (local.get $module)))
    (if (local.get $status) (then (return (i32.add (i32.const 3000) (local.get $status)))))
    (local.set $status
      (call $module_evaluate (global.get $env) (local.get $module) (i64.const -1)
        (i32.const 0) (i32.const 32)))
    (if (local.get $status) (then (return (i32.add (i32.const 4000) (local.get $status)))))
    (local.set $status (call $module_destroy (global.get $env) (local.get $module)))
    (if (local.get $status) (then (return (i32.add (i32.const 5000) (local.get $status)))))
    (i32.const 0))
  (func (export "checkpoint") (result i32)
    (call $checkpoint (global.get $env) (i32.const 0) (i32.const 0) (i32.const 60)))
  (func (export "integer") (result i32)
    (if (call $get_int32 (global.get $env) (i32.load (i32.const 32)) (i32.const 44))
      (then unreachable))
    (i32.load (i32.const 44)))

  (func (export "message") (result i32)
    (call $message_create (global.get $env) (i32.load (i32.const 32)) (i32.const 36)))
  (func (export "token") (result i32) (i32.load (i32.const 36)))
  (func (export "take") (param $token i32) (result i32)
    (call $message_take (global.get $env) (local.get $token) (i32.const 40)))
  (func (export "install") (result i32)
    (if (call $get_global (global.get $env) (i32.const 48)) (then unreachable))
    (call $set_named_property (global.get $env) (i32.load (i32.const 48))
      (i32.const 100) (i32.load (i32.const 40))))
  (func (export "drop") (param $token i32) (call $message_drop (local.get $token)))

  (func (export "undefined_in") (param $env i32) (result i32)
    (call $get_undefined (local.get $env) (i32.const 40)))
  (func (export "terminate") (param $env i32) (result i32)
    (call $terminate (local.get $env)))
  (func (export "cancel") (param $env i32) (result i32) (call $cancel (local.get $env)))
  (func (export "interrupt") (param $env i32) (result i32)
    (call $request_interrupt (local.get $env) (i32.const 1) (i32.const 777)))
  (func (export "interrupted") (result i32) (global.get $interrupted))

  ;; Interrupt callback: record `data` if it runs for this guest's own env.
  (func $on_interrupt (param $env i32) (param $data i32) (result i32)
    (global.set $interrupted
      (select (local.get $data) (i32.const -1)
        (i32.eq (local.get $env) (global.get $env))))
    (i32.const 0))
  ;; globalThis.signal(): tell the host this guest is inside JavaScript.
  (func $on_signal (param $env i32) (param $info i32) (result i32)
    (call $host_signal)
    (i32.const 0))
  (elem (i32.const 1) $on_interrupt $on_signal)
)"#;

/// Compiles the guest once; every [`Guest`] instantiates it in its own store.
pub fn compile() -> (Engine, Module) {
    let store = Store::default();
    let module = Module::new(&store, wat::parse_str(WAT).expect("invalid test WAT"))
        .expect("failed to compile the test guest");
    (store.engine().clone(), module)
}

pub struct Guest {
    store: Store,
    instance: Instance,
    memory: Memory,
}

impl Guest {
    /// Instantiates the guest in a new store. `signal` receives a message
    /// whenever a script running in this guest calls `signal()`.
    pub fn new(ctx: &NapiCtx, engine: &Engine, module: &Module, signal: Sender<()>) -> Self {
        let mut store = Store::new(engine.clone());
        let hooks = ctx.runtime_hooks();
        let (mut imports, state) = hooks
            .additional_imports(module, &mut store.as_store_mut())
            .expect("failed to create N-API imports");
        let signal = Function::new_typed(&mut store, move || {
            let _ = signal.send(());
        });
        imports.define("test", "signal", signal);
        let instance =
            Instance::new(&mut store, module, &imports).expect("failed to instantiate guest");
        let memory = instance
            .exports
            .get_memory("memory")
            .expect("guest exports no memory")
            .clone();
        hooks
            .configure_instance(
                module,
                &mut store.as_store_mut(),
                &instance,
                Some(&memory),
                state,
            )
            .expect("failed to configure the guest instance");
        Self {
            store,
            instance,
            memory,
        }
    }

    fn function<Args: WasmTypeList, Rets: WasmTypeList>(
        &self,
        name: &str,
    ) -> TypedFunction<Args, Rets> {
        self.instance
            .exports
            .get_typed_function(&self.store, name)
            .unwrap_or_else(|err| panic!("guest export {name}: {err}"))
    }

    fn call0(&mut self, name: &str) -> i32 {
        self.function::<(), i32>(name)
            .call(&mut self.store)
            .unwrap_or_else(|err| panic!("guest call {name} trapped: {err}"))
    }

    fn call1(&mut self, name: &str, arg: i32) -> i32 {
        self.function::<i32, i32>(name)
            .call(&mut self.store, arg)
            .unwrap_or_else(|err| panic!("guest call {name} trapped: {err}"))
    }

    fn call_with_source(&mut self, name: &str, source: &str) -> i32 {
        self.memory
            .view(&self.store)
            .write(u64::from(SOURCE_PTR), source.as_bytes())
            .expect("script does not fit in guest memory");
        self.function::<(i32, i32), i32>(name)
            .call(&mut self.store, SOURCE_PTR as i32, source.len() as i32)
            .unwrap_or_else(|err| panic!("guest call {name} trapped: {err}"))
    }

    /// Creates this guest's environment on the calling thread and returns its
    /// guest-visible id.
    pub fn create(&mut self) -> i32 {
        self.call0("create")
    }

    /// The owner scope id that [`Self::release_scope`] takes.
    pub fn scope(&mut self) -> i32 {
        self.call0("scope")
    }

    pub fn release(&mut self) -> i32 {
        self.call0("release")
    }

    pub fn release_scope(&mut self, scope: i32) -> i32 {
        self.call1("release_scope", scope)
    }

    /// `napi_run_script`; returns its status.
    pub fn eval(&mut self, source: &str) -> i32 {
        self.call_with_source("eval", source)
    }

    /// `unofficial_napi_contextify_run_script` in the main context; returns
    /// its status.
    pub fn eval_contextify(&mut self, source: &str) -> i32 {
        self.call_with_source("eval_contextify", source)
    }

    /// Creates, links, instantiates, evaluates and destroys a source text
    /// module. Returns 0, or `1000 * step + status` for the first failing
    /// step (1 create, 2 link, 3 instantiate, 4 evaluate, 5 destroy).
    pub fn eval_module(&mut self, source: &str) -> i32 {
        self.call_with_source("eval_module", source)
    }

    /// Drains microtasks; returns the checkpoint status.
    pub fn checkpoint(&mut self) -> i32 {
        self.call0("checkpoint")
    }

    /// Runs `source` with `napi_run_script` and returns its int32 result.
    pub fn number(&mut self, source: &str) -> i32 {
        assert_eq!(self.eval(source), 0, "script failed: {source}");
        self.integer()
    }

    /// The int32 value of the last script result.
    pub fn integer(&mut self) -> i32 {
        self.call0("integer")
    }

    /// Serializes the last script result into a message; returns the status.
    pub fn message(&mut self) -> i32 {
        self.call0("message")
    }

    /// The token written by the last successful [`Self::message`].
    pub fn token(&mut self) -> i32 {
        self.call0("token")
    }

    /// Deserializes a message into this guest's environment.
    pub fn take(&mut self, token: i32) -> i32 {
        self.call1("take", token)
    }

    /// Stores the last taken value as `globalThis.shared`.
    pub fn install(&mut self) -> i32 {
        self.call0("install")
    }

    pub fn drop_message(&mut self, token: i32) {
        self.function::<i32, ()>("drop")
            .call(&mut self.store, token)
            .unwrap_or_else(|err| panic!("guest call drop trapped: {err}"))
    }

    /// `napi_get_undefined` on an arbitrary env id; returns the status.
    pub fn undefined_in(&mut self, env: i32) -> i32 {
        self.call1("undefined_in", env)
    }

    pub fn terminate(&mut self, env: i32) -> i32 {
        self.call1("terminate", env)
    }

    pub fn cancel(&mut self, env: i32) -> i32 {
        self.call1("cancel", env)
    }

    /// Requests an interrupt on `env` whose callback records
    /// [`INTERRUPT_DATA`] in the guest that owns `env`.
    pub fn interrupt(&mut self, env: i32) -> i32 {
        self.call1("interrupt", env)
    }

    /// [`INTERRUPT_DATA`] once this guest's interrupt callback ran for its own
    /// env, -1 if it ran for another env, 0 if it never ran.
    pub fn interrupted(&mut self) -> i32 {
        self.call0("interrupted")
    }
}
