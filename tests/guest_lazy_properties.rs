//! Lazy data properties (`unofficial_napi_define_lazy_data_properties` and
//! `unofficial_napi_is_lazy_data_property`) through real Wasmer dispatch, the
//! bridge and V8: what a property looks like before its first read, its key
//! order before and after, one initializer call, a throwing initializer, one
//! initializer per context, assignment, deletion and redefinition before the
//! first read, reads through a prototype, and the argument checks, from
//! sloppy and strict callers. The
//! provider side is also covered, through the C API, by
//! `tests/runners/test_75_unofficial_lazy_data_properties.cc`.
//!
//! The guest puts `defineLazy(object, names, initializer, attributes)`,
//! `isLazy(object, key)`, `makeContext()` and `runIn(global, source)` on the
//! global; each script below returns "ok" or what went wrong.

#![cfg(not(target_arch = "wasm32"))]

use wasmer::{AsStoreMut, Imports, Instance, Memory, Module, Store, TypedFunction};
use wasmer_napi::NapiCtx;

const SOURCE_PTR: u32 = 65536;
const RESULT_PTR: u32 = 131072;
const RESULT_SIZE: u32 = 65536;

const WAT: &str = r#"(module
  (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
    (func $configure (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
    (func $create_env (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
    (func $release_env (param i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_define_lazy_data_properties"
    (func $define_lazy (param i32 i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_is_lazy_data_property"
    (func $is_lazy (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_contextify_make_context"
    (func $make_context (param i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_contextify_run_script"
    (func $contextify_run_script
      (param i32 i32 i32 i32 i32 i32 i64 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_string_utf8"
    (func $create_string (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_run_script" (func $run_script (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_int32" (func $get_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_int32" (func $create_int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_create_object" (func $create_object (param i32 i32) (result i32)))
  (import "napi" "napi_get_global" (func $get_global (param i32 i32) (result i32)))
  (import "napi" "napi_set_named_property"
    (func $set_named_property (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_create_function"
    (func $create_function (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_cb_info"
    (func $get_cb_info (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_string_utf8"
    (func $get_utf8 (param i32 i32 i32 i32 i32) (result i32)))

  ;; 8 env, 12 scope, 28 string, 32 script result, 36 length out, 48 global,
  ;; 52 function. Callbacks take 64-byte frames from a stack at 32768 (an
  ;; initializer can call back in while another callback runs).
  (memory (export "memory") 4)
  (table (export "__indirect_function_table") 8 funcref)
  (global $env (mut i32) (i32.const 0))
  (global $scope (mut i32) (i32.const 0))
  (global $sp (mut i32) (i32.const 32768))
  (data (i32.const 100) "defineLazy\00")
  (data (i32.const 112) "isLazy\00")
  (data (i32.const 120) "makeContext\00")
  (data (i32.const 132) "runIn\00")
  (data (i32.const 140) "context\00")
  (data (i32.const 148) "lazy.js\00")

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
    (call $define (i32.const 112) (i32.const 2))
    (call $define (i32.const 120) (i32.const 3))
    (call $define (i32.const 132) (i32.const 4)))

  (func (export "release") (result i32)
    (call $release_env (global.get $scope) (i32.const 0)))

  (func (export "eval") (param $ptr i32) (param $len i32) (result i32)
    (if (call $create_string (global.get $env) (local.get $ptr) (local.get $len) (i32.const 28))
      (then unreachable))
    (call $run_script (global.get $env) (i32.load (i32.const 28)) (i32.const 32)))

  ;; The last script result, a string, as UTF-8 into $buf; its length at 36.
  (func (export "read") (param $buf i32) (param $size i32) (result i32)
    (call $get_utf8 (global.get $env) (i32.load (i32.const 32)) (local.get $buf)
      (local.get $size) (i32.const 36)))
  (func (export "read_length") (result i32) (i32.load (i32.const 36)))

  (func $push (result i32) (local $f i32)
    (local.set $f (global.get $sp))
    (global.set $sp (i32.add (global.get $sp) (i32.const 64)))
    (local.get $f))

  ;; defineLazy(object, names, initializer, attributes): the status.
  (func $define_lazy_cb (param $env i32) (param $info i32) (result i32)
    (local $f i32)
    (local.set $f (call $push))
    (i32.store (local.get $f) (i32.const 4))
    (if (call $get_cb_info (local.get $env) (local.get $info) (local.get $f)
          (i32.add (local.get $f) (i32.const 4)) (i32.const 0) (i32.const 0))
      (then unreachable))
    (if (call $get_int32 (local.get $env) (i32.load offset=16 (local.get $f))
          (i32.add (local.get $f) (i32.const 24)))
      (then (i32.store offset=24 (local.get $f) (i32.const 0))))
    (if (call $create_int32 (local.get $env)
          (call $define_lazy (local.get $env) (i32.load offset=4 (local.get $f))
            (i32.load offset=8 (local.get $f)) (i32.load offset=12 (local.get $f))
            (i32.load offset=24 (local.get $f)))
          (i32.add (local.get $f) (i32.const 32)))
      (then unreachable))
    (global.set $sp (local.get $f))
    (i32.load offset=32 (local.get $f)))

  ;; isLazy(object, key): 1 or 0, or minus the status.
  (func $is_lazy_cb (param $env i32) (param $info i32) (result i32)
    (local $f i32) (local $status i32)
    (local.set $f (call $push))
    (i32.store (local.get $f) (i32.const 2))
    (if (call $get_cb_info (local.get $env) (local.get $info) (local.get $f)
          (i32.add (local.get $f) (i32.const 4)) (i32.const 0) (i32.const 0))
      (then unreachable))
    (i32.store8 offset=24 (local.get $f) (i32.const 7))
    (local.set $status (call $is_lazy (local.get $env) (i32.load offset=4 (local.get $f))
      (i32.load offset=8 (local.get $f)) (i32.add (local.get $f) (i32.const 24))))
    (if (call $create_int32 (local.get $env)
          (select (i32.sub (i32.const 0) (local.get $status))
            (i32.load8_u offset=24 (local.get $f)) (local.get $status))
          (i32.add (local.get $f) (i32.const 32)))
      (then unreachable))
    (global.set $sp (local.get $f))
    (i32.load offset=32 (local.get $f)))

  ;; makeContext(): a new contextify context's global.
  (func $make_context_cb (param $env i32) (param $info i32) (result i32)
    (local $f i32)
    (local.set $f (call $push))
    (if (call $create_object (local.get $env) (local.get $f)) (then unreachable))
    (if (call $create_string (local.get $env) (i32.const 140) (i32.const 7)
          (i32.add (local.get $f) (i32.const 4)))
      (then unreachable))
    (if (call $make_context (local.get $env) (i32.load (local.get $f))
          (i32.load offset=4 (local.get $f)) (i32.const 0) (i32.const 1) (i32.const 1)
          (i32.const 0) (i32.const 0) (i32.add (local.get $f) (i32.const 8)))
      (then unreachable))
    (global.set $sp (local.get $f))
    (i32.load offset=8 (local.get $f)))

  ;; runIn(global, source): the script's result in that context (its throw
  ;; propagates).
  (func $run_in_cb (param $env i32) (param $info i32) (result i32)
    (local $f i32)
    (local.set $f (call $push))
    (i32.store (local.get $f) (i32.const 2))
    (if (call $get_cb_info (local.get $env) (local.get $info) (local.get $f)
          (i32.add (local.get $f) (i32.const 4)) (i32.const 0) (i32.const 0))
      (then unreachable))
    (i32.store offset=16 (local.get $f) (i32.const 0))
    (i32.store offset=20 (local.get $f) (i32.load offset=8 (local.get $f)))
    (i32.store offset=24 (local.get $f) (i32.const 0))
    (if (call $create_string (local.get $env) (i32.const 148) (i32.const 7)
          (i32.add (local.get $f) (i32.const 28)))
      (then unreachable))
    (i32.store offset=32 (local.get $f) (i32.const 0))
    (drop (call $contextify_run_script (local.get $env) (i32.load offset=4 (local.get $f))
      (i32.add (local.get $f) (i32.const 16)) (i32.load offset=28 (local.get $f))
      (i32.const 0) (i32.const 0) (i64.const -1) (i32.const 1) (i32.const 0) (i32.const 0)
      (i32.const 0) (i32.add (local.get $f) (i32.const 32))))
    (global.set $sp (local.get $f))
    (i32.load offset=32 (local.get $f)))

  (elem (i32.const 1) $define_lazy_cb $is_lazy_cb $make_context_cb $run_in_cb)
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

    /// Runs `source` (a script that returns a string); its result, or the
    /// status if it threw or did not return a string.
    fn run(&mut self, source: &str) -> String {
        self.memory
            .view(&self.store)
            .write(u64::from(SOURCE_PTR), source.as_bytes())
            .unwrap();
        let eval: TypedFunction<(i32, i32), i32> = self.typed("eval");
        let status = eval
            .call(&mut self.store, SOURCE_PTR as i32, source.len() as i32)
            .expect("eval trapped");
        if status != 0 {
            return format!("script status {status}");
        }
        let read: TypedFunction<(i32, i32), i32> = self.typed("read");
        let status = read
            .call(&mut self.store, RESULT_PTR as i32, RESULT_SIZE as i32)
            .unwrap();
        if status != 0 {
            return format!("result status {status}");
        }
        let length: TypedFunction<(), i32> = self.typed("read_length");
        let length = length.call(&mut self.store).unwrap() as usize;
        let mut bytes = vec![0u8; length];
        self.memory
            .view(&self.store)
            .read(u64::from(RESULT_PTR), &mut bytes)
            .unwrap();
        String::from_utf8(bytes).unwrap()
    }

    fn release(&mut self) -> i32 {
        let release: TypedFunction<(), i32> = self.typed("release");
        release.call(&mut self.store).expect("release trapped")
    }
}

/// Shared helpers for every script: `check(condition, what)` collects
/// failures; `done()` returns "ok" or them; `counted()` makes an initializer
/// that counts its calls and returns a fresh object per name.
const PRELUDE: &str = r#"
const failures = [];
const check = (condition, what) => { if (!condition) failures.push(what); };
const done = () => failures.length ? failures.join("; ") : "ok";
const W = 1, E = 2, C = 4;
function counted() {
  const calls = [];
  const values = {};
  const initializer = function (name) {
    calls.push(String(name));
    if (!(name in values)) values[name] = { name: String(name), self: this };
    return values[name];
  };
  return { calls, values, initializer };
}
const describe = (object) => JSON.stringify(Reflect.ownKeys(object).map((key) => {
  const d = Object.getOwnPropertyDescriptor(object, key);
  return [String(key), "value" in d, !!d.writable, !!d.enumerable, !!d.configurable];
}));
"#;

fn run(guest: &mut Guest, body: &str) -> String {
    guest.run(&format!("(() => {{ try {{ {PRELUDE}\n{body}\n return done(); }} catch (e) {{ return 'threw ' + e + ' ' + e.stack; }} }})()"))
}

/// `in`, hasOwnProperty, Object.keys, for-in, Reflect.ownKeys and
/// JSON.stringify of the key list call no initializer; the descriptor is a
/// data descriptor with the attributes asked for, read once.
#[test]
fn looks_like_a_data_property_before_its_first_read() {
    let mut guest = Guest::new();
    let result = run(
        &mut guest,
        r#"
      const { calls, initializer } = counted();
      const object = { before: 1 };
      check(defineLazy(object, ["all", "none", "conf", "enum"], initializer, W | E | C) === 0, "define all");
      check(defineLazy(object, ["none"], initializer, 0) === 0, "define none");
      check(defineLazy(object, ["conf"], initializer, C) === 0, "define conf");
      check(defineLazy(object, ["enum"], initializer, E | W) === 0, "define enum");
      const symbol = Symbol("lazy");
      check(defineLazy(object, [symbol], initializer, W | C) === 0, "define symbol");
      object.after = 2;
      check("all" in object && "none" in object && symbol in object, "in");
      check(Object.prototype.hasOwnProperty.call(object, "conf"), "hasOwnProperty");
      check(JSON.stringify(Object.keys(object)) === '["before","all","enum","after"]', "keys " + JSON.stringify(Object.keys(object)));
      const seen = []; for (const key in object) seen.push(key);
      check(seen.join() === "before,all,enum,after", "for-in " + seen.join());
      check(Reflect.ownKeys(object).length === 7, "ownKeys");
      check(isLazy(object, "all") === 1 && isLazy(object, symbol) === 1, "isLazy before");
      check(calls.length === 0, "no initializer call yet: " + calls.join());
      const expect = {
        all: [true, true, true], none: [false, false, false], conf: [false, false, true], enum: [true, true, false],
      };
      for (const [name, [writable, enumerable, configurable]] of Object.entries(expect)) {
        const d = Object.getOwnPropertyDescriptor(object, name);
        check("value" in d && !("get" in d) && !("set" in d), name + " data descriptor");
        check(d.writable === writable && d.enumerable === enumerable && d.configurable === configurable, name + " attributes " + [d.writable, d.enumerable, d.configurable]);
        check(d.value && d.value.name === name, name + " value");
        check(isLazy(object, name) === 0, name + " isLazy after");
      }
      const s = Object.getOwnPropertyDescriptor(object, symbol);
      check(s.writable && !s.enumerable && s.configurable && s.value.name === "Symbol(lazy)", "symbol descriptor");
      check(calls.join() === "all,none,conf,enum,Symbol(lazy)", "one call each: " + calls.join());
      check(object.all === object.all && calls.length === 5, "value kept");
      check(object.all.self === object, "this is the holder");
    "#,
    );
    assert_eq!(result, "ok");
    assert_eq!(guest.release(), 0);
}

/// Key order holds before and after reads in any order, on a plain object and
/// on the global object; the initializer runs once per name however often it
/// is read; the global's initializer gets the global (proxy) as `this`.
#[test]
fn key_order_and_one_call() {
    let mut guest = Guest::new();
    let result = run(
        &mut guest,
        r#"
      const { calls, initializer } = counted();
      const object = { a: 1 };
      defineLazy(object, ["X"], initializer, W | C);
      object.b = 2;
      defineLazy(object, ["Y", "Z"], initializer, W | E | C);
      object.c = 3;
      const order = JSON.stringify(Reflect.ownKeys(object));
      check(order === '["a","X","b","Y","Z","c"]', "order " + order);
      for (let i = 0; i < 3; i++) { object.Z; object.X; object.Y; }
      check(JSON.stringify(Reflect.ownKeys(object)) === order, "order after reads");
      check(calls.join() === "Z,X,Y", "calls " + calls.join());
      const eager = { a: 1 };
      Object.defineProperty(eager, "X", { value: 0, writable: true, configurable: true });
      eager.b = 2;
      Object.defineProperty(eager, "Y", { value: 0, writable: true, enumerable: true, configurable: true });
      Object.defineProperty(eager, "Z", { value: 0, writable: true, enumerable: true, configurable: true });
      eager.c = 3;
      check(describe(object) === describe(eager), "descriptors " + describe(object));

      const g = counted();
      const keys = Object.getOwnPropertyNames(globalThis);
      check(defineLazy(globalThis, ["LazyOne", "LazyTwo"], g.initializer, W | C) === 0, "define on global");
      globalThis.eagerAfter = 1;
      const lazyKeys = JSON.stringify(Object.getOwnPropertyNames(globalThis));
      check(JSON.stringify(keys.concat(["LazyOne", "LazyTwo", "eagerAfter"])) === lazyKeys, "global order");
      check(typeof LazyTwo === "object" && LazyTwo.self === globalThis, "global this");
      check(LazyOne === globalThis.LazyOne && g.calls.join() === "LazyTwo,LazyOne", "global calls " + g.calls.join());
      check(JSON.stringify(Object.getOwnPropertyNames(globalThis)) === lazyKeys, "global order after");
      const d = Object.getOwnPropertyDescriptor(globalThis, "LazyOne");
      check(d.writable && !d.enumerable && d.configurable, "global attributes");
    "#,
    );
    assert_eq!(result, "ok");
    assert_eq!(guest.release(), 0);
}

/// A throwing initializer: the read throws its error, the property stays
/// lazy and in place, the next read calls it again; an initializer may read
/// other lazy properties (one builds another) and its own name re-entrantly.
#[test]
fn a_throwing_initializer_leaves_the_property_lazy() {
    let mut guest = Guest::new();
    let result = run(
        &mut guest,
        r#"
      let calls = 0, fail = true;
      const object = { first: 0 };
      defineLazy(object, ["boom", "other"], function (name) {
        calls++;
        if (name === "boom" && fail) throw new RangeError("refused");
        if (name === "boom") return this.other + 1;
        return 41;
      }, W | C);
      object.last = 0;
      const order = JSON.stringify(Reflect.ownKeys(object));
      let error;
      try { object.boom; } catch (e) { error = e; }
      check(error instanceof RangeError && error.message === "refused", "throws " + error);
      check(isLazy(object, "boom") === 1 && calls === 1, "still lazy");
      check(JSON.stringify(Reflect.ownKeys(object)) === order, "still in place");
      error = undefined;
      try { Object.getOwnPropertyDescriptor(object, "boom"); } catch (e) { error = e; }
      check(calls === 2 && error && error.message === "refused" && isLazy(object, "boom") === 1, "a descriptor read retries " + calls);
      fail = false;
      check(object.boom === 42 && calls === 4, "next read " + calls);
      check(isLazy(object, "other") === 0 && object.other === 41 && calls === 4, "built by the other");
      check(JSON.stringify(Reflect.ownKeys(object)) === order, "order after");

      // Its own name from inside: a RangeError there, and it stays lazy.
      let inner;
      const self = {};
      defineLazy(self, ["again"], function () { try { this.again; } catch (e) { inner = e; } return 3; }, W | C);
      check(self.again === 3 && inner instanceof RangeError, "re-entrant read " + inner);
      check(isLazy(self, "again") === 0 && self.again === 3, "re-entrant built");
      const loop = {};
      defineLazy(loop, ["again"], function () { return this.again; }, W | C);
      try { loop.again; } catch (e) { inner = e; }
      check(inner instanceof RangeError && isLazy(loop, "again") === 1, "uncaught re-entrant read leaves it lazy");
      // An initializer that changes its own property, or another's in flight.
      const changed = { first: 0 };
      defineLazy(changed, ["assigned", "deleted", "defined", "relazied", "outer", "inner"], function (name) {
        if (name === "assigned") this.assigned = "page";
        if (name === "deleted") delete this.deleted;
        if (name === "defined") Object.defineProperty(this, "defined", { value: "defined" });
        if (name === "relazied") defineLazy(this, ["relazied"], () => "second", W | E | C);
        if (name === "outer") return this.inner;
        if (name === "inner") { delete this.outer; return "inner"; }
        return "initializer";
      }, W | E | C);
      changed.last = 0;
      const results = {};
      for (const name of ["assigned", "deleted", "defined", "relazied", "outer"]) {
        try { results[name] = changed[name]; } catch (e) { results[name] = e.constructor.name; }
      }
      // Object.defineProperty reads the descriptor first: a re-entrant read.
      check(JSON.stringify(results) === '{"assigned":"TypeError","deleted":"TypeError","defined":"RangeError","relazied":"TypeError","outer":"TypeError"}', "changed " + JSON.stringify(results));
      check(isLazy(changed, "defined") === 1, "a failed redefinition leaves it lazy");
      check(isLazy(changed, "relazied") === 1 && changed.relazied === "second", "the new lazy definition stays");
      check(changed.assigned === "page" && !("deleted" in changed) && !("outer" in changed) && changed.inner === "inner", "kept what they did");
      check(JSON.stringify(Object.keys(changed)) === '["first","assigned","defined","relazied","inner","last"]', "keys " + JSON.stringify(Object.keys(changed)));
    "#,
    );
    assert_eq!(result, "ok");
    assert_eq!(guest.release(), 0);
}

/// Assigning or deleting before the first read replaces the property without
/// calling the initializer; defining it first calls it once and ends as the
/// same definition on an eager property would; freezing keeps it lazy.
#[test]
fn page_writes_before_the_first_read() {
    let mut guest = Guest::new();
    let result = run(
        &mut guest,
        r#"
      const lazy = counted();
      const build = (object, define) => {
        object.a = 0;
        define(object, ["set", "gone", "defined", "accessor", "frozen"]);
        object.z = 0;
        return object;
      };
      const eagerly = build({}, (object, names) => { for (const name of names) Object.defineProperty(object, name, { value: { name }, writable: true, configurable: true }); });
      const lazily = build({}, (object, names) => defineLazy(object, names, lazy.initializer, W | C));
      for (const object of [eagerly, lazily]) {
        object.set = 5;
        delete object.gone;
        Object.defineProperty(object, "defined", { value: 7, enumerable: true });
        Object.defineProperty(object, "accessor", { get() { return 8; }, configurable: true });
      }
      check(lazy.calls.join() === "defined,accessor", "calls " + lazy.calls.join());
      check(isLazy(lazily, "set") === 0 && lazily.set === 5, "assigned");
      check(!("gone" in lazily) && isLazy(lazily, "gone") === 0, "deleted");
      check(isLazy(lazily, "frozen") === 1, "frozen untouched");
      Object.freeze(eagerly); Object.freeze(lazily);
      check(isLazy(lazily, "frozen") === 1, "freeze keeps it lazy");
      check(lazy.calls.join() === "defined,accessor", "freeze calls nothing " + lazy.calls.join());
      check(lazily.frozen.name === "frozen" && eagerly.frozen.name === "frozen", "frozen read");
      check(describe(eagerly) === describe(lazily), "same frozen shape");
      check(lazy.calls.join() === "defined,accessor,frozen", "calls after " + lazy.calls.join());
      let threw = false;
      try { (function () { "use strict"; lazily.frozen = 1; })(); } catch (e) { threw = e instanceof TypeError; }
      check(threw, "frozen is read-only");
    "#,
    );
    assert_eq!(result, "ok");
    assert_eq!(guest.release(), 0);
}

/// Each context's initializer builds in its own context, whichever context
/// reads the property; one context's lazy names do not appear in another.
#[test]
fn one_initializer_per_context() {
    let mut guest = Guest::new();
    let result = run(
        &mut guest,
        r#"
      const other = makeContext();
      check(other !== globalThis && typeof other === "object", "context");
      other.defineLazy = defineLazy;
      other.isLazy = isLazy;
      check(runIn(other, `defineLazy(globalThis, ["Shared", "OnlyThere"], function (name) { return { name, realm: Object, self: this }; }, 5)`) === 0, "define there");
      check(defineLazy(globalThis, ["Shared"], function (name) { return { name, realm: Object, self: this }; }, 5) === 0, "define here");
      check(!("OnlyThere" in globalThis) && "OnlyThere" in other, "names stay in their context");
      const there = other.Shared;
      check(there.realm !== Object && there.realm === runIn(other, "Object"), "built in its context");
      check(there.self === other, "this is that global");
      check(Object.getPrototypeOf(there) === runIn(other, "Object.prototype"), "its object");
      check(Shared.realm === Object && Shared.self === globalThis && Shared !== there, "here");
      check(runIn(other, "OnlyThere.realm === Object && isLazy(globalThis, 'Shared') === 0") === true, "there, read there");
      check(isLazy(other, "OnlyThere") === 0, "built");
    "#,
    );
    assert_eq!(result, "ok");
    assert_eq!(guest.release(), 0);
}

/// Reads through a prototype (the holder is not the receiver), `with`, and
/// Reflect.get with another receiver.
#[test]
fn reads_through_a_prototype() {
    let mut guest = Guest::new();
    let result = run(
        &mut guest,
        r#"
      const { calls, initializer } = counted();
      const base = {};
      defineLazy(base, ["P", "Q", "R"], initializer, W | C);
      const derived = Object.create(base);
      const value = derived.P;
      check(value && value.name === "P" && value.self === base, "value through prototype " + JSON.stringify(value && value.name));
      check(!Object.prototype.hasOwnProperty.call(derived, "P"), "not on the receiver");
      check(isLazy(base, "P") === 0, "replaced on the base");
      check(base.P === value, "same value from the base");
      check(Reflect.get(base, "Q", {}).name === "Q", "Reflect.get receiver");
      let r; with (base) { r = R; }
      check(r.name === "R", "with");
      check(calls.length === 3, "calls " + calls.join());
    "#,
    );
    assert_eq!(result, "ok");
    assert_eq!(guest.release(), 0);
}

/// isLazy calls nothing: a JS accessor's getter, a proxy's traps, V8's own
/// native data properties; argument checks for both imports, the same from a
/// strict caller.
#[test]
fn checks_and_queries_call_nothing() {
    let mut guest = Guest::new();
    let result = run(
        &mut guest,
        r#"
      let getter = 0, traps = 0;
      const object = { data: 1, get accessor() { getter++; return 1; } };
      check(isLazy(object, "data") === 0 && isLazy(object, "accessor") === 0 && isLazy(object, "missing") === 0, "plain");
      check(isLazy([1, 2], "length") === 0 && isLazy(function f(a) {}, "name") === 0, "native data");
      const proxy = new Proxy({}, new Proxy({}, { get() { traps++; return undefined; } }));
      check(isLazy(proxy, "x") === 0, "proxy");
      check(getter === 0 && traps === 0, "nothing called " + getter + " " + traps);
      const NAPI_INVALID_ARG = 1, NAPI_OBJECT_EXPECTED = 2, NAPI_NAME_EXPECTED = 4, NAPI_FUNCTION_EXPECTED = 5, NAPI_ARRAY_EXPECTED = 8;
      const f = () => 1;
      check(defineLazy(1, ["a"], f, 0) === NAPI_OBJECT_EXPECTED, "object");
      check(defineLazy({}, "a", f, 0) === NAPI_ARRAY_EXPECTED, "array");
      const target = {};
      check(defineLazy(target, ["a", 1], f, 0) === NAPI_NAME_EXPECTED && !("a" in target), "names checked first");
      check(defineLazy({}, ["a"], {}, 0) === NAPI_FUNCTION_EXPECTED, "function");
      check(defineLazy({}, ["a"], f, 8) === NAPI_INVALID_ARG, "attributes");
      check(isLazy(1, "a") === -NAPI_OBJECT_EXPECTED && isLazy({}, 1) === -NAPI_NAME_EXPECTED, "isLazy checks");
      const fixed = {};
      Object.defineProperty(fixed, "b", { value: 1 });
      check(defineLazy(fixed, ["a", "b", "c"], f, 0) === 9 && "a" in fixed && !("c" in fixed), "stops at a non-configurable one");
      const sealed = Object.preventExtensions({});
      check(defineLazy(sealed, ["a"], f, 0) === 9 && !("a" in sealed), "non-extensible");
      check(defineLazy(proxy, ["a"], f, 0) === 9 && traps === 0, "a proxy defines nothing");
      check(defineLazy(proxy, [], f, 0) === 9 && traps === 0, "a proxy with no names");
      const strict = function (object, names) { "use strict"; return defineLazy(object, names, f, 0); };
      check(strict(fixed, ["b"]) === 9, "strict caller, non-configurable");
      check(strict(Object.preventExtensions({}), ["a"]) === 9, "strict caller, non-extensible");
      const array = [1, 2];
      check(defineLazy(array, ["5"], f, 0) === NAPI_NAME_EXPECTED && array.length === 2 && !("5" in array), "an array index");
      check(defineLazy(target, ["b", "0"], f, 0) === NAPI_NAME_EXPECTED && !("b" in target), "index names checked first");
      check(defineLazy({}, new Array(2 ** 32 - 1), f, 0) === NAPI_NAME_EXPECTED, "holes");
      check(defineLazy({}, [], f, 0) === 0, "no names");
    "#,
    );
    assert_eq!(result, "ok");
    assert_eq!(guest.release(), 0);
}
