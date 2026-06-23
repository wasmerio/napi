pub mod callback;
pub mod napi;
pub mod util;
#[cfg(feature = "cli")]
pub mod wasmer_c_api;

pub const MAX_GUEST_CSTRING_SCAN: usize = 64 * 1024;
