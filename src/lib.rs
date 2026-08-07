#[cfg(feature = "cli")]
pub mod cli;
mod ctx;
mod env;
mod guest;
mod snapi;
#[cfg(all(target_arch = "wasm32", feature = "js"))]
mod snapi_js;
#[cfg(feature = "wasix")]
mod wasix;
use std::fmt::Display;

pub const NAPI_MODULE_NAME: &str = "napi";
pub const NAPI_EXTENSION_WASMER_MODULE_PREFIX: &str = "napi_extension_wasmer_v";
pub const NAPI_EXTENSION_WASMER_MODULE_NAME: &str = "napi_extension_wasmer_v0";

pub use ctx::{
    NapiCtx, NapiCtxBuilder, NapiInstantiationState, NapiLimits, NapiRuntimeHooks, NapiSession,
};
use enum_iterator::Sequence;
pub(crate) use env::{GuestBackingStoreMapping, HostBufferCopy, NapiEnv};

/// Host capabilities required by the JavaScript-backed N-API implementation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HostJsCapabilities {
    /// Whether the JavaScript engine exposes the JS Promise Integration API.
    pub jspi: bool,
}

/// Reports capabilities of the JavaScript host backend.
///
/// Returns `None` when the crate is not compiled for wasm32 with the `js`
/// feature. Promise-aware EdgeJS entry points must require `jspi == true`.
pub fn host_js_capabilities() -> Option<HostJsCapabilities> {
    #[cfg(all(target_arch = "wasm32", feature = "js"))]
    {
        return Some(HostJsCapabilities {
            jspi: snapi_js::has_jspi(),
        });
    }
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    {
        None
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Sequence)]
pub enum NapiVersion {
    V10,
    Unknown,
}

impl Display for NapiVersion {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            NapiVersion::Unknown => write!(f, "napi_unknown"),
            NapiVersion::V10 => write!(f, "napi_v10"),
        }
    }
}

impl NapiVersion {
    pub const fn is_compatible_with(self, other: Self) -> bool {
        matches!(
            (self, other),
            (Self::V10, Self::V10) | (Self::Unknown, Self::V10)
        )
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NapiWasmerExtensionVersion {
    V0,
    Unknown,
}

impl NapiWasmerExtensionVersion {
    pub const fn is_compatible_with(self, other: Self) -> bool {
        matches!((self, other), (Self::V0, Self::V0))
    }
}

pub fn module_needs_napi(
    module: &wasmer::Module,
) -> (Option<NapiVersion>, Option<NapiWasmerExtensionVersion>) {
    NapiCtx::module_needs_napi(module)
}
