//! Frozen wasm32 imports used by the released wasmer/edgejs@0.0.1 atom.
//! The fixture is derived from atom SHA-256
//! ca6467e67c8503474cb4204cd2dbbae387c8fa19cad6f6c23131844143e27ccc.

use crate::{
    NAPI_MODULE_NAME, NapiEnv,
    budget::Pool,
    message::{MessageCharge, SERIALIZATION_RESERVATION},
    snapi::{SnapiUnofficialHeapSpaceStatistics, SnapiUnofficialHeapStatistics},
};
use wasmer::{
    AsStoreMut, Function, FunctionEnv, FunctionEnvMut, FunctionType, Imports, RuntimeError, Type,
    Value,
};

const FROZEN_SIGNATURES: &str =
    include_str!("../../../tests/fixtures/edgejs-0.0.1-napi-signatures.txt");

fn wire_type(kind: &str) -> Type {
    match kind {
        "i32" => Type::I32,
        "i64" => Type::I64,
        "f32" => Type::F32,
        "f64" => Type::F64,
        _ => panic!("invalid frozen N-API wire type: {kind}"),
    }
}

fn wire_types(kinds: &str) -> Vec<Type> {
    if kinds == "nil" || kinds.is_empty() {
        Vec::new()
    } else {
        kinds.split(", ").map(wire_type).collect()
    }
}

fn frozen_signatures() -> impl Iterator<Item = (&'static str, FunctionType, bool)> {
    FROZEN_SIGNATURES.lines().filter_map(|line| {
        let (name, signature) = line.strip_prefix("napi.")?.split_once(" (")?;
        if !name.starts_with("unofficial_napi_") {
            return None;
        }
        let (params, results) = signature.split_once(") -> ")?;
        let results = wire_types(results);
        let has_result = !results.is_empty();
        Some((
            name,
            FunctionType::new(wire_types(params), results),
            has_result,
        ))
    })
}

pub(super) fn is_known(name: &str) -> bool {
    FROZEN_SIGNATURES.lines().any(|line| {
        line.strip_prefix("napi.")
            .and_then(|line| line.split_once(' '))
            .is_some_and(|(known, _)| known == name && known.starts_with("unofficial_napi_"))
    })
}

pub(super) fn frozen_type_matches(name: &str, actual: &FunctionType) -> Option<bool> {
    FROZEN_SIGNATURES.lines().find_map(|line| {
        let (known, signature) = line.strip_prefix("napi.")?.split_once(" (")?;
        if known != name {
            return None;
        }
        let (params, results) = signature.split_once(") -> ")?;
        let expected = FunctionType::new(wire_types(params), wire_types(results));
        Some(&expected == actual)
    })
}

pub(super) fn register(store: &mut impl AsStoreMut, fe: &FunctionEnv<NapiEnv>, io: &mut Imports) {
    for (name, ty, has_result) in frozen_signatures() {
        let function = Function::new_with_env(store, fe, ty, move |env, args| {
            let status = dispatch(name, env, args)?;
            Ok(if has_result {
                vec![Value::I32(status)]
            } else {
                Vec::new()
            })
        });
        io.define(NAPI_MODULE_NAME, name, function);
    }
}

