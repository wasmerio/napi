// ============================================================
// Guest memory helpers
// ============================================================

use wasmer::{AsStoreMut, FunctionEnvMut};

use crate::NapiEnv;

/// Host address of a non-empty guest range, through the env's guest heap,
/// which maps the whole memory at a fixed base. `None` (no heap, out of
/// bounds) means the caller takes the `MemoryView` path, which reports errors.
fn guest_range(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, len: usize) -> Option<*mut u8> {
    if len == 0 {
        return None;
    }
    let (state, store) = env.data_and_store_mut();
    let heap = state.guest_heap.as_ref()?;
    let memory = state.memory.as_ref()?;
    heap.host_range(guest_ptr as u64, len as u64, || {
        let view = memory.view(&store);
        (view.data_ptr(), view.data_size())
    })
}

// Copies that tolerate other guest threads writing the same bytes, like
// Wasmer's own `MemoryBuffer` reads and writes: volatile on the guest side,
// in steps of 8, 4, 2 and 1 bytes, so a `u32` is still one access.
#[repr(C, packed)]
struct Unaligned<T>(T);

#[inline]
unsafe fn copy_from_guest(mut src: *const u8, mut dst: *mut u8, mut len: usize) {
    #[inline]
    unsafe fn step<T>(src: &mut *const u8, dst: &mut *mut u8, len: &mut usize) {
        unsafe {
            let value = (*src as *const Unaligned<T>).read_volatile();
            (*dst as *mut Unaligned<T>).write(value);
            *src = src.add(size_of::<T>());
            *dst = dst.add(size_of::<T>());
        }
        *len -= size_of::<T>();
    }
    unsafe {
        while len >= 8 {
            step::<u64>(&mut src, &mut dst, &mut len);
        }
        if len >= 4 {
            step::<u32>(&mut src, &mut dst, &mut len);
        }
        if len >= 2 {
            step::<u16>(&mut src, &mut dst, &mut len);
        }
        if len >= 1 {
            step::<u8>(&mut src, &mut dst, &mut len);
        }
    }
}

#[inline]
unsafe fn copy_to_guest(mut src: *const u8, mut dst: *mut u8, mut len: usize) {
    #[inline]
    unsafe fn step<T>(src: &mut *const u8, dst: &mut *mut u8, len: &mut usize) {
        unsafe {
            let value = (*src as *const Unaligned<T>).read();
            (*dst as *mut Unaligned<T>).write_volatile(value);
            *src = src.add(size_of::<T>());
            *dst = dst.add(size_of::<T>());
        }
        *len -= size_of::<T>();
    }
    unsafe {
        while len >= 8 {
            step::<u64>(&mut src, &mut dst, &mut len);
        }
        if len >= 4 {
            step::<u32>(&mut src, &mut dst, &mut len);
        }
        if len >= 2 {
            step::<u16>(&mut src, &mut dst, &mut len);
        }
        if len >= 1 {
            step::<u8>(&mut src, &mut dst, &mut len);
        }
    }
}

pub fn write_guest_bytes(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, data: &[u8]) -> bool {
    if let Some(dst) = guest_range(env, guest_ptr, data.len()) {
        // SAFETY: `guest_range` checked the whole range is mapped guest memory.
        unsafe { copy_to_guest(data.as_ptr(), dst, data.len()) };
        return true;
    }
    let (state, store) = env.data_and_store_mut();
    let Some(memory) = state.memory.clone() else {
        return false;
    };
    let view = memory.view(&store);
    view.write(guest_ptr as u64, data).is_ok()
}

pub fn write_guest_u32(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: u32) -> bool {
    write_guest_bytes(env, guest_ptr, &val.to_le_bytes())
}

pub fn write_guest_i32(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: i32) -> bool {
    write_guest_bytes(env, guest_ptr, &val.to_le_bytes())
}

pub fn write_guest_u64(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: u64) -> bool {
    write_guest_bytes(env, guest_ptr, &val.to_le_bytes())
}

pub fn write_guest_i64(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: i64) -> bool {
    write_guest_bytes(env, guest_ptr, &val.to_le_bytes())
}

pub fn write_guest_f64(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: f64) -> bool {
    write_guest_bytes(env, guest_ptr, &val.to_le_bytes())
}

pub fn write_guest_u8(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: u8) -> bool {
    write_guest_bytes(env, guest_ptr, &[val])
}

pub fn read_guest_bytes(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    len: usize,
) -> Option<Vec<u8>> {
    if let Some(src) = guest_range(env, guest_ptr, len) {
        let mut out = Vec::with_capacity(len);
        // SAFETY: the range is mapped guest memory; `out` has `len` bytes of
        // capacity, all written before `set_len`.
        unsafe {
            copy_from_guest(src, out.as_mut_ptr(), len);
            out.set_len(len);
        }
        return Some(out);
    }
    let (state, store) = env.data_and_store_mut();
    let memory = state.memory.clone()?;
    let view = memory.view(&store);
    // A guest cannot legitimately reference more bytes than its own linear
    // memory holds, so reject a length that exceeds it before allocating. This
    // bounds the host copy to memory the guest was already charged for and
    // stops a bogus guest-supplied length from allocating gigabytes here (the
    // subsequent bounds-checked read would fail, but only after the `vec!`).
    if len as u64 > view.data_size() {
        return None;
    }
    let mut out = vec![0u8; len];
    view.read(guest_ptr as u64, &mut out).ok()?;
    Some(out)
}

