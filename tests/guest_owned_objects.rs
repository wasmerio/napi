//! Exercise the owned-object Wasmer imports and V8 bridge.
#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Instance, Memory, Module, Store, Value};
use wasmer_napi::NapiCtx;

const OK: i32 = 0;
const INVALID_ARG: i32 = 1;
const CANNOT_RUN_JS: i32 = 23;
const SCRIPT: u32 = 1024;
const STRING: u32 = 16;
const EVAL_RESULT: u32 = 20;
const VALUE: u32 = 24;
const WEAK: u32 = 28;
const STATE: u32 = 32;
const OUTPUT: u32 = 36;

const WAT: &str = r#"(module
  (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
    (func $configure (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
    (func $env (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
    (func $release (param i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_owned_object"
    (func $create (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_move_owned_object"
    (func $move (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_string_utf8"
    (func $string (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_run_script" (func $eval (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_reference" (func $reference (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_int32" (func $int (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_global" (func $global (param i32 i32) (result i32)))
  (import "napi" "napi_set_named_property" (func $set (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_unwrap" (func $unwrap (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_and_clear_last_exception"
    (func $clear_exception (param i32 i32) (result i32)))
  (memory (export "memory") 1)
  (table (export "__indirect_function_table") 2 funcref)
  (elem (i32.const 1) $finalize)
  (func $finalize (param i32 i32 i32))
  (data (i32.const 64) "wrapper\00")
  (export "configure" (func $configure))
  (export "env" (func $env))
  (export "release" (func $release))
  (export "create" (func $create))
  (export "move" (func $move))
  (export "string" (func $string))
  (export "eval" (func $eval))
  (export "reference" (func $reference))
  (export "int" (func $int))
  (export "global" (func $global))
  (export "set" (func $set))
  (export "unwrap" (func $unwrap))
  (export "clear_exception" (func $clear_exception)))"#;

struct Guest {
    store: Store,
    instance: Instance,
    memory: Memory,
    env: u32,
}

impl Guest {
    fn new() -> Self {
        let mut store = Store::default();
        let module = Module::new(&store, wat::parse_str(WAT).unwrap()).unwrap();
        let hooks = NapiCtx::default().runtime_hooks();
        let (imports, state) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .unwrap();
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        let memory = instance.exports.get_memory("memory").unwrap().clone();
        hooks
            .configure_instance(
                &module,
                &mut store.as_store_mut(),
                &instance,
                Some(&memory),
                state,
            )
            .unwrap();
        let mut guest = Self {
            store,
            instance,
            memory,
            env: 0,
        };
        assert_eq!(guest.call("configure", &[0]), OK);
        assert_eq!(guest.call("env", &[8, 0, 8, 12]), OK);
        guest.env = guest.word(8);
        guest
    }

    fn call(&mut self, name: &str, arguments: &[u32]) -> i32 {
        let function = self.instance.exports.get_function(name).unwrap().clone();
        let arguments: Vec<_> = arguments.iter().copied().map(Value::from).collect();
        function.call(&mut self.store, &arguments).expect(name)[0].unwrap_i32()
    }

    fn word(&self, ptr: u32) -> u32 {
        let mut bytes = [0; 4];
        self.memory
            .view(&self.store)
            .read(ptr.into(), &mut bytes)
            .unwrap();
        u32::from_le_bytes(bytes)
    }

    fn eval(&mut self, source: &str) -> u32 {
        self.memory
            .view(&self.store)
            .write(SCRIPT.into(), source.as_bytes())
            .unwrap();
        assert_eq!(
            self.call("string", &[self.env, SCRIPT, source.len() as u32, STRING]),
            OK
        );
        assert_eq!(
            self.call("eval", &[self.env, self.word(STRING), EVAL_RESULT]),
            OK
        );
        self.word(EVAL_RESULT)
    }

    fn reference(&mut self, source: &str) -> u32 {
        let value = self.eval(source);
        assert_eq!(self.call("reference", &[self.env, value, 1, OUTPUT]), OK);
        self.word(OUTPUT)
    }

    fn assert_js(&mut self, source: &str) {
        let value = self.eval(source);
        assert_eq!(self.call("int", &[self.env, value, OUTPUT]), OK);
        assert_eq!(self.word(OUTPUT), 1, "{source}");
    }
}

#[test]
fn creates_and_moves_wrappers_with_their_ownership_edges() {
    let mut guest = Guest::new();
    let env = guest.env;
    let source = guest.reference("globalThis.proto = { marker: 7 }");
    let from = guest.reference("globalThis.first = new Set()");
    let to = guest.reference("globalThis.second = new Set()");
    let map = guest.reference("globalThis.owners = new WeakMap()");
    let setter = guest.reference("WeakMap.prototype.set");
    let native = 71;
    let mut create = [
        env, source, native, 1, 0, 0, from, map, setter, 0, VALUE, WEAK,
    ];

    create[11] = VALUE;
    assert_eq!(guest.call("create", &create), INVALID_ARG);
    guest.assert_js("Number(first.size === 0)");

    create[11] = WEAK;
    assert_eq!(guest.call("create", &create), OK);
    let (value, weak) = (guest.word(VALUE), guest.word(WEAK));
    assert_ne!(value, 0);
    assert_ne!(weak, 0);
    assert_eq!(guest.call("unwrap", &[env, value, OUTPUT]), OK);
    assert_eq!(guest.word(OUTPUT), native);
    assert_eq!(guest.call("global", &[env, OUTPUT]), OK);
    assert_eq!(guest.call("set", &[env, guest.word(OUTPUT), 64, value]), OK);
    guest.assert_js("Number(Object.getPrototypeOf(wrapper) === proto && first.has(wrapper) && owners.get(wrapper) === first)");

    assert_eq!(
        guest.call("move", &[env, weak, from, 0, to, map, setter, STATE]),
        OK
    );
    assert_eq!(guest.word(STATE), 0);
    guest.assert_js(
        "Number(!first.has(wrapper) && second.has(wrapper) && owners.get(wrapper) === second)",
    );

    let throws = guest.reference("(() => { throw new Error('ownership edge failed'); })");
    create[8] = throws;
    assert_eq!(guest.call("create", &create), CANNOT_RUN_JS);
    let failed_value = guest.word(VALUE);
    assert_ne!(failed_value, 0, "the wrapper owns the native data already");
    assert_ne!(guest.word(WEAK), 0);
    assert_eq!(guest.call("clear_exception", &[env, OUTPUT]), OK);
    assert_eq!(guest.call("unwrap", &[env, failed_value, OUTPUT]), OK);
    assert_eq!(guest.word(OUTPUT), native);
    assert_eq!(guest.call("release", &[guest.word(12), 0]), OK);
}
