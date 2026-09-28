//! Frozen N-API import contract of wasmer/edgejs@0.0.1.
//! Source atom SHA-256: ca6467e67c8503474cb4204cd2dbbae387c8fa19cad6f6c23131844143e27ccc.

use wasmer::{AsStoreMut, Extern, Instance, Module, Store, Type};
use wasmer_napi::NapiCtx;

unsafe extern "C" {
    fn snapi_bridge_unofficial_legacy_message_is_live(message_id: u32) -> i32;
}

const SIGNATURES: &str = include_str!("fixtures/edgejs-0.0.1-napi-signatures.txt");
const IMPORTS: &str = include_str!("fixtures/edgejs-0.0.1-imports.txt");

fn signatures() -> impl Iterator<Item = (&'static str, Vec<Type>, Vec<Type>)> {
    SIGNATURES.lines().map(|line| {
        let (name, signature) = line
            .strip_prefix("napi.")
            .unwrap()
            .split_once(" (")
            .unwrap();
        let (parameters, results) = signature.split_once(") -> ").unwrap();
        let parse = |types: &str| {
            if types == "nil" || types.is_empty() {
                Vec::new()
            } else {
                types
                    .split(", ")
                    .map(|ty| match ty {
                        "i32" => Type::I32,
                        "i64" => Type::I64,
                        "f32" => Type::F32,
                        "f64" => Type::F64,
                        _ => panic!("unexpected frozen type: {ty}"),
                    })
                    .collect()
            }
        };
        (name, parse(parameters), parse(results))
    })
}

fn wat_type(ty: Type) -> &'static str {
    match ty {
        Type::I32 => "i32",
        Type::I64 => "i64",
        Type::F32 => "f32",
        Type::F64 => "f64",
        _ => panic!("unexpected frozen type: {ty:?}"),
    }
}

#[test]
fn released_atom_import_fixture_matches_frozen_signatures() {
    assert!(IMPORTS.starts_with("Import[278]:\n"));
    let (imports, _) = IMPORTS.split_once("Function[").unwrap();
    let mut from_atom: Vec<_> = imports
        .lines()
        .filter_map(|line| {
            line.split_once("<napi.")?
                .1
                .split_once('>')
                .map(|(name, _)| name)
        })
        .collect();
    let mut from_signatures: Vec<_> = signatures().map(|(name, _, _)| name).collect();
    from_atom.sort_unstable();
    from_signatures.sort_unstable();
    assert_eq!(from_atom, from_signatures);
}

#[test]
fn released_edgejs_napi_imports_have_exact_types_and_link() {
    assert_eq!(signatures().count(), 186);
    let mut store = Store::default();
    let mut wat = String::from("(module\n");
    for (name, parameters, results) in signatures() {
        use std::fmt::Write;
        write!(wat, "(import \"napi\" \"{name}\" (func").unwrap();
        for parameter in parameters {
            write!(wat, " (param {})", wat_type(parameter)).unwrap();
        }
        for result in results {
            write!(wat, " (result {})", wat_type(result)).unwrap();
        }
        wat.push_str("))\n");
    }
    wat.push_str("(memory (export \"memory\") 1))");
    let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
    let session = NapiCtx::default().new_session(&module).unwrap();
    let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
    for (name, parameters, results) in signatures() {
        let Some(Extern::Function(function)) = imports.get_export("napi", name) else {
            panic!("missing frozen import napi.{name}");
        };
        let ty = function.ty(&store);
        assert_eq!(ty.params(), parameters, "parameters of napi.{name}");
        assert_eq!(ty.results(), results, "results of napi.{name}");
    }
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    session
        .configure_instance(&mut store.as_store_mut(), &instance, None)
        .unwrap();
}

