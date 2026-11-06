//! The deprecated guest API (the contextify imports and version 1 of the
//! option structs) on the bridge's realm calls, for both guest backends, as
//! lib/src/napi_compat.cc does for native code. Ids are guest value ids, 0 is
//! NULL, and a realm id is the value id of the realm's global or sandbox, 0
//! for the env's own realm. The lookups store their ids in a handle scope of
//! their own, as a guest at its root frame keeps every id stored in its
//! current scope.

use crate::snapi::*;

const NAPI_OK: i32 = 0;
const NAPI_INVALID_ARG: i32 = 1;

// napi_valuetype.
const NAPI_UNDEFINED: i32 = 0;
const NAPI_NULL: i32 = 1;
const NAPI_SYMBOL: i32 = 5;
const NAPI_OBJECT: i32 = 6;
const NAPI_FUNCTION: i32 = 7;

fn check(status: i32) -> Result<(), i32> {
    if status == NAPI_OK {
        Ok(())
    } else {
        Err(status)
    }
}

/// The type of `value`, or None when it is NULL or not a live value.
fn type_of(env: SnapiEnv, value: u32) -> Option<i32> {
    if value == 0 {
        return None;
    }
    let mut value_type = 0;
    (unsafe { snapi_bridge_typeof(env, value, &mut value_type) } == NAPI_OK).then_some(value_type)
}

fn is_nullish(env: SnapiEnv, value: u32) -> bool {
    value == 0 || matches!(type_of(env, value), Some(NAPI_UNDEFINED | NAPI_NULL))
}

/// The env's own global, whichever realm is current.
fn env_global(env: SnapiEnv) -> Result<u32, i32> {
    let mut scope = 0u64;
    check(unsafe { snapi_bridge_unofficial_enter_realm(env, 0, &mut scope) })?;
    let mut global = 0u32;
    let status = unsafe { snapi_bridge_get_global(env, &mut global) };
    let left = unsafe { snapi_bridge_unofficial_leave_realm(env, scope) };
    check(status)?;
    check(left)?;
    Ok(global)
}

fn is_env_global(env: SnapiEnv, value: u32) -> Result<bool, i32> {
    let global = env_global(env)?;
    let mut equal = 0i32;
    check(unsafe { snapi_bridge_strict_equals(env, value, global, &mut equal) })?;
    Ok(equal != 0)
}

/// The id of the realm `value` is the global or sandbox of, 0 if none.
fn realm_of(env: SnapiEnv, value: u32) -> Result<u32, i32> {
    let mut realm = 0u32;
    check(unsafe { snapi_bridge_unofficial_get_realm(env, value, &mut realm) })?;
    Ok(realm)
}

/// Runs `lookup` in a handle scope of its own, so that the value ids it
/// stores (globals) do not stay in the guest's current scope, which at a
/// guest's root frame is never closed. Ids it returns die with the scope.
fn in_lookup_scope<T>(env: SnapiEnv, lookup: impl FnOnce() -> Result<T, i32>) -> Result<T, i32> {
    let mut scope = 0u32;
    check(unsafe { snapi_bridge_open_handle_scope(env, &mut scope) })?;
    let result = lookup();
    let closed = check(unsafe { snapi_bridge_close_handle_scope(env, scope) });
    let value = result?;
    closed?;
    Ok(value)
}

/// The current realm, which the env's context meant before realms. The env's
/// own global means the env's realm even when it is also a realm's sandbox.
/// With the env's realm current no id is stored; with another, one: the id of
/// the current global, which names that realm. The native bridge stores a new
/// one each call; the host-JS backend hands out the global's live id when it
/// has one, so there it stores at most one across calls.
pub(crate) fn current_realm(env: SnapiEnv) -> Result<u32, i32> {
    let other_realm = in_lookup_scope(env, || {
        let mut global = 0u32;
        check(unsafe { snapi_bridge_get_global(env, &mut global) })?;
        Ok(realm_of(env, global)? != 0 && !is_env_global(env, global)?)
    })?;
    if !other_realm {
        return Ok(0);
    }
    // The id must outlive the call it names the realm for, so it is made
    // again outside the lookup's scope.
    let mut global = 0u32;
    check(unsafe { snapi_bridge_get_global(env, &mut global) })?;
    Ok(global)
}

