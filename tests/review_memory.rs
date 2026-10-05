use wasmer::{AsStoreMut, Instance, Module, Store};
use wasmer_napi::NapiCtx;
use wasmer_napi::budget::Pool;

#[test]
fn invalid_external_buffer_ranges_are_rejected() {
    for (name, import, operation) in [
        (
            "external_arraybuffer",
            "napi_create_external_arraybuffer",
            "(call $op (local.get $env) (i32.mul (memory.size) (i32.const 65536)) (i32.const 1) (i32.const 0) (i32.const 0) (i32.const 20))",
        ),
        (
            "external_buffer",
            "napi_create_external_buffer",
            "(call $op (local.get $env) (i32.const 1) (i32.mul (memory.size) (i32.const 65536)) (i32.const 0) (i32.const 0) (i32.const 20))",
        ),
        (
            "negative_external_arraybuffer",
            "napi_create_external_arraybuffer",
            "(call $op (local.get $env) (i32.const 64) (i32.const -1) (i32.const 0) (i32.const 0) (i32.const 20))",
        ),
        (
            "negative_external_buffer",
            "napi_create_external_buffer",
            "(call $op (local.get $env) (i32.const -1) (i32.const 64) (i32.const 0) (i32.const 0) (i32.const 20))",
        ),
    ] {
        let wat = format!(
            r#"(module
        (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
        (import "napi" "{import}" (func $op (param i32 i32 i32 i32 i32 i32) (result i32)))
        (memory (export "memory") 1)
        (func (export "run") (result i32) (local $env i32)
          (drop (call $create (i32.const 8) (i32.const 4) (i32.const 8)))
          (local.set $env (i32.load (i32.const 4)))
          {operation}))"#
        );
        let ctx = NapiCtx::default();
        let mut store = Store::default();
        let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
        let session = ctx.new_session(&module).unwrap();
        let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        session
            .configure_instance(&mut store.as_store_mut(), &instance, None)
            .unwrap();
        let status = instance
            .exports
            .get_typed_function::<(), i32>(&store, "run")
            .unwrap()
            .call(&mut store)
            .unwrap();
        assert_ne!(status, 0, "{name} accepted an invalid guest range");
    }
}

#[test]
fn invalid_allocation_requests_are_rejected_before_native_calls() {
    let cases = [
        (
            "napi_create_arraybuffer",
            "(param i32 i32 i32 i32)",
            "(call $op (local.get $env) (i32.const -1) (i32.const 24) (i32.const 28))",
        ),
        (
            "napi_create_buffer",
            "(param i32 i32 i32 i32)",
            "(call $op (local.get $env) (i32.const -1) (i32.const 24) (i32.const 28))",
        ),
        (
            "napi_create_buffer_copy",
            "(param i32 i32 i32 i32 i32)",
            "(call $op (local.get $env) (i32.const -1) (i32.const 64) (i32.const 24) (i32.const 28))",
        ),
        (
            "node_api_create_sharedarraybuffer",
            "(param i32 i32 i32 i32)",
            "(call $op (local.get $env) (i32.const -1) (i32.const 24) (i32.const 28))",
        ),
    ];
    for (name, signature, operation) in cases {
        let wat = format!(
            r#"(module
          (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
          (import "napi" "{name}" (func $op {signature} (result i32)))
          (memory (export "memory") 1)
          (func (export "run") (result i32) (local $env i32)
            (drop (call $create (i32.const 8) (i32.const 4) (i32.const 8)))
            (local.set $env (i32.load (i32.const 4)))
            {operation}))"#
        );
        let ctx = NapiCtx::default();
        let mut store = Store::default();
        let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
        let session = ctx.new_session(&module).unwrap();
        let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        session
            .configure_instance(&mut store.as_store_mut(), &instance, None)
            .unwrap();
        let status = instance
            .exports
            .get_typed_function::<(), i32>(&store, "run")
            .unwrap()
            .call(&mut store)
            .unwrap();
        assert_ne!(status, 0, "{name} accepted an invalid allocation request");
    }
}

