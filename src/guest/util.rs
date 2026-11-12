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

/// Host address of a guest range the provider keeps writing to after the
/// import returns (`unofficial_napi_register_guest_state`). The memory's base
/// never moves (see `guest_heap`), and the guest keeps the range allocated.
pub(crate) fn guest_host_range(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    len: usize,
) -> Option<*mut u8> {
    guest_range(env, guest_ptr, len)
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

/// A copy of the `len` guest bytes at `src`.
///
/// # Safety
/// They must be mapped guest memory.
#[inline]
unsafe fn guest_bytes_at(src: *const u8, len: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(len);
    // SAFETY: as the caller ensures; `out` has `len` bytes of capacity, all
    // written before `set_len`.
    unsafe {
        copy_from_guest(src, out.as_mut_ptr(), len);
        out.set_len(len);
    }
    out
}

pub fn read_guest_bytes(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    len: usize,
) -> Option<Vec<u8>> {
    if let Some(src) = guest_range(env, guest_ptr, len) {
        // SAFETY: the range is mapped guest memory.
        return Some(unsafe { guest_bytes_at(src, len) });
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

/// Length of the bytes before the first NUL among the `len` guest bytes at
/// `src`, or `None` if they hold none. Eight bytes per read where it can, like
/// `copy_from_guest`: a word with a zero byte is stepped through bytewise (if
/// another guest thread wrote that byte meanwhile, the scan goes on).
///
/// # Safety
/// The `len` bytes at `src` must be mapped guest memory.
unsafe fn guest_nul_offset(src: *const u8, len: usize) -> Option<usize> {
    const ONES: u64 = u64::from_ne_bytes([0x01; 8]);
    const HIGHS: u64 = u64::from_ne_bytes([0x80; 8]);
    let mut index = 0;
    while index < len {
        if len - index >= 8 {
            // SAFETY: the caller's range holds these 8 bytes.
            let word = unsafe { (src.add(index) as *const Unaligned<u64>).read_volatile() }.0;
            // No zero byte in `word` (exact as a yes/no test).
            if word.wrapping_sub(ONES) & !word & HIGHS == 0 {
                index += 8;
                continue;
            }
        }
        // SAFETY: as above, one byte.
        if unsafe { src.add(index).read_volatile() } == 0 {
            return Some(index);
        }
        index += 1;
    }
    None
}

/// The longest string V8 makes, in UTF-16 code units: `v8::String::kMaxLength`
/// (v8-primitive.h), which depends on the host's pointer width. A UTF-8 or
/// Latin-1 string has no more code units than bytes, so one of at most this
/// many bytes is never too long. V8 refuses a longer string given its length,
/// but aborts the process on one given without (`NAPI_AUTO_LENGTH`, as -1).
pub const V8_MAX_STRING_BYTES: usize = if cfg!(target_pointer_width = "32") {
    (1 << 28) - 16
} else {
    (1 << 29) - 24
};

/// How many guest bytes from `start`, in a memory of `size` bytes, may hold a
/// `NAPI_AUTO_LENGTH` string of at most `max` bytes and its NUL: the rest of
/// the memory, but at most `max + 1` bytes and at most `u32::MAX`. A string is
/// then at most `u32::MAX - 1` bytes, the bound explicit lengths have, because
/// the bridge takes a length of `u32::MAX` for `NAPI_AUTO_LENGTH` and would
/// run `strlen` over the host copy, which has no NUL. Only a string at address
/// 0 of a 4 GiB memory meets that cap. `None` if `start` is at or past the end.
fn c_string_search_len(size: u64, start: u64, max: usize) -> Option<usize> {
    let rest = size.checked_sub(start).filter(|&rest| rest > 0)?;
    let bound = (max as u64).saturating_add(1).min(u64::from(u32::MAX));
    usize::try_from(rest.min(bound)).ok()
}

/// The guest's NUL-terminated string at `guest_ptr` (a `NAPI_AUTO_LENGTH`
/// argument), without the NUL. `None` if no NUL comes before the end of guest
/// memory (or within `u32::MAX` bytes, see `c_string_search_len`), or
/// `guest_ptr` is outside it.
pub fn read_guest_c_string(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32) -> Option<Vec<u8>> {
    read_guest_c_string_max(env, guest_ptr, usize::MAX)
}

/// As `read_guest_c_string`, but also `None` for a string longer than `max`
/// bytes, without a scan past them. Strings the bridge hands to V8 with no
/// length (names, error codes and messages, symbol descriptions) are read with
/// `V8_MAX_STRING_BYTES`.
pub fn read_guest_c_string_max(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    max: usize,
) -> Option<Vec<u8>> {
    // Names are short: scan a first chunk in place, which needs no lookup of
    // the memory's size while it lies within the length already seen.
    const CHUNK: usize = 256;
    let chunk = CHUNK.min(max.saturating_add(1));
    let mut scanned = 0;
    if let Some(src) = guest_range(env, guest_ptr, chunk) {
        // SAFETY: the chunk is mapped guest memory.
        if let Some(len) = unsafe { guest_nul_offset(src, chunk) } {
            // SAFETY: the string's bytes are in the chunk.
            return Some(unsafe { guest_bytes_at(src, len) });
        }
        scanned = chunk;
    }
    // A longer string (script sources can reach megabytes), or one that starts
    // within a chunk of the memory's end: the rest of the memory, in place.
    let start = u64::from(guest_ptr);
    let search = c_string_search_len(guest_data_size(env), start, max)?;
    if let Some(src) = guest_range(env, guest_ptr, search) {
        // The chunk lay within the memory and `max + 1` bytes, so within
        // `search`. That length comes from this env's view, the chunk's from
        // the heap: check anyway.
        let tail = search.checked_sub(scanned)?;
        // SAFETY: the range is mapped guest memory, and `scanned` <= `search`.
        let len = scanned + unsafe { guest_nul_offset(src.add(scanned), tail) }?;
        // SAFETY: the string's bytes are in the range.
        return Some(unsafe { guest_bytes_at(src, len) });
    }
    // No guest heap: through the `MemoryView`. Find the NUL a block at a
    // time, then read the string once.
    let (state, store) = env.data_and_store_mut();
    let memory = state.memory.clone()?;
    let view = memory.view(&store);
    let end = start + search as u64;
    let mut block = [0u8; 4096];
    let mut offset = start;
    while offset < end {
        let len = (end - offset).min(block.len() as u64) as usize;
        view.read(offset, &mut block[..len]).ok()?;
        if let Some(nul) = block[..len].iter().position(|&byte| byte == 0) {
            let mut out = vec![0u8; usize::try_from(offset - start).ok()? + nul];
            view.read(start, &mut out).ok()?;
            return Some(out);
        }
        offset += len as u64;
    }
    None
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::AtomicBool;
    use std::sync::{Arc, Mutex};

    use wasmer::{AsStoreMut, FunctionEnv, FunctionEnvMut, Memory, MemoryType, Pages, Store};

    use super::{
        V8_MAX_STRING_BYTES, c_string_search_len, copy_from_guest, copy_to_guest, guest_data_size,
        guest_nul_offset, read_guest_c_string_max,
    };
    use crate::NapiEnv;
    use crate::budget::ResourceBudget;
    use crate::env::EnvRegistry;
    use crate::guest_heap::GuestHeap;

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

    /// The NUL scan finds the first zero at every start alignment and every
    /// position, in the 8-byte words and the bytewise tail, among bytes that
    /// are 0x01 or have the high bit set (the edges of the word test), and
    /// never reports a zero just past the range.
    #[test]
    fn nul_scan_finds_the_first_zero_in_the_range() {
        let fillers: [&[u8]; 4] = [&[0x01], &[0x80], &[0xff], &[0x01, 0x80, 0xff, 0x7f, 0x81]];
        for filler in fillers {
            for start in 0..8 {
                for len in 0..=40 {
                    let mut bytes: Vec<u8> = (0..start + len + 1)
                        .map(|i| filler[i % filler.len()])
                        .collect();
                    bytes[start + len] = 0;
                    // SAFETY: the `len` bytes from `start` are inside `bytes`.
                    let scan =
                        |bytes: &[u8]| unsafe { guest_nul_offset(bytes[start..].as_ptr(), len) };
                    let case = format!("filler {filler:x?}, start {start}, {len} bytes");
                    assert_eq!(scan(&bytes), None, "{case}");
                    for nul in 0..len {
                        let mut with_nul = bytes.clone();
                        with_nul[start + nul] = 0;
                        // A later zero as well: the first one counts.
                        with_nul[start + len - 1] = 0;
                        assert_eq!(scan(&with_nul), Some(nul), "{case}, NUL at {nul}");
                    }
                }
            }
        }
    }

    /// The search for a `NAPI_AUTO_LENGTH` string's NUL covers the rest of the
    /// memory, but never more than `u32::MAX` bytes: a string of `u32::MAX`
    /// bytes (a 4 GiB memory, the string at 0, its NUL the last byte) would
    /// reach the bridge as `NAPI_AUTO_LENGTH`.
    #[test]
    fn c_string_search_never_finds_a_string_of_u32_max_bytes() {
        const MAX: usize = u32::MAX as usize;
        let four_gib = 1u64 << 32;
        let search = |size, start| c_string_search_len(size, start, usize::MAX);
        assert_eq!(search(four_gib, 0), Some(MAX));
        assert_eq!(search(four_gib, 1), Some(MAX));
        assert_eq!(search(four_gib, 2), Some(MAX - 1));
        assert_eq!(search(four_gib, four_gib - 1), Some(1));
        assert_eq!(search(four_gib, four_gib), None);
        // Smaller memories: the rest of the memory, nothing at or past its end.
        assert_eq!(search(65536, 0), Some(65536));
        assert_eq!(search(65536, 65535), Some(1));
        assert_eq!(search(65536, 65536), None);
        assert_eq!(search(65536, u64::from(u32::MAX)), None);
        assert_eq!(search(0, 0), None);
    }

    /// A search bounded at `max` bytes finds a string of `max` bytes and not
    /// one of `max + 1`, in a memory with room for longer ones. Names and
    /// messages are read with `V8_MAX_STRING_BYTES`, `v8::String::kMaxLength`,
    /// so V8 never gets one too long for it.
    #[test]
    fn c_string_search_stops_after_max_bytes() {
        for max in [0, 1, 7, 8, 9, 255, 256, 1000] {
            let mut bytes = vec![b'a'; max + 64];
            let search = c_string_search_len(bytes.len() as u64, 0, max);
            assert_eq!(search, Some(max + 1), "max {max}");
            let scan = |bytes: &[u8]| {
                // SAFETY: `search` <= `bytes.len()`.
                unsafe { guest_nul_offset(bytes.as_ptr(), search.unwrap()) }
            };
            bytes[max + 1] = 0;
            assert_eq!(scan(&bytes), None, "{} bytes, max {max}", max + 1);
            bytes[max] = 0;
            assert_eq!(scan(&bytes), Some(max), "{max} bytes, max {max}");
        }
        // The rest of a small memory is less than the bound.
        assert_eq!(c_string_search_len(65536, 1000, 65536), Some(64536));
        let four_gib = 1u64 << 32;
        assert_eq!(
            c_string_search_len(four_gib, 0, V8_MAX_STRING_BYTES),
            Some(V8_MAX_STRING_BYTES + 1)
        );
        assert_eq!(
            c_string_search_len(four_gib, 0, u32::MAX as usize),
            Some(u32::MAX as usize)
        );
    }

    /// `read_guest_c_string_max` over a real memory, in place through a guest
    /// heap and through the `MemoryView` without one, reads a string of `max`
    /// bytes and refuses one of `max + 1`. That holds for limits below the
    /// 256-byte first chunk too, which no import passes, so the in-place chunk
    /// must stop after `max + 1` bytes as well. And it holds for strings that
    /// end at the memory's last byte or run into its end.
    #[test]
    fn c_string_reads_stop_after_max_bytes() {
        let mut store = Store::default();
        let ty = MemoryType::new(Pages(64), Some(Pages(2048)), false);
        let memory = Memory::new(&mut store, ty).expect("create owned memory");
        let heap = GuestHeap::new(
            &mut store.as_store_mut(),
            &memory,
            ResourceBudget::unlimited(),
        )
        .expect("guest heap");
        for heap in [Some(heap), None] {
            let path = if heap.is_some() { "heap" } else { "view" };
            let mut state = NapiEnv::new(
                ResourceBudget::unlimited(),
                None,
                Arc::new(Mutex::new(EnvRegistry::default())),
                Arc::new(AtomicBool::new(false)),
            );
            state.memory = Some(memory.clone());
            state.guest_heap = heap;
            let mut env = FunctionEnv::new(&mut store, state).into_mut(&mut store);
            let size = u32::try_from(guest_data_size(&mut env)).unwrap();
            let put = |env: &mut FunctionEnvMut<NapiEnv>, ptr: u32, bytes: &[u8]| {
                let (state, store) = env.data_and_store_mut();
                let view = state.memory.as_ref().unwrap().view(&store);
                view.write(u64::from(ptr), bytes).unwrap();
            };
            let text = |len: usize| vec![b'a'; len];
            let with_nul = |len: usize| [text(len), vec![0]].concat();
            for max in [0, 1, 7, 8, 9, 255, 256, 257, 1000] {
                let max_u32 = max as u32;
                // Away from the end: `max` bytes, then `max + 1`, which only
                // the bound refuses.
                put(&mut env, 4096, &with_nul(max));
                let case = format!("{path}, max {max}");
                assert_eq!(
                    read_guest_c_string_max(&mut env, 4096, max),
                    Some(text(max)),
                    "{case}"
                );
                put(&mut env, 4096, &with_nul(max + 1));
                assert_eq!(
                    read_guest_c_string_max(&mut env, 4096, max),
                    None,
                    "{case}, one more"
                );
                assert_eq!(
                    read_guest_c_string_max(&mut env, 4096, usize::MAX),
                    Some(text(max + 1))
                );
                // The NUL is the memory's last byte.
                let at_end = size - max_u32 - 1;
                put(&mut env, at_end, &with_nul(max));
                let case = format!("{path}, max {max}, ending at the end");
                assert_eq!(
                    read_guest_c_string_max(&mut env, at_end, max),
                    Some(text(max)),
                    "{case}"
                );
                let one_more = size - max_u32 - 2;
                put(&mut env, one_more, &with_nul(max + 1));
                assert_eq!(
                    read_guest_c_string_max(&mut env, one_more, max),
                    None,
                    "{case}, one more"
                );
                assert_eq!(
                    read_guest_c_string_max(&mut env, one_more, usize::MAX),
                    Some(text(max + 1))
                );
                // `max + 1` bytes up to the end and no NUL.
                put(&mut env, at_end, &text(max + 1));
                let case = format!("{path}, max {max}, unterminated");
                assert_eq!(
                    read_guest_c_string_max(&mut env, at_end, max),
                    None,
                    "{case}"
                );
                assert_eq!(
                    read_guest_c_string_max(&mut env, at_end, usize::MAX),
                    None,
                    "{case}"
                );
            }
        }
    }
}
