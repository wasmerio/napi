#![cfg(all(target_arch = "wasm32", feature = "js"))]

use wasm_bindgen::JsCast;
use wasm_bindgen_test::wasm_bindgen_test;
use wasmer::{
    AsStoreAsync, AsStoreMut, Extern, Function, FunctionEnv, FunctionEnvMut, Instance, Memory,
    MemoryType, Module, Store, StoreAsync, Value,
};
use wasmer_napi::{NapiCtx, NapiSession};

thread_local! {
    static EXTERNAL_STORE_OWNER: std::cell::RefCell<Option<StoreAsync>> = const { std::cell::RefCell::new(None) };
}

const GUEST: &str = r#"(module
    (import "env" "memory" (memory 1))
    (import "proof" "assert_async" (func $assert_async))
    (import "proof" "drop_external_owner" (func $drop_external_owner))
    (import "napi" "napi_wasm_init_env" (func $new_env (result i32)))
    (import "napi" "napi_get_global" (func $global (param i32 i32) (result i32)))
    (import "napi" "napi_create_function" (func $function (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_set_named_property" (func $set (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_has_named_property" (func $has_named (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_get_property_names" (func $keys (param i32 i32 i32) (result i32)))
    (import "napi" "napi_coerce_to_number" (func $number (param i32 i32 i32) (result i32)))
    (import "napi" "napi_coerce_to_string" (func $text (param i32 i32 i32) (result i32)))
    (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_run_script" (func $run (param i32 i32 i32) (result i32)))
    (import "napi" "napi_create_int32" (func $create_integer (param i32 i32 i32) (result i32)))
    (import "napi" "napi_get_value_int32" (func $integer (param i32 i32 i32) (result i32)))
    (import "napi" "napi_get_named_property" (func $get (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_new_instance" (func $construct (param i32 i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_instanceof" (func $instanceof (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_get_and_clear_last_exception" (func $exception (param i32 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_get_promise_details"
        (func $promise_details (param i32 i32 i32 i32 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_contextify_run_script"
        (func $contextify (param i32 i32 i32 i32 i32 i32 i64 i32 i32 i32 i32 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
        (func $create_env (param i32 i32 i32 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
        (func $release_env (param i32 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_event_loop_checkpoint"
        (func $checkpoint (param i32 i32 i32 i32) (result i32)))
    (table (export "__indirect_function_table") 4 funcref)
    (elem (i32.const 1) $first $second $release_callback)
    (data (i32.const 64) "callback\00")
    (data (i32.const 256) "callback()")
    (data (i32.const 384) "throw new Error('release-callback')")
    (data (i32.const 448) "throws\00")
    (data (i32.const 464) "captured_exception\00")
    (data (i32.const 512) "target\00")
    (data (i32.const 528) "answer\00")
    (data (i32.const 544) "operation_result\00")
    (global $env (mut i32) (i32.const 0))
    (global $calls (mut i32) (i32.const 0))
    (global $release_status (mut i32) (i32.const -1))
    (global $throw_on_callback (mut i32) (i32.const 0))
    (global $drop_owner (mut i32) (i32.const 0))
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
    (func $release_callback (param i32 i32) (result i32)
        global.get $drop_owner if call $drop_external_owner end
        i32.const 36 i32.load i32.const 0 call $release_env global.set $release_status
        global.get $throw_on_callback
        if
            global.get $env i32.const 384 i32.const 35 i32.const 40
            call $string call $check
            global.get $env i32.const 40 i32.load i32.const 44 call $run drop
        end
        call $answer)
    (func (export "throwing_helper") (param i32) (result i32)
        global.get $env i32.const 8 i32.load i32.const 448 i32.const 48 call $get call $check
        local.get 0
        if (result i32)
            global.get $env i32.const 8 i32.load i32.const 48 i32.load i32.const 52 call $instanceof
        else
            global.get $env i32.const 48 i32.load i32.const 0 i32.const 0 i32.const 52 call $construct
        end)
    (func (export "property_helper") (param $mode i32) (result i32) (local $status i32)
        global.get $env i32.const 8 i32.load i32.const 512 i32.const 80 call $get call $check
        local.get $mode i32.const 7 i32.eq
        if
            global.get $env i32.const 80 i32.load i32.const 88 i32.const 84 i32.const 92
            call $promise_details return
        end
        local.get $mode i32.const 1 i32.eq
        if
            global.get $env i32.const 80 i32.load i32.const 528 i32.const 12 i32.load
            call $set return
        end
        local.get $mode i32.const 2 i32.eq
        if
            global.get $env i32.const 80 i32.load i32.const 528 i32.const 84
            call $has_named local.tee $status if local.get $status return end
            global.get $env i32.const 84 i32.load8_u i32.const 84 call $create_integer call $check
        else
            local.get $mode i32.const 3 i32.eq
            if
                global.get $env i32.const 80 i32.load i32.const 84 call $keys local.set $status
            else
                local.get $mode i32.const 4 i32.eq
                if
                    global.get $env i32.const 80 i32.load i32.const 84 call $number local.set $status
                else
                    local.get $mode i32.const 5 i32.eq
                    if
                        global.get $env i32.const 80 i32.load i32.const 84 call $text local.set $status
                    else
                        local.get $mode i32.const 6 i32.eq
                        if
                            global.get $env i32.const 80 i32.load i32.const 0 i32.const 0 i32.const 84
                            call $construct local.set $status
                        else
                            global.get $env i32.const 80 i32.load i32.const 528 i32.const 84
                            call $get local.set $status
                        end
                    end
                end
            end
            local.get $status if local.get $status return end
        end
        global.get $env i32.const 8 i32.load i32.const 544 i32.const 84 i32.load call $set)
    (func (export "publish_exception")
        global.get $env i32.const 56 call $exception call $check
        global.get $env i32.const 8 i32.load i32.const 464 i32.const 56 i32.load call $set call $check)
    (func (export "release_status") (result i32) global.get $release_status)
    (func (export "drop_owner_on_callback") (param i32) local.get 0 global.set $drop_owner)
    (func (export "throw_on_callback") (param i32) local.get 0 global.set $throw_on_callback)
    (func (export "release_owned") (result i32)
        i32.const 36 i32.load i32.const 0 call $release_env)
    (func (export "drain") (result i32)
        global.get $env i32.const 0 i32.const 1 i32.const 60 call $checkpoint)
    (func (export "init_owned")
        i32.const 8 i32.const 0 i32.const 32 i32.const 36 call $create_env call $check
        i32.const 32 i32.load global.set $env
        i32.const -1 global.set $release_status
        global.get $env i32.const 8 call $global call $check
        global.get $env i32.const 64 i32.const 8 i32.const 3 i32.const 0 i32.const 12
        call $function call $check
        global.get $env i32.const 8 i32.load i32.const 64 i32.const 12 i32.load
        call $set call $check)
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
    imports.define(
        "proof",
        "drop_external_owner",
        Function::new_typed(&mut store, || {
            EXTERNAL_STORE_OWNER.with(|slot| drop(slot.borrow_mut().take()));
        }),
    );
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

#[wasm_bindgen_test]
fn released_callbacks_do_not_accumulate_owned_js_roots() {
    fn cycle() {
        {
            let (_ctx, mut store, instance, memory, session) = instance();
            assert_eq!(
                eval(&mut store, &instance, &memory, "eval", "callback()"),
                42
            );
            // The synchronous guest and provider frames have all returned.
            unsafe {
                session.deactivate(&mut store.as_store_mut());
            }
        }
        // Drain the documented expired Module/Memory cache at this task boundary.
        wasmer::js::collect_shared_objects();
    }

    cycle();
    let baseline = wasm_bindgen::externref_heap_live_count();
    for _ in 0..3 {
        cycle();
        assert_eq!(wasm_bindgen::externref_heap_live_count(), baseline);
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

#[wasm_bindgen_test]
fn active_provider_release_preserves_callbacks_and_exception_cleanup() {
    let (_ctx, mut store, instance, memory, session) = instance();
    let init_owned = instance
        .exports
        .get_typed_function::<(), ()>(&store, "init_owned")
        .unwrap();
    let release = instance
        .exports
        .get_typed_function::<(), i32>(&store, "release_owned")
        .unwrap();
    let release_status = instance
        .exports
        .get_typed_function::<(), i32>(&store, "release_status")
        .unwrap();
    let set_throw = instance
        .exports
        .get_typed_function::<i32, ()>(&store, "throw_on_callback")
        .unwrap();
    for throws in [false, true] {
        init_owned.call(&mut store).unwrap();
        set_throw.call(&mut store, i32::from(throws)).unwrap();
        let script = if throws {
            "try { callback(); 0 } catch (e) { e.message === 'release-callback' ? 42 : 0 }"
        } else {
            "callback(); 42"
        };
        assert_eq!(eval(&mut store, &instance, &memory, "eval", script), 42);
        assert_ne!(release_status.call(&mut store).unwrap(), 0);
        assert_eq!(eval(&mut store, &instance, &memory, "eval", "40 + 2"), 42);
        assert_eq!(release.call(&mut store).unwrap(), 0);
        assert_ne!(release.call(&mut store).unwrap(), 0);
    }
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
}

#[wasm_bindgen_test]
fn throwing_helpers_preserve_exception_identity_and_release_custody() {
    let (_ctx, mut store, instance, memory, session) = instance();
    let init_owned = instance
        .exports
        .get_typed_function::<(), ()>(&store, "init_owned")
        .unwrap();
    let throwing_helper = instance
        .exports
        .get_typed_function::<i32, i32>(&store, "throwing_helper")
        .unwrap();
    let publish = instance
        .exports
        .get_typed_function::<(), ()>(&store, "publish_exception")
        .unwrap();
    let release = instance
        .exports
        .get_typed_function::<(), i32>(&store, "release_owned")
        .unwrap();
    for mode in [0, 1] {
        init_owned.call(&mut store).unwrap();
        let script = if mode == 0 {
            "globalThis.expectedError = new Error('constructor'); globalThis.throws = function() { throw expectedError; }; 42"
        } else {
            "globalThis.expectedError = new Error('instanceof'); globalThis.throws = function() {}; Object.defineProperty(throws, Symbol.hasInstance, {value() { throw expectedError; }}); 42"
        };
        assert_eq!(eval(&mut store, &instance, &memory, "eval", script), 42);
        assert_eq!(throwing_helper.call(&mut store, mode).unwrap(), 10);
        publish.call(&mut store).unwrap();
        assert_eq!(
            eval(
                &mut store,
                &instance,
                &memory,
                "eval",
                "captured_exception === expectedError ? 42 : 0"
            ),
            42
        );
        assert_eq!(release.call(&mut store).unwrap(), 0);
    }
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
}

#[wasm_bindgen_test]
fn property_and_coercion_callouts_preserve_nested_callbacks() {
    let (_ctx, mut store, instance, memory, session) = instance();
    let helper = instance
        .exports
        .get_typed_function::<i32, i32>(&store, "property_helper")
        .unwrap();
    let reset = instance
        .exports
        .get_typed_function::<(), ()>(&store, "reset")
        .unwrap();
    let calls = instance
        .exports
        .get_typed_function::<(), i32>(&store, "calls")
        .unwrap();
    for (mode, setup, check) in [
        (
            0,
            "globalThis.target = { get answer() { return callback(); } }; 42",
            "operation_result === 42 ? 42 : 0",
        ),
        (
            1,
            "globalThis.target = { set answer(fn) { globalThis.setter_answer = fn(); } }; 42",
            "setter_answer === 42 ? 42 : 0",
        ),
        (
            2,
            "globalThis.target = new Proxy({}, { has() { return callback() === 42; } }); 42",
            "operation_result === 1 ? 42 : 0",
        ),
        (
            3,
            "globalThis.target = new Proxy({}, { ownKeys() { callback(); return ['answer']; }, getOwnPropertyDescriptor() { return { enumerable: true, configurable: true }; } }); 42",
            "operation_result.length === 1 && operation_result[0] === 'answer' ? 42 : 0",
        ),
        (
            4,
            "globalThis.target = { valueOf() { return callback(); } }; 42",
            "operation_result === 42 ? 42 : 0",
        ),
        (
            5,
            "globalThis.target = { toString() { return String(callback()); } }; 42",
            "operation_result === '42' ? 42 : 0",
        ),
        (
            6,
            "globalThis.target = function() { this.answer = callback(); }; 42",
            "operation_result.answer === 42 ? 42 : 0",
        ),
        (
            7,
            "globalThis.target = Promise.resolve(42); Object.defineProperty(target, 'constructor', { get() { callback(); return Promise; } }); 42",
            "42",
        ),
    ] {
        assert_eq!(eval(&mut store, &instance, &memory, "eval", setup), 42);
        reset.call(&mut store).unwrap();
        assert_eq!(helper.call(&mut store, mode).unwrap(), 0);
        assert_eq!(calls.call(&mut store).unwrap(), 2);
        assert_eq!(eval(&mut store, &instance, &memory, "eval", check), 42);
    }
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
}

#[wasm_bindgen_test]
fn property_callout_exceptions_preserve_the_exact_js_value() {
    let (_ctx, mut store, instance, memory, session) = instance();
    let helper = instance
        .exports
        .get_typed_function::<i32, i32>(&store, "property_helper")
        .unwrap();
    let publish = instance
        .exports
        .get_typed_function::<(), ()>(&store, "publish_exception")
        .unwrap();
    for (mode, setup) in [
        (
            0,
            "globalThis.expectedError = {}; globalThis.target = { get answer() { callback(); throw expectedError; } }; 42",
        ),
        (
            3,
            "globalThis.expectedError = {}; globalThis.target = new Proxy({}, { ownKeys() { callback(); throw expectedError; } }); 42",
        ),
        (
            4,
            "globalThis.expectedError = {}; globalThis.target = { valueOf() { callback(); throw expectedError; } }; 42",
        ),
        (
            5,
            "globalThis.expectedError = {}; globalThis.target = { toString() { callback(); throw expectedError; } }; 42",
        ),
        (
            7,
            "globalThis.expectedError = {}; globalThis.target = Promise.resolve(42); Object.defineProperty(target, 'constructor', { get() { callback(); throw expectedError; } }); 42",
        ),
    ] {
        assert_eq!(eval(&mut store, &instance, &memory, "eval", setup), 42);
        assert_eq!(helper.call(&mut store, mode).unwrap(), 10);
        publish.call(&mut store).unwrap();
        assert_eq!(
            eval(
                &mut store,
                &instance,
                &memory,
                "eval",
                "captured_exception === expectedError ? 42 : 0"
            ),
            42
        );
    }
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
}

#[wasm_bindgen_test]
fn private_metadata_ignores_guest_prototype_overrides() {
    let (_ctx, mut store, instance, memory, session) = instance();
    let helper = instance
        .exports
        .get_typed_function::<i32, i32>(&store, "property_helper")
        .unwrap();
    assert_eq!(
        eval(
            &mut store,
            &instance,
            &memory,
            "eval",
            r#"
        globalThis.metadata_calls = 0;
        globalThis.target = { answer: 42 };
        globalThis.restore_metadata = (() => {
            const get = WeakMap.prototype.get;
            const set = WeakMap.prototype.set;
            const same = Object.is;
            const apply = Reflect.apply;
            WeakMap.prototype.get = function(key) { metadata_calls++; return apply(get, this, [key]); };
            WeakMap.prototype.set = function(key, value) { metadata_calls++; return apply(set, this, [key, value]); };
            Object.is = function(a, b) { metadata_calls++; return same(a, b); };
            return () => { WeakMap.prototype.get = get; WeakMap.prototype.set = set; Object.is = same; };
        })();
        42
    "#
        ),
        42
    );
    // Restore primordials before asserting, including when an operation failed.
    let first = helper.call(&mut store, 0);
    let second = helper.call(&mut store, 0);
    let count = eval(
        &mut store,
        &instance,
        &memory,
        "eval",
        "restore_metadata(); metadata_calls",
    );
    assert_eq!(first.unwrap(), 0);
    assert_eq!(second.unwrap(), 0);
    assert_eq!(count, 0);
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
}

struct QueuedCallbackFixture {
    ctx: NapiCtx,
    store: StoreAsync,
    instance: Instance,
    session: NapiSession,
    callback: js_sys::Function,
    drops: std::sync::Arc<std::sync::atomic::AtomicUsize>,
}

async fn queued_callback_fixture() -> QueuedCallbackFixture {
    use std::sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    };
    struct DropProbe(Arc<AtomicUsize>);
    impl Drop for DropProbe {
        fn drop(&mut self) {
            self.0.fetch_add(1, Ordering::SeqCst);
        }
    }
    let (ctx, mut store, instance, memory, session) = prepared_instance(true);
    let drops = Arc::new(AtomicUsize::new(0));
    let _probe = FunctionEnv::new(&mut store, DropProbe(drops.clone()));
    let init = instance
        .exports
        .get_typed_function::<(), ()>(&store, "init")
        .unwrap();
    let init_owned = instance
        .exports
        .get_typed_function::<(), ()>(&store, "init_owned")
        .unwrap();
    let eval = instance
        .exports
        .get_typed_function::<(i32, i32), i32>(&store, "eval")
        .unwrap();
    let store = store.into_async();
    init.call_async(&store).await.unwrap();
    init_owned.call_async(&store).await.unwrap();
    let script = "console.__wasmerNapiQueuedCallbackProof = callback; 42";
    {
        let locked = store.store().write();
        memory.view(&locked).write(1024, script.as_bytes()).unwrap();
    }
    assert_eq!(
        eval.call_async(&store, 1024, script.len() as i32)
            .await
            .unwrap(),
        42
    );
    let console = js_sys::Reflect::get(&js_sys::global(), &"console".into()).unwrap();
    let key = wasm_bindgen::JsValue::from_str("__wasmerNapiQueuedCallbackProof");
    let callback = js_sys::Reflect::get(&console, &key).unwrap();
    assert!(js_sys::Reflect::delete_property(&js_sys::Object::from(console), &key).unwrap());
    QueuedCallbackFixture {
        ctx,
        store,
        instance,
        session,
        callback: callback.unchecked_into(),
        drops,
    }
}

#[wasm_bindgen_test]
async fn queued_callbacks_block_guest_release_until_they_are_dispatched() {
    let QueuedCallbackFixture {
        store,
        instance,
        session,
        callback,
        ..
    } = queued_callback_fixture().await;
    let release = {
        let locked = store.read();
        instance
            .exports
            .get_typed_function::<(), i32>(&locked, "release_owned")
            .unwrap()
    };
    let drain = {
        let locked = store.read();
        instance
            .exports
            .get_typed_function::<(), i32>(&locked, "drain")
            .unwrap()
    };
    let pending = {
        let _locked = store.store().write();
        let value = callback.call0(&wasm_bindgen::JsValue::UNDEFINED).unwrap();
        wasm_bindgen_futures::JsFuture::from(
            value
                .dyn_into::<js_sys::Promise>()
                .expect("locked Store defers the callback"),
        )
    };
    assert_ne!(release.call_async(&store).await.unwrap(), 0);
    assert_eq!(drain.call_async(&store).await.unwrap(), 0);
    assert_eq!(pending.await.unwrap().as_f64(), Some(42.0));
    assert_eq!(release.call_async(&store).await.unwrap(), 0);
    let mut store = match store.into_store() {
        Ok(store) => store,
        Err(_) => panic!("settled callback retained Store"),
    };
    unsafe {
        session.deactivate(&mut store.as_store_mut());
    }
}

#[wasm_bindgen_test]
async fn dropping_the_last_store_retires_callbacks_before_guest_entry() {
    use std::sync::atomic::Ordering;
    let QueuedCallbackFixture {
        ctx,
        store,
        instance: _instance,
        session,
        callback,
        drops,
    } = queued_callback_fixture().await;
    let pending = {
        let _locked = store.store().write();
        let value = callback.call0(&wasm_bindgen::JsValue::UNDEFINED).unwrap();
        wasm_bindgen_futures::JsFuture::from(
            value
                .dyn_into::<js_sys::Promise>()
                .expect("locked Store defers the callback"),
        )
    };
    assert_eq!(drops.load(Ordering::SeqCst), 0);
    drop(session);
    drop(store);
    assert_eq!(drops.load(Ordering::SeqCst), 1);
    assert_eq!(ctx.active_sessions(), 0);
    assert_eq!(
        pending.await.unwrap_err().as_string().as_deref(),
        Some("N-API environment released before callback entry")
    );
    assert!(
        callback
            .call0(&wasm_bindgen::JsValue::UNDEFINED)
            .unwrap()
            .is_undefined()
    );
}

#[wasm_bindgen_test]
async fn external_callbacks_pin_the_store_through_provider_result_cleanup() {
    use std::sync::atomic::Ordering;
    let QueuedCallbackFixture {
        ctx,
        store,
        instance,
        session,
        callback,
        drops,
    } = queued_callback_fixture().await;
    let set_drop = {
        let locked = store.read();
        instance
            .exports
            .get_typed_function::<i32, ()>(&locked, "drop_owner_on_callback")
            .unwrap()
    };
    set_drop.call_async(&store, 1).await.unwrap();
    drop(session);
    EXTERNAL_STORE_OWNER.with(|slot| {
        assert!(slot.borrow_mut().replace(store).is_none());
    });
    assert_eq!(drops.load(Ordering::SeqCst), 0);
    assert_eq!(
        callback
            .call0(&wasm_bindgen::JsValue::UNDEFINED)
            .unwrap()
            .as_f64(),
        Some(42.0)
    );
    assert_eq!(drops.load(Ordering::SeqCst), 1);
    assert_eq!(ctx.active_sessions(), 0);
    EXTERNAL_STORE_OWNER.with(|slot| assert!(slot.borrow().is_none()));
    assert!(
        callback
            .call0(&wasm_bindgen::JsValue::UNDEFINED)
            .unwrap()
            .is_undefined()
    );
}