fn dispatch(name: &str, env: FunctionEnvMut<NapiEnv>, args: &[Value]) -> Result<i32, RuntimeError> {
    // Control shutdown is sticky across cloned WASIX worker import sets.
    // Release is still available so an instance can tear down admitted envs.
    if env.data().host_stopped()
        && name != "unofficial_napi_release_env"
        && name != "unofficial_napi_release_env_with_loop"
        && name != "unofficial_napi_release_serialized_value"
    {
        return Ok(1);
    }
    match name {
        "unofficial_napi_set_flags_from_string" => {
            // The released CLI passes these two fixed defaults before it
            // creates an env. Read a bounded guest slice, but never forward
            // guest flags into the process-wide V8 runtime.
            let length = args[1].unwrap_i32();
            if !(0..=64).contains(&length) {
                return Ok(1);
            }
            if length == 0 {
                return Ok(0);
            }
            let mut env = env;
            let pointer = args[0].unwrap_i32();
            if pointer <= 0 {
                return Ok(1);
            }
            let Some(bytes) = super::read_guest_bytes(&mut env, pointer, length as usize) else {
                return Ok(1);
            };
            Ok(i32::from(!legacy_default_flags(bytes.as_slice())))
        }
        "unofficial_napi_serialize_value" => legacy_serialize_value(env, args),
        "unofficial_napi_deserialize_value" => legacy_deserialize_value(env, args),
        "unofficial_napi_release_serialized_value" => {
            let id = args[0].unwrap_i32();
            if id == 0 {
                return Ok(0);
            }
            if id < 0 {
                return Err(RuntimeError::new("invalid legacy serialized payload"));
            }
            let Some(_charge) = env.data().pending_messages.take_legacy(id as u32) else {
                return Err(RuntimeError::new("invalid legacy serialized payload"));
            };
            unsafe { crate::snapi::snapi_bridge_unofficial_message_drop(id as u32) };
            Ok(0)
        }
        "unofficial_napi_create_env" => Ok(super::guest_unofficial_napi_create_env(
            env,
            args[0].unwrap_i32(),
            0,
            args[1].unwrap_i32(),
            args[2].unwrap_i32(),
        )),
        "unofficial_napi_create_env_with_options" => {
            let mut env = env;
            let options_ptr = args[1].unwrap_i32();
            let options = if options_ptr == 0 {
                super::abi::EnvCreate::default()
            } else if options_ptr > 0 {
                let Some(options) = super::abi::read_legacy_env_create(&mut env, options_ptr)
                else {
                    return Ok(1);
                };
                options
            } else {
                return Ok(1);
            };
            Ok(super::create_env_with_options(
                env,
                args[0].unwrap_i32(),
                options,
                args[2].unwrap_i32(),
                args[3].unwrap_i32(),
            ))
        }
        "unofficial_napi_release_env" => {
            super::guest_unofficial_napi_release_env(env, args[0].unwrap_i32(), 0)
                .map_err(|error| RuntimeError::user(Box::new(error)))
        }
        "unofficial_napi_release_env_with_loop" => super::guest_unofficial_napi_release_env(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
        )
        .map_err(|error| RuntimeError::user(Box::new(error))),
        "unofficial_napi_set_enqueue_foreground_task_callback" => {
            Ok(legacy_set_foreground_task_callback(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
            ))
        }
        "unofficial_napi_set_fatal_error_callbacks" => Ok(legacy_set_fatal_error_callbacks(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
            args[2].unwrap_i32(),
        )),
        "unofficial_napi_arraybuffer_view_has_buffer" => {
            Ok(super::guest_unofficial_napi_arraybuffer_view_has_buffer(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
            ))
        }
        "unofficial_napi_cancel_terminate_execution" => Ok(
            super::guest_unofficial_napi_cancel_terminate_execution(env, args[0].unwrap_i32()),
        ),
        "unofficial_napi_contextify_contains_module_syntax" => Ok(
            super::guest_unofficial_napi_contextify_contains_module_syntax(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
                args[3].unwrap_i32(),
                args[4].unwrap_i32(),
                args[5].unwrap_i32(),
            ),
        ),
        "unofficial_napi_contextify_compile_function" => Ok(legacy_compile_function(env, args)),
        "unofficial_napi_contextify_compile_function_for_cjs_loader" => {
            legacy_compile_cjs(env, args)
        }
        "unofficial_napi_contextify_create_cached_data" => legacy_create_cached_data(env, args),
        "unofficial_napi_contextify_make_context" => {
            Ok(super::guest_unofficial_napi_contextify_make_context(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
                args[3].unwrap_i32(),
                args[4].unwrap_i32(),
                args[5].unwrap_i32(),
                args[6].unwrap_i32(),
                args[7].unwrap_i32(),
                args[8].unwrap_i32(),
            ))
        }
        "unofficial_napi_contextify_run_script" => legacy_run_script(env, args),
        "unofficial_napi_create_private_symbol" => {
            Ok(super::guest_unofficial_napi_create_private_symbol(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
                args[3].unwrap_i32(),
            ))
        }
        "unofficial_napi_get_call_sites" => Ok(super::guest_unofficial_napi_get_call_sites(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
            args[2].unwrap_i32(),
        )),
        "unofficial_napi_get_constructor_name" => {
            Ok(super::guest_unofficial_napi_get_constructor_name(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
            ))
        }
        "unofficial_napi_get_continuation_preserved_embedder_data" => Ok(
            super::guest_unofficial_napi_get_continuation_preserved_embedder_data(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
            ),
        ),
        "unofficial_napi_get_hash_seed" => Ok(super::guest_unofficial_napi_get_hash_seed(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
        )),
        "unofficial_napi_get_heap_code_statistics" => {
            Ok(super::guest_unofficial_napi_get_heap_code_statistics(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
            ))
        }
        "unofficial_napi_get_heap_statistics" => Ok(legacy_heap_statistics(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
        )),
        "unofficial_napi_get_heap_space_count" => Ok(legacy_heap_space_count(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
        )),
        "unofficial_napi_get_heap_space_statistics" => Ok(legacy_heap_space_statistics(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
            args[2].unwrap_i32(),
        )),
        "unofficial_napi_get_process_memory_info" => Ok(legacy_process_memory_info(env, args)),
        "unofficial_napi_get_own_non_index_properties" => {
            Ok(super::guest_unofficial_napi_get_own_non_index_properties(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
                args[3].unwrap_i32(),
            ))
        }
        "unofficial_napi_get_promise_details" => {
            Ok(super::guest_unofficial_napi_get_promise_details(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
                args[3].unwrap_i32(),
                args[4].unwrap_i32(),
            ))
        }
        "unofficial_napi_get_proxy_details" => Ok(super::guest_unofficial_napi_get_proxy_details(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
            args[2].unwrap_i32(),
            args[3].unwrap_i32(),
        )),
        "unofficial_napi_mark_promise_as_handled" => {
            Ok(super::guest_unofficial_napi_mark_promise_as_handled(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
            ))
        }
        "unofficial_napi_module_wrap_create_cached_data" => {
            Ok(super::guest_unofficial_napi_module_wrap_create_cached_data(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
            ))
        }
        "unofficial_napi_module_wrap_create_source_text" => legacy_module_create(env, args, false),
        "unofficial_napi_module_wrap_create_synthetic" => legacy_module_create(env, args, true),
        "unofficial_napi_module_wrap_create_required_module_facade" => {
            super::guest_unofficial_napi_module_wrap_create_required_module_facade(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
            )
            .map_err(|error| RuntimeError::user(Box::new(error)))
        }
        "unofficial_napi_module_wrap_destroy" => {
            Ok(super::guest_unofficial_napi_module_wrap_destroy(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
            ))
        }
        "unofficial_napi_module_wrap_get_module_source_object" => Ok(
            super::guest_unofficial_napi_module_wrap_get_module_source_object(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
            ),
        ),
        "unofficial_napi_module_wrap_get_namespace" => {
            Ok(super::guest_unofficial_napi_module_wrap_get_namespace(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
            ))
        }
        "unofficial_napi_module_wrap_get_module_requests" => {
            Ok(legacy_module_metadata(env, args, true))
        }
        "unofficial_napi_module_wrap_has_top_level_await" => {
            Ok(legacy_module_metadata(env, args, false))
        }
        "unofficial_napi_module_wrap_get_status" => {
            Ok(super::guest_unofficial_napi_module_wrap_get_state(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
                0,
                0,
            ))
        }
        "unofficial_napi_module_wrap_get_error" => {
            Ok(super::guest_unofficial_napi_module_wrap_get_state(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                0,
                args[2].unwrap_i32(),
                0,
            ))
        }
        "unofficial_napi_module_wrap_has_async_graph" => {
            Ok(super::guest_unofficial_napi_module_wrap_get_state(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                0,
                0,
                args[2].unwrap_i32(),
            ))
        }
        "unofficial_napi_module_wrap_check_unsettled_top_level_await" => Ok(
            super::guest_unofficial_napi_module_wrap_check_unsettled_top_level_await(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
                args[3].unwrap_i32(),
            ),
        ),
        "unofficial_napi_module_wrap_evaluate" => {
            super::guest_unofficial_napi_module_wrap_evaluate(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i64(),
                args[3].unwrap_i32(),
                args[4].unwrap_i32(),
            )
            .map_err(|error| RuntimeError::user(Box::new(error)))
        }
        "unofficial_napi_module_wrap_evaluate_sync" => {
            super::guest_unofficial_napi_module_wrap_evaluate_sync(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
                args[3].unwrap_i32(),
                args[4].unwrap_i32(),
            )
            .map_err(|error| RuntimeError::user(Box::new(error)))
        }
        "unofficial_napi_module_wrap_import_module_dynamically" => {
            legacy_import_module_dynamically(env, args)
        }
        "unofficial_napi_module_wrap_instantiate" => {
            Ok(super::guest_unofficial_napi_module_wrap_instantiate(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
            ))
        }
        "unofficial_napi_module_wrap_link" => Ok(super::guest_unofficial_napi_module_wrap_link(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
            args[2].unwrap_i32(),
            args[3].unwrap_i32(),
        )),
        "unofficial_napi_module_wrap_set_export" => {
            Ok(super::guest_unofficial_napi_module_wrap_set_export(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
                args[3].unwrap_i32(),
            ))
        }
        "unofficial_napi_module_wrap_set_import_module_dynamically_callback" => Ok(
            legacy_module_hook(env, args[0].unwrap_i32(), args[1].unwrap_i32(), 0),
        ),
        "unofficial_napi_module_wrap_set_initialize_import_meta_object_callback" => Ok(
            legacy_module_hook(env, args[0].unwrap_i32(), args[1].unwrap_i32(), 1),
        ),
        "unofficial_napi_module_wrap_set_module_source_object" => Ok(
            super::guest_unofficial_napi_module_wrap_set_module_source_object(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
            ),
        ),
        "unofficial_napi_notify_datetime_configuration_change" => Ok(
            super::guest_unofficial_napi_notify_datetime_configuration_change(
                env,
                args[0].unwrap_i32(),
            ),
        ),
        "unofficial_napi_preserve_error_source_message" => {
            Ok(super::guest_unofficial_napi_preserve_error_source_message(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
            ))
        }
        "unofficial_napi_process_microtasks" => {
            super::guest_unofficial_napi_event_loop_checkpoint(env, args[0].unwrap_i32(), 0, 0, 0)
                .map_err(|error| RuntimeError::user(Box::new(error)))
        }
        "unofficial_napi_preview_entries" => Ok(super::guest_unofficial_napi_preview_entries(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
            args[2].unwrap_i32(),
            args[3].unwrap_i32(),
        )),
        "unofficial_napi_request_interrupt" => Ok(super::guest_unofficial_napi_request_interrupt(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
            args[2].unwrap_i32(),
        )),
        "unofficial_napi_low_memory_notification" | "unofficial_napi_request_gc_for_testing" => {
            let mut env = env;
            let guest_env = args[0].unwrap_i32();
            let snapi_env = super::snapi_env(&env, guest_env);
            if snapi_env.is_null() {
                return Ok(1);
            }
            super::with_cb_context(&mut env, guest_env, || unsafe {
                crate::snapi::snapi_bridge_unofficial_collect_garbage(snapi_env)
            })
            .map_err(|error| RuntimeError::user(Box::new(error)))
        }
        "unofficial_napi_set_near_heap_limit_callback"
        | "unofficial_napi_remove_near_heap_limit_callback"
        | "unofficial_napi_set_stack_limit" => {
            // A wasm32 stack pointer is not a native V8 stack limit. The
            // provider owns this isolate's heap limit and stop control.
            Ok(i32::from(
                super::snapi_env(&env, args[0].unwrap_i32()).is_null(),
            ))
        }
        "unofficial_napi_structured_clone" => Ok(super::guest_unofficial_napi_structured_clone(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
            0,
            args[2].unwrap_i32(),
        )),
        "unofficial_napi_structured_clone_with_transfer" => {
            Ok(super::guest_unofficial_napi_structured_clone(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
                args[2].unwrap_i32(),
                args[3].unwrap_i32(),
            ))
        }
        "unofficial_napi_set_continuation_preserved_embedder_data" => Ok(
            super::guest_unofficial_napi_set_continuation_preserved_embedder_data(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
            ),
        ),
        "unofficial_napi_set_prepare_stack_trace_callback" => Ok(
            super::guest_unofficial_napi_set_prepare_stack_trace_callback(
                env,
                args[0].unwrap_i32(),
                args[1].unwrap_i32(),
            ),
        ),
        "unofficial_napi_set_promise_hooks" => Ok(super::guest_unofficial_napi_set_promise_hooks(
            env,
            args[0].unwrap_i32(),
            args[1].unwrap_i32(),
            args[2].unwrap_i32(),
            args[3].unwrap_i32(),
            args[4].unwrap_i32(),
        )),
        "unofficial_napi_terminate_execution" => Ok(
            super::guest_unofficial_napi_terminate_execution(env, args[0].unwrap_i32()),
        ),
        // Every released import has an exact type and a host function. An
        // operation with no safe compatibility path reports failure rather
        // than pretending that an irreversible state transition succeeded.
        // This is a wasm32 address returned by a guest-side copy. Native
        // free() must never see it; its guest allocator owns the memory.
        "unofficial_napi_free_buffer" => Ok(0),
        "unofficial_napi_create_serdes_binding"
        | "unofficial_napi_get_caller_location"
        | "unofficial_napi_get_current_stack_trace"
        | "unofficial_napi_get_error_source_positions"
        | "unofficial_napi_start_cpu_profile"
        | "unofficial_napi_start_heap_profile"
        | "unofficial_napi_stop_cpu_profile"
        | "unofficial_napi_stop_heap_profile"
        | "unofficial_napi_take_heap_snapshot" => Ok(1),
        _ => Err(RuntimeError::new(format!(
            "unclassified frozen N-API import: {name}"
        ))),
    }
}

