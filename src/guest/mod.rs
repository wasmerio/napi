pub(crate) mod abi;
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub mod callback;
#[cfg(all(target_arch = "wasm32", feature = "js"))]
#[path = "callback_js.rs"]
pub mod callback;
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub(crate) mod callback_cache;
pub(crate) mod compat;
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

/// How far, in code units, the JS backend scans for a `NAPI_AUTO_LENGTH`
/// string's terminator (util_js.rs), and the V8 backend for a UTF-16 one. The
/// V8 backend's UTF-8 and Latin-1 strings scan to the end of guest memory, or
/// for names and messages to `V8_MAX_STRING_BYTES` (util.rs
/// `read_guest_c_string`).
pub const MAX_GUEST_CSTRING_SCAN: usize = 64 * 1024;

/// Hard ceilings on host scratch buffers sized by a guest-supplied *count*
/// (rather than by data that lives in guest memory). These are rejected before
/// allocating — mirroring the WASIX `MAX_SOCKET_PAYLOAD` write cap — so a bogus
/// count can never allocate gigabytes host-side. Each is ~256 KiB worth, far
/// beyond any legitimate use (no function has 65k args; no real BigInt is a
/// 2-Mbit integer), which keeps the transient uncounted host memory negligible.
pub const MAX_NAPI_CALLBACK_ARGS: usize = 64 * 1024;
pub const MAX_NAPI_BIGINT_WORDS: usize = 32 * 1024;

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

/// A function or class name, or a symbol description, that the guest passed
/// as a pointer and a byte length, held for one bridge call.
///
/// The bridge gets a pointer to bytes held here and their exact count, so the
/// engine never reads past them. NULs are bytes like any other: an explicit
/// length may hold them, as in Node, where `napi_create_function(env, "a\0b",
/// 3, ...)` names the function `"a\0b"`. A NUL follows the bytes as well, in
/// case anything reads the name as a C string.
///
/// A NULL guest pointer with an explicit non-zero length reaches the bridge
/// as NULL with that length, never as a length with nothing behind it. The
/// engine decides: V8 refuses it for a class or private symbol and ignores
/// it for a function, as Node does; the host-JS bridge ignores it.
pub(crate) struct BridgeName {
    /// The name, then a NUL.
    bytes: Vec<u8>,
    len: u32,
    null: bool,
}

impl BridgeName {
    /// `bytes` were read from the guest's `guest_ptr` for `guest_len`: up to
    /// the NUL for `NAPI_AUTO_LENGTH` (`u32::MAX`), exactly `guest_len` bytes
    /// otherwise, and none for a NULL pointer with an explicit length.
    pub(crate) fn new(guest_ptr: u32, guest_len: u32, mut bytes: Vec<u8>) -> Self {
        let null = guest_ptr == 0 && guest_len != 0 && guest_len != u32::MAX;
        // Bytes read from a 32-bit memory number below u32::MAX, which the
        // bridge would take for NAPI_AUTO_LENGTH; the clamp only keeps it so.
        let len = if null {
            guest_len
        } else {
            u32::try_from(bytes.len())
                .unwrap_or(u32::MAX)
                .min(u32::MAX - 1)
        };
        bytes.push(0);
        Self { bytes, len, null }
    }

    pub(crate) fn as_ptr(&self) -> *const std::ffi::c_char {
        if self.null {
            std::ptr::null()
        } else {
            self.bytes.as_ptr().cast()
        }
    }

    /// The length the bridge gets: never `u32::MAX`, its NAPI_AUTO_LENGTH.
    pub(crate) fn len(&self) -> u32 {
        self.len
    }
}

#[cfg(test)]
mod tests {
    use super::BridgeName;

    fn held(name: &BridgeName) -> Vec<u8> {
        // SAFETY: as_ptr points at len bytes and a NUL.
        unsafe { std::slice::from_raw_parts(name.as_ptr().cast::<u8>(), name.len() as usize + 1) }
            .to_vec()
    }

    #[test]
    fn bridge_name_keeps_every_byte_and_its_exact_length() {
        let name = BridgeName::new(64, 5, b"ab\0cd".to_vec());
        assert_eq!(name.len(), 5);
        assert_eq!(held(&name), b"ab\0cd\0");

        // NAPI_AUTO_LENGTH: the bytes up to the NUL, passed with their count.
        let name = BridgeName::new(64, u32::MAX, b"abc".to_vec());
        assert_eq!(name.len(), 3);
        assert_eq!(held(&name), b"abc\0");

        // Empty, with a pointer or without one.
        for ptr in [0, 64] {
            let name = BridgeName::new(ptr, 0, Vec::new());
            assert_eq!(name.len(), 0);
            assert_eq!(held(&name), b"\0");
        }
    }

    #[test]
    fn bridge_name_keeps_a_null_pointer_null() {
        let name = BridgeName::new(0, 7, Vec::new());
        assert!(name.as_ptr().is_null());
        assert_eq!(name.len(), 7);
        // With NAPI_AUTO_LENGTH, address 0 is the caller's to read or not
        // (functions and classes read the C string there); whatever was read
        // is passed on.
        let name = BridgeName::new(0, u32::MAX, b"x".to_vec());
        assert!(!name.as_ptr().is_null());
        assert_eq!(name.len(), 1);
    }
}
