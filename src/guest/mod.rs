pub(crate) mod abi;
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub mod callback;
#[cfg(all(target_arch = "wasm32", feature = "js"))]
#[path = "callback_js.rs"]
pub mod callback;
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub mod napi;
#[cfg(all(target_arch = "wasm32", feature = "js"))]
#[path = "napi_js.rs"]
pub mod napi;
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub mod util;
#[cfg(all(target_arch = "wasm32", feature = "js"))]
#[path = "util_js.rs"]
pub mod util;

pub const MAX_GUEST_CSTRING_SCAN: usize = 64 * 1024;

/// Hard ceilings on host scratch buffers sized by a guest-supplied *count*
/// (rather than by data that lives in guest memory). These are rejected before
/// allocating — mirroring the WASIX `MAX_SOCKET_PAYLOAD` write cap — so a bogus
/// count can never allocate gigabytes host-side. Each is ~256 KiB worth, far
/// beyond any legitimate use (no function has 65k args; no real BigInt is a
/// 2-Mbit integer), which keeps the transient uncounted host memory negligible.
pub const MAX_NAPI_CALLBACK_ARGS: usize = 64 * 1024;
pub const MAX_NAPI_BIGINT_WORDS: usize = 32 * 1024;
/// One N-API definition call rarely needs even hundreds of properties. Bound
/// its nine host descriptor arrays and copied names independently of memory
/// limit configuration (which may be unlimited in local tools).
pub const MAX_NAPI_PROPERTY_DESCRIPTORS: usize = 4096;

/// Byte width for the public `napi_typedarray_type` enum. Keep the mapping in
/// one place for both guest backends; values outside the public enum are not
/// alternate engine-specific typed-array kinds.
pub const fn typedarray_element_size(typ: i32) -> Option<usize> {
    match typ {
        0..=2 => Some(1),
        3 | 4 | 11 => Some(2),
        5..=7 => Some(4),
        8..=10 => Some(8),
        _ => None,
    }
}

/// Maximum depth of nested guest↔host callback crossings on one thread. Each
/// crossing piles host native stack frames (Rust bridge + C++ V8) that neither
/// the wasm nor the V8 stack limit bounds, so unbounded reentrancy would SIGSEGV
/// the process uncatchably. Far beyond any legitimate synchronous nesting.
#[cfg(any(test, not(all(target_arch = "wasm32", feature = "js"))))]
pub const MAX_CALLBACK_DEPTH: u32 = 256;

// Both guest backends register exactly these extension and env functions.
// Declaration matching must stay available when `napi` selects the JS backend.
pub(crate) fn is_known_extension_import(name: &str) -> bool {
    matches!(
        name,
        "unofficial_napi_configure_runtime"
            | "unofficial_napi_create_env"
            | "unofficial_napi_attach_env"
            | "unofficial_napi_release_env"
            | "unofficial_napi_collect_garbage"
            | "unofficial_napi_event_loop_checkpoint"
            | "unofficial_napi_create_uninitialized_arraybuffer"
            | "unofficial_napi_acquire_buffer_lease"
            | "unofficial_napi_release_buffer_lease"
            | "unofficial_napi_create_guest_backed_typedarray"
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
            | "unofficial_napi_enqueue_microtask"
            | "unofficial_napi_set_promise_reject_callback"
            | "unofficial_napi_get_own_non_index_properties"
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

pub(crate) fn is_known_env_import(name: &str) -> bool {
    matches!(
        name,
        "uv_cpu_info"
            | "uv_interface_addresses"
            | "uv_free_interface_addresses"
            | "uv_resident_set_memory"
            | "uv_get_free_memory"
            | "uv_get_total_memory"
            | "_Z20OSSL_set_max_threadsP15ossl_lib_ctx_sty"
    )
}
