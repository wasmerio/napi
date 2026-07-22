pub mod callback;
pub mod napi;
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
