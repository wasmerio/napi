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

/// Per-call ceilings for explicitly budgeted or managed contexts. Standalone
/// contexts use guest-memory/ABI bounds instead of these resource quotas.
pub const MAX_NAPI_CALLBACK_ARGS: usize = 64 * 1024;
pub const MAX_NAPI_BIGINT_WORDS: usize = 32 * 1024;
/// One N-API definition call rarely needs even hundreds of properties. Bound
/// its host descriptor arrays and copied names when resource limits are enabled.
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
pub const MAX_CALLBACK_DEPTH: u32 = 256;
