#![cfg(all(target_arch = "wasm32", feature = "js"))]

use wasm_bindgen_test::wasm_bindgen_test;
use wasmer::{
    AsStoreMut, Extern, Function, FunctionEnv, FunctionEnvMut, Instance, Memory, MemoryType,
    Module, Store, Value,
};
use wasmer_napi::{NapiCtx, NapiSession};

const GUEST: &str = r#"(module
    (import "env" "memory" (memory 1))
    (import "proof" "assert_async" (func $assert_async))
    (import "napi" "napi_wasm_init_env" (func $new_env (result i32)))
    (import "napi" "napi_get_global" (func $global (param i32 i32) (result i32)))
    (import "napi" "napi_create_function" (func $function (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_set_named_property" (func $set (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_run_script" (func $run (param i32 i32 i32) (result i32)))
    (import "napi" "napi_create_int32" (func $create_integer (param i32 i32 i32) (result i32)))
    (import "napi" "napi_get_value_int32" (func $integer (param i32 i32 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_contextify_run_script"
        (func $contextify (param i32 i32 i32 i32 i32 i32 i64 i32 i32 i32 i32 i32) (result i32)))
    (table (export "__indirect_function_table") 3 funcref)
    (elem (i32.const 1) $first $second)
    (data (i32.const 64) "callback\00")
    (data (i32.const 256) "callback()")
    (global $env (mut i32) (i32.const 0))
    (global $calls (mut i32) (i32.const 0))
    (func $check (param i32) local.get 0 if unreachable end)
    (func $answer (result i32)
        global.get $env i32.const 42 i32.const 24 call $create_integer call $check
        i32.const 24 i32.load)
    (func $first (param i32 i32) (result i32)
        global.get $calls i32.const 1 i32.add global.set $calls
        global.get $calls i32.const 1 i32.eq
        if (result i32) i32.const 256 i32.const 10 call $eval else call $answer end)
    (func $second (export "second") (param i32 i32) (result i32)
        global.get $calls i32.const 10 i32.add global.set $calls call $answer)
    (func (export "init")
        call $assert_async
        call $new_env global.set $env
        global.get $env i32.eqz if unreachable end
        global.get $env i32.const 8 call $global call $check
        global.get $env i32.const 64 i32.const 8 i32.const 1 i32.const 0 i32.const 12
        call $function call $check
        global.get $env i32.const 8 i32.load i32.const 64 i32.const 12 i32.load
        call $set call $check)
    (func $eval (param i32 i32) (result i32)
        global.get $env local.get 0 local.get 1 i32.const 16 call $string call $check
        global.get $env i32.const 16 i32.load i32.const 20 call $run call $check
        i32.const 20 i32.load)
    (func (export "eval") (param i32 i32) (result i32)
        global.get $env local.get 0 local.get 1 call $eval
        i32.const 24 call $integer call $check i32.const 24 i32.load)
    (func (export "eval_contextify") (param i32 i32) (result i32)
        global.get $env local.get 0 local.get 1 i32.const 16 call $string call $check
        i32.const 128 i32.const 0 i32.store
        i32.const 132 i32.const 16 i32.load i32.store
        i32.const 136 i32.const 0 i32.store
        global.get $env i32.const 0 i32.const 128 i32.const 16 i32.load
        i32.const 0 i32.const 0 i64.const -1 i32.const 1 i32.const 0 i32.const 0
        i32.const 0 i32.const 20 call $contextify call $check
        global.get $env i32.const 20 i32.load i32.const 24 call $integer call $check
        i32.const 24 i32.load)
    (func (export "reset") i32.const 0 global.set $calls)
    (func (export "calls") (result i32) global.get $calls)
)"#;

fn prepared_instance(jspi: bool) -> (NapiCtx, Store, Instance, Memory, NapiSession) {
    let mut store = Store::default();
    if jspi {
        wasmer::js::set_async_mode(&mut store, wasmer::js::AsyncMode::Jspi).unwrap();
    }
    let module = Module::new(&store, wat::parse_str(GUEST).unwrap()).unwrap();
    let ctx = NapiCtx::default();
    let hooks = ctx.runtime_hooks();
    let (mut imports, state) = hooks
        .additional_imports(&module, &mut store.as_store_mut())
        .unwrap();
    let expected = FunctionEnv::new(&mut store, jspi);
    imports.define(
        "proof",
        "assert_async",
        Function::new_typed_with_env(&mut store, &expected, |env: FunctionEnvMut<bool>| {
            assert_eq!(env.as_async_mut().is_some(), *env.data());
        }),
    );
    let session = state.session().unwrap().clone();
    let Some(Extern::Memory(memory)) = imports.get_export("env", "memory") else {
        panic!("missing guest memory");
    };
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    hooks
        .configure_instance(
            &module,
            &mut store.as_store_mut(),
            &instance,
            Some(&memory),
            state,
        )
        .unwrap();
    (ctx, store, instance, memory, session)
}

fn instance() -> (NapiCtx, Store, Instance, Memory, NapiSession) {
    let (ctx, mut store, instance, memory, session) = prepared_instance(false);
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init")
        .unwrap()
        .call(&mut store)
        .unwrap();
    (ctx, store, instance, memory, session)
}

#[wasm_bindgen_test]
fn memory_buffers_follow_growth_for_shared_and_unshared_memory() {
    for shared in [false, true] {
        let mut store = Store::default();
        let memory = Memory::new(&mut store, MemoryType::new(1, Some(3), shared)).unwrap();
        let before = memory.as_js().js_buffer();
        assert!(js_sys::Object::is(&before, &memory.as_js().js_buffer()));
        js_sys::Uint8Array::new(&before).set_index(0, 7);
        memory.grow(&mut store, 1_u32).unwrap();
        let after = memory.as_js().js_buffer();
        assert!(!js_sys::Object::is(&before, &after));
        let bytes = js_sys::Uint8Array::new(&after);
        assert_eq!(bytes.length(), 2 * 65536);
        assert_eq!(bytes.get_index(0), 7);
        assert_eq!(
            js_sys::Reflect::get(&before, &"byteLength".into())
                .unwrap()
                .as_f64(),
            Some(if shared { 65536.0 } else { 0.0 })
        );
    }
}

#[wasm_bindgen_test]
async fn persistent_callbacks_do_not_retain_their_async_store() {
    let (_ctx, store, instance, _memory, session) = prepared_instance(true);
    let init = instance
        .exports
        .get_typed_function::<(), ()>(&store, "init")
        .unwrap();
    let store = store.into_async();
    init.call_async(&store).await.unwrap();
    let mut store = match store.into_store() {
        Ok(store) => store,
        Err(_) => panic!("persistent callback state retained its own Store"),
    };
    // The JavaScript root and its Promise have settled before returning to sync.
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
}

fn eval(
    store: &mut Store,
    instance: &Instance,
    memory: &Memory,
    export: &str,
    script: &str,
) -> i32 {
    memory.view(store).write(1024, script.as_bytes()).unwrap();
    instance
        .exports
        .get_typed_function::<(i32, i32), i32>(store, export)
        .unwrap()
        .call(store, 1024, script.len() as i32)
        .unwrap()
}

#[wasm_bindgen_test]
fn script_and_contextify_preserve_nested_guest_callbacks() {
    let (_ctx, mut store, instance, memory, session) = instance();
    for export in ["eval", "eval_contextify"] {
        instance
            .exports
            .get_typed_function::<(), ()>(&store, "reset")
            .unwrap()
            .call(&mut store)
            .unwrap();
        assert_eq!(
            eval(&mut store, &instance, &memory, export, "callback()"),
            42
        );
        assert_eq!(
            instance
                .exports
                .get_typed_function::<(), i32>(&store, "calls")
                .unwrap()
                .call(&mut store)
                .unwrap(),
            2
        );
        assert_eq!(eval(&mut store, &instance, &memory, "eval", "40 + 2"), 42);
    }
    // Every synchronous nested callback has returned; no provider job remains.
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
}

#[wasm_bindgen_test]
fn callbacks_use_the_current_guest_table_target() {
    let (_ctx, mut store, instance, memory, session) = instance();
    assert_eq!(
        eval(&mut store, &instance, &memory, "eval", "callback()"),
        42
    );
    let replacement = instance.exports.get_function("second").unwrap().clone();
    instance
        .exports
        .get_table("__indirect_function_table")
        .unwrap()
        .set(&mut store, 1, Value::FuncRef(Some(replacement)))
        .unwrap();
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "reset")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert_eq!(
        eval(&mut store, &instance, &memory, "eval", "callback()"),
        42
    );
    assert_eq!(
        instance
            .exports
            .get_typed_function::<(), i32>(&store, "calls")
            .unwrap()
            .call(&mut store)
            .unwrap(),
        10
    );
    // This test owns the session and all calls have returned on this thread.
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
}
