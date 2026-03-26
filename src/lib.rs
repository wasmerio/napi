#[cfg(feature = "cli")]
pub mod cli;
mod ctx;
mod env;
mod guest;
mod snapi;

pub(crate) const NAPI_MODULE_NAME: &str = "napi";
pub(crate) const NAPI_EXTENSION_WASMER_MODULE_PREFIX: &str = "napi_extension_wasmer_v";
pub(crate) const NAPI_EXTENSION_WASMER_MODULE_NAME: &str = "napi_extension_wasmer_v0";

pub use ctx::{NapiCtx, NapiCtxBuilder, NapiLimits, NapiRuntimeHooks, NapiSession};
pub(crate) use env::{GuestBackingStoreMapping, HostBufferCopy, RuntimeEnv};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NapiVersion {
    V10,
    Unknown,
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