#[test]
fn legacy_serialized_payload_can_be_read_twice_then_released() {
    let wat = r#"(module
      (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
      (import "napi" "unofficial_napi_release_env" (func $release (param i32) (result i32)))
      (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "napi_get_value_string_utf8" (func $get_string (param i32 i32 i32 i32 i32) (result i32)))
      (import "napi" "unofficial_napi_serialize_value" (func $serialize (param i32 i32 i32) (result i32)))
      (import "napi" "unofficial_napi_deserialize_value" (func $deserialize (param i32 i32 i32) (result i32)))
      (import "napi" "unofficial_napi_release_serialized_value" (func $release_payload (param i32)))
      (memory (export "memory") 1)
      (data (i32.const 100) "ok")
      (func (export "run") (result i32)
        (local $env i32) (local $payload i32)
        (if (call $create (i32.const 8) (i32.const 4) (i32.const 8))
          (then (return (i32.const 1))))
        (local.set $env (i32.load (i32.const 4)))
        (if (call $string (local.get $env) (i32.const 100) (i32.const 2) (i32.const 12))
          (then (return (i32.const 2))))
        (if (call $serialize (local.get $env) (i32.load (i32.const 12)) (i32.const 16))
          (then (return (i32.const 3))))
        (local.set $payload (i32.load (i32.const 16)))
        (if (call $deserialize (local.get $env) (local.get $payload) (i32.const 20))
          (then (return (i32.const 4))))
        (if (call $get_string (local.get $env) (i32.load (i32.const 20))
                 (i32.const 64) (i32.const 3) (i32.const 80))
          (then (return (i32.const 5))))
        (if (i32.ne (i32.load16_u (i32.const 64)) (i32.const 27503))
          (then (return (i32.const 6))))
        (if (call $deserialize (local.get $env) (local.get $payload) (i32.const 24))
          (then (return (i32.const 7))))
        (if (call $get_string (local.get $env) (i32.load (i32.const 24))
                 (i32.const 64) (i32.const 3) (i32.const 80))
          (then (return (i32.const 8))))
        (if (i32.ne (i32.load16_u (i32.const 64)) (i32.const 27503))
          (then (return (i32.const 9))))
        (call $release_payload (local.get $payload))
        (if (i32.eqz (call $deserialize (local.get $env) (local.get $payload) (i32.const 24)))
          (then (return (i32.const 10))))
        (if (call $release (i32.load (i32.const 8)))
          (then (return (i32.const 11))))
        (i32.const 0))
      (func (export "release_again")
        (call $release_payload (i32.load (i32.const 16)))))"#;
    let mut store = Store::default();
    let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
    let ctx = NapiCtx::default();
    let session = ctx.new_session(&module).unwrap();
    let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    session
        .configure_instance(&mut store.as_store_mut(), &instance, None)
        .unwrap();
    let run = instance
        .exports
        .get_typed_function::<(), i32>(&store, "run")
        .unwrap();
    assert_eq!(run.call(&mut store).unwrap(), 0);
    assert_eq!(ctx.budget().snapshot().serialized_message, 0);
    let release_again = instance
        .exports
        .get_typed_function::<(), ()>(&store, "release_again")
        .unwrap();
    assert!(release_again.call(&mut store).is_err());
}

#[test]
fn legacy_serialized_payload_is_cleared_on_host_stop() {
    let wat = r#"(module
      (import "napi" "unofficial_napi_create_env" (func $create (param i32 i32 i32) (result i32)))
      (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
      (import "napi" "unofficial_napi_serialize_value" (func $serialize (param i32 i32 i32) (result i32)))
      (memory (export "memory") 1)
      (data (i32.const 100) "retained")
      (func (export "run") (result i32)
        (if (call $create (i32.const 8) (i32.const 4) (i32.const 8))
          (then (return (i32.const 1))))
        (if (call $string (i32.load (i32.const 4)) (i32.const 100) (i32.const 8) (i32.const 12))
          (then (return (i32.const 2))))
        (if (call $serialize (i32.load (i32.const 4)) (i32.load (i32.const 12)) (i32.const 16))
          (then (return (i32.const 3))))
        (i32.load (i32.const 16))))"#;
    let mut store = Store::default();
    let module = Module::new(&store, wat::parse_str(wat).unwrap()).unwrap();
    let ctx = NapiCtx::default();
    let session = ctx.new_session(&module).unwrap();
    let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    session
        .configure_instance(&mut store.as_store_mut(), &instance, None)
        .unwrap();
    let run = instance
        .exports
        .get_typed_function::<(), i32>(&store, "run")
        .unwrap();
    let message = run.call(&mut store).unwrap();
    assert!(message > 0);
    assert!(ctx.budget().snapshot().serialized_message > 0);
    assert_eq!(
        unsafe { snapi_bridge_unofficial_legacy_message_is_live(message as u32) },
        1
    );
    ctx.runtime_control().terminate_all();
    assert_eq!(ctx.budget().snapshot().serialized_message, 0);
    assert_eq!(
        unsafe { snapi_bridge_unofficial_legacy_message_is_live(message as u32) },
        0
    );
}

