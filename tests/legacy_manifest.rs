//! Frozen N-API import contract of wasmer/edgejs@0.0.1.
//! Source atom SHA-256: ca6467e67c8503474cb4204cd2dbbae387c8fa19cad6f6c23131844143e27ccc.

use wasmer::{AsStoreMut, Extern, Instance, Module, Store, Type};
use wasmer_napi::NapiCtx;

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