fn legacy_set_foreground_task_callback(
    mut env: FunctionEnvMut<NapiEnv>,
    guest_env: i32,
    callback: i32,
    target: i32,
) -> i32 {
    if guest_env <= 0 || super::snapi_env(&env, guest_env).is_null() {
        return 1;
    }
    if callback == 0 && target == 0 {
        return 0;
    }
    if callback <= 0 || target <= 0 || super::read_guest_bytes(&mut env, target, 1).is_none() {
        return 1;
    }
    let Some(table) = env.data().table.clone() else {
        return 1;
    };
    let mut store = env.as_store_mut();
    let Some(Value::FuncRef(Some(function))) = table.get(&mut store, callback as u32) else {
        return 1;
    };
    let expected = FunctionType::new(
        [Type::I32, Type::I32, Type::I32, Type::I32, Type::I64],
        [Type::I32],
    );
    if function.ty(&store) != expected {
        return 1;
    }
    // The guest callback is not installed. V8's fallback foreground queue is
    // drained when this environment reaches an event-loop checkpoint; queued
    // tasks remain pending if the guest does not drive that checkpoint.
    0
}

fn legacy_module_create(
    mut env: FunctionEnvMut<NapiEnv>,
    args: &[Value],
    synthetic: bool,
) -> Result<i32, RuntimeError> {
    let guest_env = args[0].unwrap_i32();
    let result_ptr = args[if synthetic { 6 } else { 8 }].unwrap_i32();
    if guest_env <= 0
        || result_ptr <= 0
        || super::read_guest_bytes(&mut env, result_ptr, 4).is_none()
    {
        return Ok(1);
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() {
        return Ok(1);
    }
    // The released source-text entry accepts either a host-option symbol or
    // cached-data bytes. Preserve the symbol and compile from source without
    // retaining guest cache bytes.
    let mut handle = 0u32;
    let status = super::with_cb_context(&mut env, guest_env, || unsafe {
        crate::snapi::snapi_bridge_unofficial_module_wrap_create_legacy(
            snapi_env,
            if synthetic { 2 } else { 1 },
            args[1].unwrap_i32().max(0) as u32,
            args[2].unwrap_i32().max(0) as u32,
            args[3].unwrap_i32().max(0) as u32,
            if synthetic {
                0
            } else {
                args[4].unwrap_i32().max(0) as u32
            },
            if synthetic { 0 } else { args[5].unwrap_i32() },
            if synthetic { 0 } else { args[6].unwrap_i32() },
            if synthetic {
                0
            } else {
                args[7].unwrap_i32().max(0) as u32
            },
            if synthetic {
                args[4].unwrap_i32().max(0) as u32
            } else {
                0
            },
            if synthetic {
                args[5].unwrap_i32().max(0) as u32
            } else {
                0
            },
            &mut handle,
        )
    })
    .map_err(|error| RuntimeError::user(Box::new(error)))?;
    if status == 0 && !super::write_guest_u32(&mut env, result_ptr as u32, handle) {
        unsafe { crate::snapi::snapi_bridge_unofficial_module_wrap_destroy(snapi_env, handle) };
        return Ok(1);
    }
    Ok(status)
}

fn legacy_module_metadata(mut env: FunctionEnvMut<NapiEnv>, args: &[Value], requests: bool) -> i32 {
    let guest_env = args[0].unwrap_i32();
    let handle = args[1].unwrap_i32();
    let result_ptr = args[2].unwrap_i32();
    if guest_env <= 0
        || handle <= 0
        || result_ptr <= 0
        || super::read_guest_bytes(&mut env, result_ptr, if requests { 4 } else { 1 }).is_none()
    {
        return 1;
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() {
        return 1;
    }
    let mut result_id = 0u32;
    let mut has_tla = 0i32;
    let status = unsafe {
        crate::snapi::snapi_bridge_unofficial_module_wrap_get_legacy_metadata(
            snapi_env,
            handle as u32,
            if requests {
                &mut result_id
            } else {
                std::ptr::null_mut()
            },
            if requests {
                std::ptr::null_mut()
            } else {
                &mut has_tla
            },
        )
    };
    if status != 0 {
        return status;
    }
    if requests {
        i32::from(!super::write_guest_u32(
            &mut env,
            result_ptr as u32,
            result_id,
        ))
    } else {
        i32::from(!super::write_guest_u8(
            &mut env,
            result_ptr as u32,
            u8::from(has_tla != 0),
        ))
    }
}

fn legacy_import_module_dynamically(
    mut env: FunctionEnvMut<NapiEnv>,
    args: &[Value],
) -> Result<i32, RuntimeError> {
    let guest_env = args[0].unwrap_i32();
    let argc = args[1].unwrap_i32();
    let result_ptr = args[3].unwrap_i32();
    if guest_env <= 0
        || !(1..=5).contains(&argc)
        || result_ptr <= 0
        || super::read_guest_bytes(&mut env, result_ptr, 4).is_none()
    {
        return Ok(1);
    }
    let Some(ids) = super::read_guest_u32_array(&mut env, args[2].unwrap_i32(), argc as usize)
    else {
        return Ok(1);
    };
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() {
        return Ok(1);
    }
    let mut result = 0u32;
    let status = super::with_cb_context(&mut env, guest_env, || unsafe {
        crate::snapi::snapi_bridge_unofficial_module_wrap_import_module_dynamically_legacy(
            snapi_env,
            argc as u32,
            ids.as_ptr(),
            &mut result,
        )
    })
    .map_err(|error| RuntimeError::user(Box::new(error)))?;
    if status == 0 && !super::write_guest_u32(&mut env, result_ptr as u32, result) {
        return Ok(1);
    }
    Ok(status)
}

fn legacy_heap_statistics(
    mut env: FunctionEnvMut<NapiEnv>,
    guest_env: i32,
    result_ptr: i32,
) -> i32 {
    const FIELDS: usize = 14;
    if guest_env <= 0
        || result_ptr <= 0
        || super::read_guest_bytes(&mut env, result_ptr, FIELDS * 8).is_none()
    {
        return 1;
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() {
        return 1;
    }
    let mut stats = SnapiUnofficialHeapStatistics {
        size: std::mem::size_of::<SnapiUnofficialHeapStatistics>() as u32,
        version: 1,
        ..Default::default()
    };
    let status =
        unsafe { crate::snapi::snapi_bridge_unofficial_get_heap_statistics(snapi_env, &mut stats) };
    if status != 0 {
        return status;
    }
    let values = [
        stats.total_heap_size,
        stats.total_heap_size_executable,
        stats.total_physical_size,
        stats.total_available_size,
        stats.used_heap_size,
        stats.heap_size_limit,
        stats.does_zap_garbage,
        stats.malloced_memory,
        stats.peak_malloced_memory,
        stats.number_of_native_contexts,
        stats.number_of_detached_contexts,
        stats.total_global_handles_size,
        stats.used_global_handles_size,
        stats.external_memory,
    ];
    let mut normalized = [0u64; FIELDS];
    for (index, value) in values.into_iter().enumerate() {
        if stats.valid_fields & (1u64 << index) != 0 {
            normalized[index] = value;
        }
    }
    i32::from(!super::write_guest_pod(&mut env, result_ptr, &normalized))
}

fn legacy_heap_space_count(
    mut env: FunctionEnvMut<NapiEnv>,
    guest_env: i32,
    result_ptr: i32,
) -> i32 {
    if guest_env <= 0
        || result_ptr <= 0
        || super::read_guest_bytes(&mut env, result_ptr, 4).is_none()
    {
        return 1;
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() {
        return 1;
    }
    let mut count = 0u32;
    let status = unsafe {
        crate::snapi::snapi_bridge_unofficial_get_heap_space_statistics(
            snapi_env,
            std::ptr::null_mut(),
            0,
            &mut count,
        )
    };
    if status == 0 && !super::write_guest_u32(&mut env, result_ptr as u32, count) {
        return 1;
    }
    status
}

fn legacy_heap_space_statistics(
    mut env: FunctionEnvMut<NapiEnv>,
    guest_env: i32,
    index: i32,
    result_ptr: i32,
) -> i32 {
    // Keep the same bounded maximum as the versioned heap-space adapter.
    if guest_env <= 0
        || !(0..1024).contains(&index)
        || result_ptr <= 0
        || super::read_guest_bytes(
            &mut env,
            result_ptr,
            std::mem::size_of::<SnapiUnofficialHeapSpaceStatistics>(),
        )
        .is_none()
    {
        return 1;
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() {
        return 1;
    }
    let capacity = index as usize + 1;
    let mut stats: Vec<SnapiUnofficialHeapSpaceStatistics> =
        (0..capacity).map(|_| Default::default()).collect();
    let mut count = 0u32;
    let status = unsafe {
        crate::snapi::snapi_bridge_unofficial_get_heap_space_statistics(
            snapi_env,
            stats.as_mut_ptr(),
            capacity as u32,
            &mut count,
        )
    };
    if status != 0 {
        return status;
    }
    if index as u32 >= count {
        return 1;
    }
    i32::from(!super::write_guest_pod(
        &mut env,
        result_ptr,
        &stats[index as usize],
    ))
}

fn legacy_process_memory_info(mut env: FunctionEnvMut<NapiEnv>, args: &[Value]) -> i32 {
    let guest_env = args[0].unwrap_i32();
    if guest_env <= 0 {
        return 1;
    }
    for arg in &args[1..] {
        let pointer = arg.unwrap_i32();
        if pointer < 0 || (pointer > 0 && super::read_guest_bytes(&mut env, pointer, 8).is_none()) {
            return 1;
        }
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() {
        return 1;
    }
    let mut stats = SnapiUnofficialHeapStatistics {
        size: std::mem::size_of::<SnapiUnofficialHeapStatistics>() as u32,
        version: 1,
        ..Default::default()
    };
    let status =
        unsafe { crate::snapi::snapi_bridge_unofficial_get_heap_statistics(snapi_env, &mut stats) };
    if status != 0 {
        return status;
    }
    let values = [
        (0, stats.total_heap_size),
        (4, stats.used_heap_size),
        (13, stats.external_memory),
        (14, stats.array_buffer_memory),
    ];
    for (arg, (bit, value)) in args[1..].iter().zip(values) {
        let pointer = arg.unwrap_i32();
        if pointer > 0
            && (stats.valid_fields & (1u64 << bit) == 0
                || !super::write_guest_pod(&mut env, pointer, &(value as f64)))
        {
            return 1;
        }
    }
    0
}

fn legacy_set_fatal_error_callbacks(
    mut env: FunctionEnvMut<NapiEnv>,
    guest_env: i32,
    fatal: i32,
    oom: i32,
) -> i32 {
    if guest_env <= 0 || fatal <= 0 || oom <= 0 {
        return 1;
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() {
        return 1;
    }
    let Some(table) = env.data().table.clone() else {
        return 1;
    };
    let mut store = env.as_store_mut();
    for (index, expected) in [
        (fatal, FunctionType::new([Type::I32; 3], [])),
        (oom, FunctionType::new([Type::I32; 4], [])),
    ] {
        let Some(Value::FuncRef(Some(function))) = table.get(&mut store, index as u32) else {
            return 1;
        };
        if function.ty(&store) != expected {
            return 1;
        }
    }
    // The published guest's callbacks abort the process. A provider hook
    // records a workload-local stop if V8 returns to the import boundary.
    // Native fatal errors can still abort inside V8 before control returns.
    unsafe { crate::snapi::snapi_bridge_unofficial_attach_legacy_env(snapi_env) }
}

fn legacy_compile_function(mut env: FunctionEnvMut<NapiEnv>, args: &[Value]) -> i32 {
    let guest_env = args[0].unwrap_i32();
    let result_ptr = args[11].unwrap_i32();
    if guest_env <= 0 || result_ptr <= 0 {
        return 1;
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() || super::read_guest_bytes(&mut env, result_ptr, 4).is_none() {
        return 1;
    }
    let mut result = 0;
    let status = unsafe {
        crate::snapi::snapi_bridge_unofficial_contextify_compile_function_legacy(
            snapi_env,
            args[1].unwrap_i32().max(0) as u32,
            args[2].unwrap_i32().max(0) as u32,
            args[3].unwrap_i32(),
            args[4].unwrap_i32(),
            args[5].unwrap_i32().max(0) as u32,
            args[6].unwrap_i32(),
            args[7].unwrap_i32().max(0) as u32,
            args[8].unwrap_i32().max(0) as u32,
            args[9].unwrap_i32().max(0) as u32,
            args[10].unwrap_i32().max(0) as u32,
            &mut result,
        )
    };
    if status == 0 && !super::write_guest_u32(&mut env, result_ptr as u32, result) {
        return 1;
    }
    status
}

fn legacy_compile_cjs(
    mut env: FunctionEnvMut<NapiEnv>,
    args: &[Value],
) -> Result<i32, RuntimeError> {
    let guest_env = args[0].unwrap_i32();
    let result_ptr = args[5].unwrap_i32();
    if guest_env <= 0 || result_ptr <= 0 {
        return Ok(1);
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() || super::read_guest_bytes(&mut env, result_ptr, 4).is_none() {
        return Ok(1);
    }
    let mut result = 0;
    let status = super::with_cb_context(&mut env, guest_env, || unsafe {
        crate::snapi::snapi_bridge_unofficial_contextify_compile_cjs_legacy(
            snapi_env,
            args[1].unwrap_i32().max(0) as u32,
            args[2].unwrap_i32().max(0) as u32,
            args[3].unwrap_i32(),
            args[4].unwrap_i32(),
            &mut result,
        )
    })
    .map_err(|error| RuntimeError::user(Box::new(error)))?;
    if status == 0 && !super::write_guest_u32(&mut env, result_ptr as u32, result) {
        return Ok(1);
    }
    Ok(status)
}

fn legacy_run_script(
    mut env: FunctionEnvMut<NapiEnv>,
    args: &[Value],
) -> Result<i32, RuntimeError> {
    let guest_env = args[0].unwrap_i32();
    let result_ptr = args[11].unwrap_i32();
    if guest_env <= 0 || result_ptr <= 0 || args[6].unwrap_i64() != -1 || args[8].unwrap_i32() != 0
    {
        return Ok(1);
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() || super::read_guest_bytes(&mut env, result_ptr, 4).is_none() {
        return Ok(1);
    }
    let mut result = 0;
    let status = super::with_cb_context(&mut env, guest_env, || unsafe {
        crate::snapi::snapi_bridge_unofficial_contextify_run_script(
            snapi_env,
            args[1].unwrap_i32().max(0) as u32,
            args[2].unwrap_i32().max(0) as u32,
            0,
            args[3].unwrap_i32().max(0) as u32,
            args[4].unwrap_i32(),
            args[5].unwrap_i32(),
            args[6].unwrap_i64(),
            args[7].unwrap_i32(),
            args[8].unwrap_i32(),
            args[9].unwrap_i32(),
            args[10].unwrap_i32().max(0) as u32,
            &mut result,
        )
    })
    .map_err(|error| RuntimeError::user(Box::new(error)))?;
    if status == 0 && !super::write_guest_u32(&mut env, result_ptr as u32, result) {
        return Ok(1);
    }
    Ok(status)
}

fn legacy_create_cached_data(
    mut env: FunctionEnvMut<NapiEnv>,
    args: &[Value],
) -> Result<i32, RuntimeError> {
    const TRANSIENT_RESERVATION: u64 = 16 * 1024 * 1024;
    let guest_env = args[0].unwrap_i32();
    let result_ptr = args[6].unwrap_i32();
    if guest_env <= 0 || result_ptr <= 0 {
        return Ok(1);
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() || super::read_guest_bytes(&mut env, result_ptr, 4).is_none() {
        return Ok(1);
    }
    let budget = env.data().budget.clone();
    if budget
        .try_charge(Pool::HostTransient, TRANSIENT_RESERVATION)
        .is_err()
    {
        return Ok(1);
    }
    let mut result = 0;
    let status = super::with_cb_context(&mut env, guest_env, || unsafe {
        crate::snapi::snapi_bridge_unofficial_contextify_create_cached_data_legacy(
            snapi_env,
            args[1].unwrap_i32().max(0) as u32,
            args[2].unwrap_i32().max(0) as u32,
            args[3].unwrap_i32(),
            args[4].unwrap_i32(),
            args[5].unwrap_i32().max(0) as u32,
            &mut result,
        )
    });
    budget.uncharge(Pool::HostTransient, TRANSIENT_RESERVATION);
    let status = status.map_err(|error| RuntimeError::user(Box::new(error)))?;
    if status == 0 && !super::write_guest_u32(&mut env, result_ptr as u32, result) {
        return Ok(1);
    }
    Ok(status)
}

fn legacy_module_hook(
    env: FunctionEnvMut<NapiEnv>,
    guest_env: i32,
    callback: i32,
    kind: i32,
) -> i32 {
    if guest_env <= 0 || callback < 0 {
        return 1;
    }
    unsafe {
        crate::snapi::snapi_bridge_unofficial_module_wrap_set_legacy_hook(
            super::snapi_env(&env, guest_env),
            callback as u32,
            kind,
        )
    }
}

fn legacy_serialize_value(
    mut env: FunctionEnvMut<NapiEnv>,
    args: &[Value],
) -> Result<i32, RuntimeError> {
    let guest_env = args[0].unwrap_i32();
    let value = args[1].unwrap_i32();
    let result_ptr = args[2].unwrap_i32();
    if guest_env <= 0 || value <= 0 || result_ptr <= 0 {
        return Ok(1);
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() || super::read_guest_bytes(&mut env, result_ptr, 4).is_none() {
        return Ok(1);
    }
    let pending = env.data().pending_messages.clone();
    let Ok(mut charge) =
        MessageCharge::reserve(env.data().budget.clone(), SERIALIZATION_RESERVATION)
    else {
        return Ok(1);
    };
    let mut message = 0;
    let mut retained_bytes = 0;
    let status = super::with_cb_context(&mut env, guest_env, || unsafe {
        crate::snapi::snapi_bridge_unofficial_message_create_legacy_metered(
            snapi_env,
            value as u32,
            &mut message,
            &mut retained_bytes,
        )
    });
    let status = match status {
        Ok(status) => status,
        Err(error) => {
            if message != 0 {
                unsafe { crate::snapi::snapi_bridge_unofficial_message_drop(message) };
            }
            return Err(RuntimeError::user(Box::new(error)));
        }
    };
    if status != 0 {
        return Ok(status);
    }
    if !charge.shrink(retained_bytes) || pending.insert_legacy(message, charge).is_err() {
        unsafe { crate::snapi::snapi_bridge_unofficial_message_drop(message) };
        return Ok(1);
    }
    if !super::write_guest_u32(&mut env, result_ptr as u32, message) {
        drop(pending.take_legacy(message));
        unsafe { crate::snapi::snapi_bridge_unofficial_message_drop(message) };
        return Ok(1);
    }
    Ok(0)
}

fn legacy_deserialize_value(
    mut env: FunctionEnvMut<NapiEnv>,
    args: &[Value],
) -> Result<i32, RuntimeError> {
    let guest_env = args[0].unwrap_i32();
    let message = args[1].unwrap_i32();
    let result_ptr = args[2].unwrap_i32();
    if guest_env <= 0 || message <= 0 || result_ptr <= 0 {
        return Ok(1);
    }
    let snapi_env = super::snapi_env(&env, guest_env);
    if snapi_env.is_null() || super::read_guest_bytes(&mut env, result_ptr, 4).is_none() {
        return Ok(1);
    }
    // A concurrent explicit release removes the registry entry, while this
    // lease keeps the app charge until the native reader has finished.
    let Some(_charge) = env.data().pending_messages.lease_legacy(message as u32) else {
        return Ok(1);
    };
    let mut value = 0;
    let status = super::with_cb_context(&mut env, guest_env, || unsafe {
        crate::snapi::snapi_bridge_unofficial_message_read_legacy(
            snapi_env,
            message as u32,
            &mut value,
        )
    })
    .map_err(|error| RuntimeError::user(Box::new(error)))?;
    if status == 0 && !super::write_guest_u32(&mut env, result_ptr as u32, value) {
        return Ok(1);
    }
    Ok(status)
}

fn legacy_default_flags(bytes: &[u8]) -> bool {
    matches!(
        bytes,
        b"" | b"--js-source-phase-imports"
            | b"--harmony-import-attributes"
            | b"--js-source-phase-imports --harmony-import-attributes"
    )
}

#[cfg(test)]
mod tests {
    use super::legacy_default_flags;

    #[test]
    fn only_released_edgejs_default_flags_are_inert() {
        assert!(legacy_default_flags(
            b"--js-source-phase-imports --harmony-import-attributes"
        ));
        assert!(legacy_default_flags(b"--js-source-phase-imports"));
        assert!(legacy_default_flags(b"--harmony-import-attributes"));
        assert!(legacy_default_flags(b""));
        assert!(!legacy_default_flags(b"--allow-natives-syntax"));
        assert!(!legacy_default_flags(
            b"--js-source-phase-imports --allow-natives-syntax"
        ));
        assert!(!legacy_default_flags(
            b"--harmony-import-attributes --js-source-phase-imports"
        ));
    }
}