/// Reads a little-endian u32 from guest memory without allocating.
pub fn read_guest_u32(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32) -> Option<u32> {
    if let Some(src) = guest_range(env, guest_ptr, 4) {
        let mut bytes = [0u8; 4];
        // SAFETY: the 4-byte range is mapped guest memory.
        unsafe { copy_from_guest(src, bytes.as_mut_ptr(), 4) };
        return Some(u32::from_le_bytes(bytes));
    }
    let (state, store) = env.data_and_store_mut();
    let memory = state.memory.clone()?;
    let mut bytes = [0u8; 4];
    memory
        .view(&store)
        .read(guest_ptr as u64, &mut bytes)
        .ok()?;
    Some(u32::from_le_bytes(bytes))
}

/// Live size of the guest's linear memory in bytes, or 0 if it has none. Used
/// to bound host allocations sized by a guest-supplied length: a guest can
/// never reference more than its own memory holds.
pub fn guest_data_size(env: &mut FunctionEnvMut<NapiEnv>) -> u64 {
    let Some(memory) = env.data().memory.clone() else {
        return 0;
    };
    let (_, store) = env.data_and_store_mut();
    memory.view(&store).data_size()
}

/// Allocate `len` bytes of guest memory, passing the import's store directly so
/// the heap can claim more from the guest when its arena is short.
///
/// V8 allocator hooks can recover a store only inside an explicitly guarded
/// reentrant bridge; ordinary imports already have the store and should not
/// publish and rediscover it through runtime TLS.
pub fn alloc_guest(
    env: &mut FunctionEnvMut<NapiEnv>,
    heap: &crate::guest_heap::GuestHeap,
    len: usize,
    zero: bool,
) -> Option<u32> {
    let mut store = env.as_store_mut();
    heap.alloc_with_store(&mut store, len, zero)
}

#[allow(unused)]
pub fn allocate_guest_bytes(env: &mut FunctionEnvMut<NapiEnv>, data: &[u8]) -> Option<u32> {
    let heap = env.data().guest_heap.clone()?;
    let guest_ptr = alloc_guest(env, &heap, data.len(), false)?;
    if !write_guest_bytes(env, guest_ptr, data) {
        heap.free_offset(guest_ptr);
        return None;
    }
    Some(guest_ptr)
}

pub fn host_ptr_to_guest_ptr(env: &mut FunctionEnvMut<NapiEnv>, host_addr: u64) -> Option<u32> {
    let memory = env.data().memory.clone()?;
    let (_, store_ref) = env.data_and_store_mut();
    let view = memory.view(&store_ref);
    let host_base = view.data_ptr() as u64;
    let memory_len = view.data_size();
    if host_addr < host_base || host_addr >= host_base + memory_len {
        return None;
    }
    u32::try_from(host_addr - host_base).ok()
}

pub fn read_guest_u32_array(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    count: usize,
) -> Option<Vec<u32>> {
    // Guard the byte-length multiply against overflow; the read below is then
    // clamped to the guest's memory size by `read_guest_bytes`.
    let byte_len = count.checked_mul(4)?;
    let bytes = read_guest_bytes(env, guest_ptr, byte_len)?;
    let mut result = Vec::with_capacity(count);
    for chunk in bytes.chunks_exact(4) {
        result.push(u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]));
    }
    Some(result)
}

pub fn read_guest_c_string(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32) -> Option<Vec<u8>> {
    // Names are short: scan a first chunk in place. A string that is longer,
    // or a chunk that would pass the memory's end, takes the full scan below.
    const CHUNK: usize = 256;
    if let Some(src) = guest_range(env, guest_ptr, CHUNK) {
        let mut out = Vec::new();
        for index in 0..CHUNK {
            // SAFETY: the chunk is mapped guest memory.
            let byte = unsafe { src.add(index).read_volatile() };
            if byte == 0 {
                return Some(out);
            }
            out.push(byte);
        }
    }
    let (state, store) = env.data_and_store_mut();
    let memory = state.memory.clone()?;
    let view = memory.view(&store);
    let mut out = Vec::new();
    for offset in (guest_ptr as u64..).take(super::MAX_GUEST_CSTRING_SCAN) {
        let mut b = [0u8; 1];
        view.read(offset, &mut b).ok()?;
        if b[0] == 0 {
            return Some(out);
        }
        out.push(b[0]);
    }
    None
}

#[cfg(test)]
mod tests {
    use super::{copy_from_guest, copy_to_guest};

    /// Both copies move exactly `len` bytes for every alignment of either
    /// side and every length through the 8-byte loop and each of the 4-, 2-
    /// and 1-byte tails, and touch nothing around the destination.
    #[test]
    fn guest_copies_handle_any_alignment_and_length() {
        let source: Vec<u8> = (0..64u8).map(|b| b.wrapping_mul(37) ^ 0x5a).collect();
        let copies: [(&str, unsafe fn(*const u8, *mut u8, usize)); 2] =
            [("from", copy_from_guest), ("to", copy_to_guest)];
        for (name, copy) in copies {
            for src_at in 0..8 {
                for dst_at in 0..8 {
                    for len in 0..=40 {
                        let mut dst = [0xeeu8; 64];
                        // SAFETY: both ranges are inside their arrays.
                        unsafe { copy(source[src_at..].as_ptr(), dst[dst_at..].as_mut_ptr(), len) };
                        let case = format!("copy_{name}_guest {src_at} -> {dst_at}, {len} bytes");
                        assert_eq!(
                            dst[dst_at..dst_at + len],
                            source[src_at..src_at + len],
                            "{case}"
                        );
                        assert!(dst[..dst_at].iter().all(|&b| b == 0xee), "{case}");
                        assert!(dst[dst_at + len..].iter().all(|&b| b == 0xee), "{case}");
                    }
                }
            }
        }
    }
}