#[test]
fn shared_arraybuffer_exposes_a_guest_address_for_its_backing_store() {
    let wat = r#"(module
      (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
      (import "napi" "node_api_create_sharedarraybuffer" (func $create_sab (param i32 i32 i32 i32) (result i32)))
      (memory (export "memory") 1)
      (func (export "run") (result i32) (local $env i32) (local $ptr i32)
        (if (call $create (i32.const 8) (i32.const 4) (i32.const 8))
          (then (return (i32.const 1))))
        (local.set $env (i32.load (i32.const 4)))
        (if (call $create_sab (local.get $env) (i32.const 8) (i32.const 24) (i32.const 28))
          (then (return (i32.const 2))))
        (local.set $ptr (i32.load (i32.const 24)))
        (if (i32.eqz (local.get $ptr)) (then (return (i32.const 3))))
        (if (i32.gt_u (i32.add (local.get $ptr) (i32.const 8))
                      (i32.mul (memory.size) (i32.const 65536)))
          (then (return (i32.const 4))))
        (i64.store (local.get $ptr) (i64.const 123456789))
        (if (i64.ne (i64.load (local.get $ptr)) (i64.const 123456789))
          (then (return (i32.const 5))))
        (i32.const 0)))"#;
    let ctx = NapiCtx::default();
    let mut store = Store::default();
    let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
    let session = ctx.new_session(&module).unwrap();
    let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    session
        .configure_instance(&mut store.as_store_mut(), &instance, None)
        .unwrap();
    let status = instance
        .exports
        .get_typed_function::<(), i32>(&store, "run")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert_eq!(status, 0, "shared backing store was not guest addressable");
}