/// Opt in with the immutable released atom path. The manifest test above runs
/// in CI even when the 26 MiB registry artifact is not locally available.
#[cfg(feature = "cli")]
#[test]
fn released_edgejs_atom_runs_a_script_when_provided() {
    let Some(path) = std::env::var_os("NAPI_EDGEJS_0_0_1_ATOM") else {
        return;
    };
    let (exit, stdout, stderr) = wasmer_napi::cli::run_wasix_main_capture_stdio_with_ctx(
        &NapiCtx::default(),
        std::path::Path::new(&path),
        &["-e".into(), "console.log('LEGACY_NAPI_BOOT_OK')".into()],
        &[],
    )
    .unwrap();
    assert_eq!(exit, 0, "{stderr}");
    assert!(stdout.contains("LEGACY_NAPI_BOOT_OK"), "{stdout}\n{stderr}");
}

#[cfg(feature = "cli")]
#[test]
fn released_edgejs_atom_reports_version_when_provided() {
    let Some(path) = std::env::var_os("NAPI_EDGEJS_0_0_1_ATOM") else {
        return;
    };
    let (exit, stdout, stderr) = wasmer_napi::cli::run_wasix_main_capture_stdio_with_ctx(
        &NapiCtx::default(),
        std::path::Path::new(&path),
        &["--version".into()],
        &[],
    )
    .unwrap();
    assert_eq!(exit, 0, "{stderr}");
    assert!(stdout.starts_with('v'), "{stdout}\n{stderr}");
}

#[cfg(feature = "cli")]
#[test]
#[ignore = "released atom's ESM bootstrap requires an unavailable cjs_lexer binding"]
fn released_edgejs_atom_runs_esm_when_provided() {
    let Some(path) = std::env::var_os("NAPI_EDGEJS_0_0_1_ATOM") else {
        return;
    };
    let (exit, stdout, stderr) = wasmer_napi::cli::run_wasix_main_capture_stdio_with_ctx(
        &NapiCtx::default(),
        std::path::Path::new(&path),
        &[
            "-e".into(),
            "console.log('LEGACY_ESM_SYNC'); import('data:text/javascript,export default 7').then(m => console.log('LEGACY_ESM_' + m.default), e => console.error('LEGACY_ESM_ERROR', e)); setTimeout(() => console.log('LEGACY_ESM_TIMER'), 10)".into(),
        ],
        &[],
    )
    .unwrap();
    assert_eq!(exit, 0, "{stderr}");
    assert!(stdout.contains("LEGACY_ESM_b"), "{stdout}\n{stderr}");
}

#[cfg(feature = "cli")]
#[test]
#[ignore = "released atom's worker cleanup requires a stable native owning thread"]
fn released_edgejs_atom_runs_worker_when_provided() {
    let Some(path) = std::env::var_os("NAPI_EDGEJS_0_0_1_ATOM") else {
        return;
    };
    let (exit, stdout, stderr) = wasmer_napi::cli::run_wasix_main_capture_stdio_with_ctx(
        &NapiCtx::default(),
        std::path::Path::new(&path),
        &[
            "-e".into(),
            "const {Worker}=require('node:worker_threads'); const w=new Worker(\"const {parentPort}=require('node:worker_threads'); parentPort.postMessage(7)\",{eval:true}); w.on('message',m=>console.log('LEGACY_WORKER_'+m)); w.on('error',e=>console.error('LEGACY_WORKER_ERROR',e)); setTimeout(()=>console.log('LEGACY_WORKER_TIMER'),50)".into(),
        ],
        &[],
    )
    .unwrap();
    assert_eq!(exit, 0, "{stderr}");
    assert!(stdout.contains("LEGACY_WORKER_7"), "{stdout}\n{stderr}");
}