/// The realm a context argument names: a contextified object's, the env's
/// for its own global, and for NULL, undefined or null the current realm, or
/// the env's when `nullish_is_current` is false. Anything else is
/// napi_invalid_arg.
pub(crate) fn realm_from_context(
    env: SnapiEnv,
    context: u32,
    nullish_is_current: bool,
) -> Result<u32, i32> {
    match type_of(env, context) {
        None if context == 0 => {}
        Some(NAPI_UNDEFINED | NAPI_NULL) => {}
        Some(NAPI_OBJECT | NAPI_FUNCTION) => {
            let realm = realm_of(env, context).map_err(|_| NAPI_INVALID_ARG)?;
            if realm != 0 || in_lookup_scope(env, || is_env_global(env, context))? {
                return Ok(realm);
            }
            return Err(NAPI_INVALID_ARG);
        }
        _ => return Err(NAPI_INVALID_ARG),
    }
    if nullish_is_current {
        current_realm(env)
    } else {
        Ok(0)
    }
}

/// The realm bytecode-open options compile for: the current realm in
/// version 1, `realm_or_null` in version 2.
pub(crate) fn bytecode_open_realm(
    env: SnapiEnv,
    version: u32,
    realm_or_null: u32,
) -> Result<u32, i32> {
    if version == 1 {
        current_realm(env)
    } else {
        Ok(realm_or_null)
    }
}

/// The realm module-create options make the module in. Version 2 takes
/// `realm_or_null`, and then the deprecated context must be nullish; else the
/// context names the realm, nullish meaning the current realm in version 1
/// and the env's in version 2.
pub(crate) fn module_create_realm(
    env: SnapiEnv,
    version: u32,
    context_or_undefined: u32,
    realm_or_null: u32,
) -> Result<u32, i32> {
    if version >= 2 && realm_or_null != 0 {
        if !is_nullish(env, context_or_undefined) {
            return Err(NAPI_INVALID_ARG);
        }
        return Ok(realm_or_null);
    }
    realm_from_context(env, context_or_undefined, version == 1)
}

/// unofficial_napi_contextify_make_context: an object or function sandbox
/// becomes the realm's sandbox, a symbol makes a fresh global. Returns the
/// realm's global: the sandbox, or the fresh global.
#[allow(clippy::too_many_arguments)]
pub(crate) fn make_context(
    env: SnapiEnv,
    sandbox_or_symbol: u32,
    name: u32,
    origin_or_undefined: u32,
    allow_code_gen_strings: i32,
    allow_code_gen_wasm: i32,
    own_microtask_queue: i32,
    host_defined_option_id: u32,
) -> Result<u32, i32> {
    if name == 0 {
        return Err(NAPI_INVALID_ARG);
    }
    let sandbox = match type_of(env, sandbox_or_symbol) {
        Some(NAPI_SYMBOL) => 0,
        Some(NAPI_OBJECT | NAPI_FUNCTION) => sandbox_or_symbol,
        _ => return Err(NAPI_INVALID_ARG),
    };
    let (mut realm, mut global) = (0u32, 0u32);
    check(unsafe {
        snapi_bridge_unofficial_create_realm(
            env,
            sandbox,
            name,
            origin_or_undefined,
            host_defined_option_id,
            i32::from(allow_code_gen_strings != 0),
            i32::from(allow_code_gen_wasm != 0),
            i32::from(own_microtask_queue != 0),
            &mut realm,
            &mut global,
        )
    })?;
    Ok(global)
}

#[cfg(all(test, not(all(target_arch = "wasm32", feature = "js"))))]
mod tests {
    use std::sync::Arc;

    use wasmer::{FunctionEnv, Imports, Instance, Memory, MemoryType, Module, Store};