#[test]
fn bigint_query_and_short_output_report_required_words_without_overwrite() {
    let wat = r#"(module
      (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
      (import "napi" "napi_create_bigint_words" (func $create_words (param i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_get_value_bigint_words" (func $get_words (param i32 i32 i32 i32 i32) (result i32)))
      (memory (export "memory") 1)
      (func (export "run") (result i32) (local $env i32)
        (drop (call $create (i32.const 8) (i32.const 4) (i32.const 8)))
        (local.set $env (i32.load (i32.const 4)))
        (i64.store (i32.const 80) (i64.const 17))
        (i64.store (i32.const 88) (i64.const 34))
        (i64.store (i32.const 96) (i64.const 51))
        (if (call $create_words (local.get $env) (i32.const 1) (i32.const 3) (i32.const 80) (i32.const 20))
          (then (return (i32.const 1))))
        (i32.store (i32.const 28) (i32.const 0))
        (if (call $get_words (local.get $env) (i32.load (i32.const 20)) (i32.const 24) (i32.const 28) (i32.const 0))
          (then (return (i32.const 2))))
        (if (i32.ne (i32.load (i32.const 28)) (i32.const 3)) (then (return (i32.const 3))))
        (i32.store (i32.const 28) (i32.const 1))
        (i64.store (i32.const 112) (i64.const 99))
        (if (call $get_words (local.get $env) (i32.load (i32.const 20)) (i32.const 24) (i32.const 28) (i32.const 104))
          (then (return (i32.const 4))))
        (if (i32.ne (i32.load (i32.const 28)) (i32.const 3)) (then (return (i32.const 5))))
        (if (i64.ne (i64.load (i32.const 104)) (i64.const 17)) (then (return (i32.const 6))))
        (if (i64.ne (i64.load (i32.const 112)) (i64.const 99)) (then (return (i32.const 7))))
        (i32.const 0)))"#;
    let ctx = NapiCtx::default();
    let mut store = Store::default();
    let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
    let session = ctx.new_session(&module).unwrap();
    let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    session
        .configure_instance(&mut store.as_store_mut(), &instance, None)
        .unwrap();
    let status = instance
        .exports
        .get_typed_function::<(), i32>(&store, "run")
        .unwrap()
        .call(&mut store)
        .unwrap();
    assert_eq!(status, 0);
}

#[test]
fn converted_utf16_and_bigint_inputs_are_charged_under_finite_budgets() {
    let wat = r#"(module
      (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf16" (func $utf16 (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_bigint_words" (func $bigint (param i32 i32 i32 i32 i32) (result i32)))
      (memory (export "memory") 1)
      (func (export "init") (result i32)
        (i32.store16 (i32.const 100) (i32.const 65))
        (i32.store16 (i32.const 102) (i32.const 66))
        (i64.store (i32.const 120) (i64.const 17))
        (i64.store (i32.const 128) (i64.const 34))
        (call $create (i32.const 8) (i32.const 4) (i32.const 8)))
      (func (export "run") (param $mode i32) (result i32)
        (if (result i32) (i32.eqz (local.get $mode))
          (then (call $utf16 (i32.load (i32.const 4)) (i32.const 100) (i32.const 2) (i32.const 20)))
          (else (call $bigint (i32.load (i32.const 4)) (i32.const 0) (i32.const 2) (i32.const 120) (i32.const 20))))))"#;
    for (mode, live_bytes) in [(0, 8u64), (1, 32u64)] {
        for allowed in [live_bytes - 1, live_bytes] {
            let ctx = NapiCtx::builder()
                .total_memory_bytes(128 * 1024 * 1024)
                .build();
            let budget = ctx.budget();
            let mut store = Store::default();
            let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
            let session = ctx.new_session(&module).unwrap();
            let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
            let instance = Instance::new(&mut store, &module, &imports).unwrap();
            session
                .configure_instance(&mut store.as_store_mut(), &instance, None)
                .unwrap();
            assert_eq!(
                instance
                    .exports
                    .get_typed_function::<(), i32>(&store, "init")
                    .unwrap()
                    .call(&mut store)
                    .unwrap(),
                0
            );
            let reserve = budget.memory_remaining().checked_sub(allowed).unwrap();
            budget.try_charge(Pool::HostTransient, reserve).unwrap();
            let status = instance
                .exports
                .get_typed_function::<i32, i32>(&store, "run")
                .unwrap()
                .call(&mut store, mode)
                .unwrap();
            if allowed == live_bytes {
                assert_eq!(status, 0, "mode {mode}");
            } else {
                assert_ne!(status, 0, "mode {mode} accepted uncharged conversion");
            }
            budget.uncharge(Pool::HostTransient, reserve);
            assert_eq!(budget.snapshot().host_transient, 0);
        }
    }
}

#[test]
fn utf16_and_bigint_outputs_use_only_charged_scratch() {
    let wat = r#"(module
      (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf16" (func $make_string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_get_value_string_utf16" (func $get_string (param i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_create_bigint_words" (func $make_bigint (param i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_get_value_bigint_words" (func $get_bigint (param i32 i32 i32 i32 i32) (result i32)))
      (memory (export "memory") 1)
      (func (export "init") (result i32)
        (i32.store16 (i32.const 100) (i32.const 65))
        (i32.store16 (i32.const 102) (i32.const 66))
        (i64.store (i32.const 120) (i64.const 17))
        (i64.store (i32.const 128) (i64.const 34))
        (if (call $create (i32.const 8) (i32.const 4) (i32.const 8))
          (then (return (i32.const 1))))
        (if (call $make_string (i32.load (i32.const 4)) (i32.const 100) (i32.const 2) (i32.const 20))
          (then (return (i32.const 2))))
        (if (call $make_bigint (i32.load (i32.const 4)) (i32.const 0) (i32.const 2) (i32.const 120) (i32.const 24))
          (then (return (i32.const 3))))
        (i32.store (i32.const 44) (i32.const 2))
        (i32.const 0))
      (func (export "run") (param $mode i32) (result i32)
        (if (result i32) (i32.eqz (local.get $mode))
          (then (call $get_string (i32.load (i32.const 4)) (i32.load (i32.const 20))
            (i32.const 300) (i32.const 2) (i32.const 40)))
          (else (call $get_bigint (i32.load (i32.const 4)) (i32.load (i32.const 24))
            (i32.const 48) (i32.const 44) (i32.const 320))))))"#;
    for (mode, live_bytes) in [(0, 4u64), (1, 20u64)] {
        for allowed in [live_bytes - 1, live_bytes] {
            let ctx = NapiCtx::builder()
                .total_memory_bytes(128 * 1024 * 1024)
                .build();
            let budget = ctx.budget();
            let mut store = Store::default();
            let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
            let session = ctx.new_session(&module).unwrap();
            let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
            let instance = Instance::new(&mut store, &module, &imports).unwrap();
            session
                .configure_instance(&mut store.as_store_mut(), &instance, None)
                .unwrap();
            assert_eq!(
                instance
                    .exports
                    .get_typed_function::<(), i32>(&store, "init")
                    .unwrap()
                    .call(&mut store)
                    .unwrap(),
                0
            );
            let reserve = budget.memory_remaining().checked_sub(allowed).unwrap();
            budget.try_charge(Pool::HostTransient, reserve).unwrap();
            let status = instance
                .exports
                .get_typed_function::<i32, i32>(&store, "run")
                .unwrap()
                .call(&mut store, mode)
                .unwrap();
            if allowed == live_bytes {
                assert_eq!(status, 0, "mode {mode}");
            } else {
                assert_ne!(status, 0, "mode {mode} accepted uncharged output scratch");
            }
            budget.uncharge(Pool::HostTransient, reserve);
            assert_eq!(budget.snapshot().host_transient, 0);
        }
    }
}
