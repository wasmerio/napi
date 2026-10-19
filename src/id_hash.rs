//! Hashing for maps keyed by ids the provider allocates itself (environments,
//! scopes, handles, references, callbacks, wraps) or by host addresses.
//!
//! Such keys are never chosen by the guest, so SipHash's flooding resistance
//! only costs time, and several of these maps are consulted on every N-API
//! call. A multiplicative hash spreads sequential ids over both the bucket index
//! (low bits) and hashbrown's control byte (top bits).

use std::hash::{BuildHasherDefault, Hasher};

#[derive(Default)]
pub(crate) struct IdHasher(u64);

impl Hasher for IdHasher {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        for &byte in bytes {
            self.write_u64(u64::from(byte));
        }
    }

    fn write_u32(&mut self, value: u32) {
        self.write_u64(u64::from(value));
    }

    fn write_u64(&mut self, value: u64) {
        self.0 = (self.0.rotate_left(5) ^ value).wrapping_mul(0x9e37_79b9_7f4a_7c15);
    }

    fn write_usize(&mut self, value: usize) {
        self.write_u64(value as u64);
    }
}

/// A `HashMap` for provider-allocated keys; create it with `default()`.
pub(crate) type IdHashMap<K, V> = std::collections::HashMap<K, V, BuildHasherDefault<IdHasher>>;

#[cfg(test)]
mod tests {
    use super::IdHashMap;

    #[cfg_attr(target_arch = "wasm32", wasm_bindgen_test::wasm_bindgen_test)]
    #[cfg_attr(not(target_arch = "wasm32"), test)]
    fn sequential_and_tuple_keys_round_trip() {
        let mut map = IdHashMap::default();
        for id in 1..=10_000u32 {
            map.insert((id, id.wrapping_mul(7)), id);
        }
        for id in 1..=10_000u32 {
            assert_eq!(map.get(&(id, id.wrapping_mul(7))), Some(&id));
        }
        assert_eq!(map.get(&(0, 0)), None);
    }
}