    use super::*;
    use crate::budget::ResourceBudget;

    unsafe extern "C" {
        fn snapi_bridge_unofficial_get_live_value_count(env: SnapiEnv) -> u64;
    }

    /// Forwards to the deprecated imports, which a guest built against older
    /// headers calls, and to the realm imports they stand for.
    const GUEST: &str = r#"(module
      (import "env" "memory" (memory 1))
      (import "napi" "napi_wasm_init_env" (func $init (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_contextify_run_script"
        (func $contextify_run_script
          (param i32 i32 i32 i32 i32 i32 i64 i32 i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_run_script"
        (func $run_script
          (param i32 i32 i32 i32 i32 i32 i64 i32 i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_contextify_compile_function"
        (func $contextify_compile_function
          (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_compile_function"
        (func $compile_function
          (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_bytecode_open"
        (func $bytecode_open (param i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_bytecode_release"
        (func $bytecode_release (param i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_module_wrap_create"
        (func $module_create (param i32 i32 i32) (result i32)))
      (import "napi_extension_wasmer_v0" "unofficial_napi_module_wrap_destroy"
        (func $module_destroy (param i32 i32) (result i32)))
      (func (export "init") (result i32) (call $init))
      ;; (env, context or realm, source_ptr, filename, result_ptr)
      (func (export "contextify_run_script") (param i32 i32 i32 i32 i32) (result i32)
        (call $contextify_run_script (local.get 0) (local.get 1) (local.get 2) (local.get 3)
          (i32.const 0) (i32.const 0) (i64.const -1) (i32.const 1) (i32.const 0)
          (i32.const 0) (i32.const 0) (local.get 4)))
      (func (export "run_script") (param i32 i32 i32 i32 i32) (result i32)
        (call $run_script (local.get 0) (local.get 1) (local.get 2) (local.get 3)
          (i32.const 0) (i32.const 0) (i64.const -1) (i32.const 1) (i32.const 0)
          (i32.const 0) (i32.const 0) (local.get 4)))
      (func (export "contextify_compile_function") (param i32 i32 i32 i32 i32) (result i32)
        (call $contextify_compile_function (local.get 0) (local.get 2) (local.get 3)
          (i32.const 0) (i32.const 0) (local.get 1) (i32.const 0) (i32.const 0)
          (i32.const 0) (local.get 4)))
      (func (export "compile_function") (param i32 i32 i32 i32 i32) (result i32)
        (call $compile_function (local.get 0) (local.get 1) (local.get 2) (local.get 3)
          (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 0)
          (local.get 4)))
      ;; (env, options_ptr, result_ptr): opens bytecode and releases it.
      (func (export "bytecode") (param i32 i32 i32) (result i32)
        (local $status i32)
        (local.set $status (call $bytecode_open (local.get 0) (local.get 1) (local.get 2)))
        (if (local.get $status) (then (return (local.get $status))))
        (call $bytecode_release (local.get 0) (i32.load (local.get 2))))
      ;; (env, options_ptr, result_ptr): creates a module and destroys it.
      (func (export "module") (param i32 i32 i32) (result i32)
        (local $status i32)
        (local.set $status (call $module_create (local.get 0) (local.get 1) (local.get 2)))
        (if (local.get $status) (then (return (local.get $status))))
        (call $module_destroy (local.get 0) (i32.load (local.get 2)))))"#;

    const SCRIPT: u32 = 16;
    const BODY: u32 = 32;
    const MODULE_SOURCE: u32 = 48;
    const BYTECODE: u32 = 64;
    const MODULE: u32 = 128;
    const RESULT: u32 = 256;
    const CALLS: u64 = 16;

    fn status(code: i32) -> Result<(), i32> {
        check(code)
    }

    fn new_id(make: impl FnOnce(*mut u32) -> i32) -> u32 {
        let mut id = 0;
        assert_eq!(status(make(&mut id)), Ok(()));
        id
    }

    fn string(env: SnapiEnv, text: &str) -> u32 {
        new_id(|out| unsafe {
            snapi_bridge_create_string_utf8(env, text.as_ptr().cast(), text.len() as u32, out)
        })
    }

    fn le(words: &[u32]) -> Vec<u8> {
        words.iter().flat_map(|word| word.to_le_bytes()).collect()
    }

    // A guest at its root frame keeps every value id it is given, so a
    // deprecated call must store no more ids than the realm call it stands
    // for: its result.
    #[test]
    fn deprecated_imports_store_only_their_results() {
        let mut store = Store::default();
        let memory = Memory::new(&mut store, MemoryType::new(1, None, false)).unwrap();
        let mut napi_env = crate::env::NapiEnv::new(
            ResourceBudget::unlimited(),
            None,
            Arc::default(),
            Arc::default(),
        );
        napi_env.memory = Some(memory.clone());
        // Dropping the store releases the env.
        let function_env = FunctionEnv::new(&mut store, napi_env);
        let mut imports = Imports::new();
        crate::guest::napi::register_napi_imports(&mut store, &function_env, &mut imports);
        imports.define("env", "memory", memory.clone());
        let module = Module::new(&store, wat::parse_str(GUEST).unwrap()).unwrap();
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        let call5 = |name: &str| {
            instance
                .exports
                .get_typed_function::<(u32, u32, u32, u32, u32), i32>(&store, name)
                .unwrap()
        };
        let call3 = |name: &str| {
            instance
                .exports
                .get_typed_function::<(u32, u32, u32), i32>(&store, name)
                .unwrap()
        };
        let (contextify_run_script, run_script) =
            (call5("contextify_run_script"), call5("run_script"));
        let (contextify_compile_function, compile_function) = (
            call5("contextify_compile_function"),
            call5("compile_function"),
        );
        let (bytecode, module_call) = (call3("bytecode"), call3("module"));
        let guest_env = instance
            .exports
            .get_typed_function::<(), u32>(&store, "init")
            .unwrap()
            .call(&mut store)
            .unwrap();
        assert_ne!(guest_env, 0);
        let env = function_env.as_ref(&store).resolve_napi_env(guest_env);

        let filename = string(env, "count.js");
        let undefined = new_id(|out| unsafe { snapi_bridge_get_undefined(env, out) });
        let env_global = new_id(|out| unsafe { snapi_bridge_get_global(env, out) });
        let name = string(env, "count");
        let (mut realm, mut realm_global) = (0, 0);
        assert_eq!(
            status(unsafe {
                snapi_bridge_unofficial_create_realm(
                    env,
                    0,
                    name,
                    0,
                    0,
                    1,
                    1,
                    0,
                    &mut realm,
                    &mut realm_global,
                )
            }),
            Ok(())
        );
        let wrapper = new_id(|out| unsafe { snapi_bridge_create_object(env, out) });
        let url = string(env, "count.mjs");
        let source_text = string(env, "1 + 1");
        let write = |store: &Store, at: u32, bytes: &[u8]| {
            memory.view(store).write(at.into(), bytes).unwrap();
        };
        // Three text unofficial_napi_js_source structs.
        for (at, text) in [
            (SCRIPT, "1"),
            (BODY, "return 1;"),
            (MODULE_SOURCE, "export const one = 1;"),
        ] {
            write(&store, at, &le(&[0, string(env, text), 0]));
        }
        // Bytecode-open options of either version, for `realm` in version 2.
        let bytecode_options = |version: u32, realm: u32| {
            let mut words = vec![
                if version == 1 { 48 } else { 52 },
                version,
                source_text,
                filename,
                0,
                undefined,
                undefined,
                0,
                0,
                0,
                0,
                0,
            ];
            if version != 1 {
                words.push(realm);
            }
            le(&words)
        };
        // Module-create options of either version, for `context` and, in
        // version 2, `realm`.
        let module_options = |version: u32, context: u32, realm: u32| {
            let mut words = vec![
                if version == 1 { 40 } else { 44 },
                version,
                1,
                wrapper,
                url,
                context,
                MODULE_SOURCE,
                0,
                0,
                undefined,
            ];
            if version != 1 {
                words.push(realm);
            }
            le(&words)
        };

        // Checks that `CALLS` calls of `deprecated` store `extra` more value
        // ids than as many calls of `replacement`.
        let live = || unsafe { snapi_bridge_unofficial_get_live_value_count(env) };
        let stored = |store: &mut Store, call: &dyn Fn(&mut Store) -> i32| {
            let before = live();
            for _ in 0..CALLS {
                assert_eq!(call(store), 0);
            }
            live() - before
        };
        let check_pair = |store: &mut Store,
                          deprecated: &dyn Fn(&mut Store) -> i32,
                          replacement: &dyn Fn(&mut Store) -> i32,
                          extra: u64| {
            let deprecated = stored(store, deprecated);
            let replacement = stored(store, replacement);
            assert_eq!(deprecated, replacement + extra);
        };

        // With the env's realm current, no context, the env's global or a
        // realm's global.
        for (context, realm_id) in [
            (0, 0),
            (undefined, 0),
            (env_global, 0),
            (realm_global, realm),
        ] {
            check_pair(
                &mut store,
                &|store| {
                    contextify_run_script
                        .call(store, guest_env, context, SCRIPT, filename, RESULT)
                        .unwrap()
                },
                &|store| {
                    run_script
                        .call(store, guest_env, realm_id, SCRIPT, filename, RESULT)
                        .unwrap()
                },
                0,
            );
            check_pair(
                &mut store,
                &|store| {
                    contextify_compile_function
                        .call(store, guest_env, context, BODY, filename, RESULT)
                        .unwrap()
                },
                &|store| {
                    compile_function
                        .call(store, guest_env, realm_id, BODY, filename, RESULT)
                        .unwrap()
                },
                0,
            );
            let (v1, v2) = (
                module_options(1, context, 0),
                module_options(2, 0, realm_id),
            );
            check_pair(
                &mut store,
                &|store| {
                    write(store, MODULE, &v1);
                    module_call.call(store, guest_env, MODULE, RESULT).unwrap()
                },
                &|store| {
                    write(store, MODULE, &v2);
                    module_call.call(store, guest_env, MODULE, RESULT).unwrap()
                },
                0,
            );
        }
        let (v1, v2) = (bytecode_options(1, 0), bytecode_options(2, 0));
        check_pair(
            &mut store,
            &|store| {
                write(store, BYTECODE, &v1);
                bytecode.call(store, guest_env, BYTECODE, RESULT).unwrap()
            },
            &|store| {
                write(store, BYTECODE, &v2);
                bytecode.call(store, guest_env, BYTECODE, RESULT).unwrap()
            },
            0,
        );

        // With another realm current, the deprecated call also stores the
        // id that names that realm, once per call on the native bridge.
        let mut scope = 0u64;
        assert_eq!(
            status(unsafe { snapi_bridge_unofficial_enter_realm(env, realm, &mut scope) }),
            Ok(())
        );
        check_pair(
            &mut store,
            &|store| {
                contextify_run_script
                    .call(store, guest_env, 0, SCRIPT, filename, RESULT)
                    .unwrap()
            },
            &|store| {
                run_script
                    .call(store, guest_env, realm, SCRIPT, filename, RESULT)
                    .unwrap()
            },
            CALLS,
        );
        let (v1, v2) = (bytecode_options(1, 0), bytecode_options(2, realm));
        check_pair(
            &mut store,
            &|store| {
                write(store, BYTECODE, &v1);
                bytecode.call(store, guest_env, BYTECODE, RESULT).unwrap()
            },
            &|store| {
                write(store, BYTECODE, &v2);
                bytecode.call(store, guest_env, BYTECODE, RESULT).unwrap()
            },
            CALLS,
        );
        assert_eq!(
            status(unsafe { snapi_bridge_unofficial_leave_realm(env, scope) }),
            Ok(())
        );
    }
}
