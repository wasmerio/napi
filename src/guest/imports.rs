//! The names of the imports both backends provide besides the N-API functions
//! themselves, for a runtime that asks before it links a module (the WASIX v2
//! hook's `provides_import`).
//!
//! Each backend registers these in its own `register_napi_imports` and
//! `register_env_imports`; `tests::lists_match_what_is_registered` holds the
//! lists to them.

/// The functions of the Wasmer extension namespace
/// ([`NAPI_EXTENSION_WASMER_MODULE_NAME`]) that `register_napi_imports`
/// provides.
///
/// [`NAPI_EXTENSION_WASMER_MODULE_NAME`]: crate::NAPI_EXTENSION_WASMER_MODULE_NAME
pub(crate) fn is_known_extension_import(name: &str) -> bool {
    matches!(
        name,
        "unofficial_napi_configure_runtime"
            | "unofficial_napi_create_env"
            | "unofficial_napi_attach_env"
            | "unofficial_napi_set_env_cleanup_callback"
            | "unofficial_napi_release_env"
            | "unofficial_napi_collect_garbage"
            | "unofficial_napi_memory_pressure"
            | "unofficial_napi_event_loop_checkpoint"
            | "unofficial_napi_create_uninitialized_arraybuffer"
            | "unofficial_napi_acquire_buffer_lease"
            | "unofficial_napi_release_buffer_lease"
            | "unofficial_napi_create_guest_backed_typedarray"
            | "unofficial_napi_create_uint8array"
            | "unofficial_napi_create_uint8array_copy"
            | "unofficial_napi_set_prepare_stack_trace_callback"
            | "unofficial_napi_get_promise_details"
            | "unofficial_napi_get_proxy_details"
            | "unofficial_napi_preview_entries"
            | "unofficial_napi_get_call_sites"
            | "unofficial_napi_arraybuffer_view_has_buffer"
            | "unofficial_napi_get_constructor_name"
            | "unofficial_napi_create_private_symbol"
            | "unofficial_napi_get_continuation_preserved_embedder_data"
            | "unofficial_napi_set_continuation_preserved_embedder_data"
            | "unofficial_napi_notify_datetime_configuration_change"
            | "unofficial_napi_terminate_execution"
            | "unofficial_napi_cancel_terminate_execution"
            | "unofficial_napi_request_interrupt"
            | "unofficial_napi_structured_clone"
            | "unofficial_napi_message_create"
            | "unofficial_napi_message_take"
            | "unofficial_napi_message_drop"
            | "unofficial_napi_create_trusted_script"
            | "unofficial_napi_is_executing_js"
            | "unofficial_napi_enqueue_microtask"
            | "unofficial_napi_set_promise_reject_callback"
            | "unofficial_napi_get_own_non_index_properties"
            | "unofficial_napi_define_lazy_data_properties"
            | "unofficial_napi_is_lazy_data_property"
            | "unofficial_napi_set_promise_hooks"
            | "unofficial_napi_get_hash_seed"
            | "unofficial_napi_get_error_metadata"
            | "unofficial_napi_configure_source_maps"
            | "unofficial_napi_preserve_error_source_message"
            | "unofficial_napi_mark_promise_as_handled"
            | "unofficial_napi_get_heap_statistics"
            | "unofficial_napi_get_heap_space_statistics"
            | "unofficial_napi_get_heap_code_statistics"
            | "unofficial_napi_configure_near_heap_limit_callback"
            | "unofficial_napi_profile_start"
            | "unofficial_napi_profile_stop"
            | "unofficial_napi_take_heap_snapshot"
            | "unofficial_napi_create_serdes_binding"
            | "unofficial_napi_contextify_contains_module_syntax"
            | "unofficial_napi_contextify_make_context"
            | "unofficial_napi_contextify_run_script"
            | "unofficial_napi_contextify_compile_function"
            | "unofficial_napi_create_realm"
            | "unofficial_napi_get_realm"
            | "unofficial_napi_enter_realm"
            | "unofficial_napi_leave_realm"
            | "unofficial_napi_run_script"
            | "unofficial_napi_compile_function"
            | "unofficial_napi_bytecode_open"
            | "unofficial_napi_bytecode_serialize"
            | "unofficial_napi_bytecode_release"
            | "unofficial_napi_module_wrap_create"
            | "unofficial_napi_module_wrap_destroy"
            | "unofficial_napi_module_wrap_link"
            | "unofficial_napi_module_wrap_instantiate"
            | "unofficial_napi_module_wrap_evaluate"
            | "unofficial_napi_module_wrap_evaluate_sync"
            | "unofficial_napi_module_wrap_get_namespace"
            | "unofficial_napi_module_wrap_get_state"
            | "unofficial_napi_module_wrap_check_unsettled_top_level_await"
            | "unofficial_napi_module_wrap_set_export"
            | "unofficial_napi_module_wrap_set_module_source_object"
            | "unofficial_napi_module_wrap_get_module_source_object"
            | "unofficial_napi_module_wrap_create_cached_data"
            | "unofficial_napi_module_wrap_set_hooks"
            | "unofficial_napi_module_wrap_create_required_module_facade"
    )
}

/// The `env` functions that `register_env_imports` provides.
pub(crate) fn is_known_env_import(name: &str) -> bool {
    ENV_IMPORTS.contains(&name)
}

const ENV_IMPORTS: [&str; 7] = [
    "uv_cpu_info",
    "uv_interface_addresses",
    "uv_free_interface_addresses",
    "uv_resident_set_memory",
    "uv_get_free_memory",
    "uv_get_total_memory",
    "_Z20OSSL_set_max_threadsP15ossl_lib_ctx_sty",
];

/// How many names [`is_known_extension_import`] accepts.
#[cfg(test)]
const EXTENSION_IMPORTS: usize = 81;

#[cfg(all(test, not(all(target_arch = "wasm32", feature = "js"))))]
mod tests {
    use super::*;
    use crate::{NAPI_EXTENSION_WASMER_MODULE_NAME, NAPI_MODULE_NAME, NapiCtx};
    use wasmer::{AsStoreMut, Module, Store};

    #[test]
    fn lists_match_what_is_registered() {
        let mut store = Store::default();
        let module = Module::new(
            &store,
            r#"(module
                (import "napi" "napi_get_undefined" (func (param i32 i32) (result i32)))
                (memory (export "memory") 1))"#,
        )
        .unwrap();
        let (imports, _state) = NapiCtx::default()
            .runtime_hooks()
            .additional_imports(&module, &mut store.as_store_mut())
            .unwrap();
        let mut extension = 0;
        let mut env = 0;
        for (namespace, name, _) in imports.iter() {
            assert!(
                NapiCtx::provides_import(namespace, name),
                "{namespace}.{name} is registered but not declared"
            );
            match namespace {
                NAPI_EXTENSION_WASMER_MODULE_NAME => extension += 1,
                "env" => env += 1,
                NAPI_MODULE_NAME => {}
                other => panic!("unexpected namespace {other}"),
            }
        }
        // Nothing is declared that is not registered.
        assert_eq!(env, ENV_IMPORTS.len());
        assert_eq!(extension, EXTENSION_IMPORTS);
    }
}
