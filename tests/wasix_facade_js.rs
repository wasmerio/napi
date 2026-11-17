//! The JavaScript-backed binding is attached through the actual machine hook.
#![cfg(all(target_arch = "wasm32", feature = "js", feature = "wasix"))]

use std::sync::Arc;
use wasixv2::{Machine, MachineBuilder, Stdio};
use wasixv2_host::WebHost;
use wasm_bindgen_test::wasm_bindgen_test;
use wasmer_napi::{NapiCtx, NapiHook};

wasm_bindgen_test::wasm_bindgen_test_configure!(run_in_browser);

// Match callback_control_js's guest-side N-API string/script/value protocol.
const GUEST: &str = r#"(module
    (import "env" "memory" (memory 2))
    (import "wasix_32v1" "proc_exit" (func $exit (param i32)))
    (import "napi" "napi_wasm_init_env" (func $init (result i32)))
    (import "napi" "napi_create_string_utf8"
        (func $string (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_run_script" (func $script (param i32 i32 i32) (result i32)))
    (import "napi" "napi_get_value_int32" (func $integer (param i32 i32 i32) (result i32)))
    (table (export "__indirect_function_table") 1 funcref)
    (global (export "__stack_pointer") (mut i32) (i32.const 65536))
    (global (export "__data_end") i32 (i32.const 65536))
    (global (export "__heap_base") i32 (i32.const 65536))
    (data (i32.const 1024) "6 * 7\00")
    (func $check (param i32) local.get 0 if unreachable end)
    (func (export "_start") (local $env i32)
        call $init local.tee $env i32.eqz if unreachable end
        local.get $env i32.const 1024 i32.const -1 i32.const 8 call $string call $check
        local.get $env i32.const 8 i32.load i32.const 12 call $script call $check
        local.get $env i32.const 12 i32.load i32.const 16 call $integer call $check
        i32.const 16 i32.load i32.const 42 i32.ne if unreachable end
        i32.const 0 call $exit unreachable))"#;

fn machine(context: &NapiCtx) -> Machine {
    MachineBuilder::new(Arc::new(WebHost::new()))
        .instantiation_hook(Arc::new(NapiHook::new(context.clone())))
        .build()
}

#[wasm_bindgen_test]
async fn facade_js_napi_executes_and_releases_the_instance_session() {
    let context = NapiCtx::builder().max_sessions(1).max_envs(1).build();
    let machine = machine(&context);
    let wasm = wat::parse_str(GUEST).unwrap();
    for _ in 0..2 {
        assert_eq!(
            machine
                .run(&wasm, Stdio::from_fns(|_| {}, |_| {}))
                .await
                .unwrap(),
            0
        );
        assert_eq!(context.active_sessions(), 0);
        let budget = context.budget().snapshot();
        assert_eq!(budget.live_isolates, 0);
        assert_eq!(budget.wasm_linear, 0);
        assert_eq!(budget.v8_heap_reserved, 0);
        assert_eq!(budget.v8_external, 0);
    }
}
