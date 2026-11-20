// ============================================================
// Guest memory helpers
// ============================================================

use std::{
    mem::MaybeUninit,
    ops::{Deref, DerefMut},
    sync::Arc,
};

use wasmer::{AsStoreMut, FunctionEnvMut};

use crate::{
    NapiEnv,
    budget::{Pool, ResourceBudget, UNCHARGED_HOST_COPY_BYTES},
};

/// Bytes charged to [`Pool::HostTransient`] for host copies of guest memory,
/// returned when dropped.
#[derive(Debug, Default)]
pub struct TransientCharge {
    budget: Option<Arc<ResourceBudget>>,
    bytes: u64,
}

impl TransientCharge {
    /// Charges `bytes` more to `budget`. False, charging nothing, if the
    /// budget refused them.
    pub fn add(&mut self, budget: &Arc<ResourceBudget>, bytes: usize) -> bool {
        let Ok(bytes) = u64::try_from(bytes) else {
            return false;
        };
        if bytes == 0 {
            return true;
        }
        if budget.try_charge(Pool::HostTransient, bytes).is_err() {
            return false;
        }
        match &self.budget {
            Some(held) => debug_assert!(Arc::ptr_eq(held, budget)),
            None => self.budget = Some(Arc::clone(budget)),
        }
        self.bytes += bytes;
        true
    }

    /// Takes over `other`'s charge.
    pub fn absorb(&mut self, mut other: TransientCharge) {
        let Some(budget) = other.budget.take() else {
            return;
        };
        match &self.budget {
            Some(held) => debug_assert!(Arc::ptr_eq(held, &budget)),
            None => self.budget = Some(budget),
        }
        self.bytes += std::mem::take(&mut other.bytes);
    }

    /// The charge for a host copy of `bytes` guest bytes: nothing below
    /// [`UNCHARGED_HOST_COPY_BYTES`]. `None` if the budget refused it.
    fn for_copy(env: &FunctionEnvMut<NapiEnv>, bytes: usize) -> Option<Self> {
        let mut charge = Self::default();
        if bytes >= UNCHARGED_HOST_COPY_BYTES && !charge.add(&env.data().budget, bytes) {
            return None;
        }
        Some(charge)
    }
}

impl Drop for TransientCharge {
    fn drop(&mut self) {
        if let Some(budget) = &self.budget {
            budget.uncharge(Pool::HostTransient, self.bytes);
        }
    }
}

/// A value made from guest memory with its [`TransientCharge`]: the bytes
/// stay charged for as long as the value lives.
#[derive(Debug, Default)]
pub struct Charged<T> {
    // Dropped before `charge`, so the budget never sees bytes that are still
    // allocated as free.
    value: T,
    charge: TransientCharge,
}

/// A host copy of guest memory, charged while it lives.
pub type HostCopy<T = u8> = Charged<Vec<T>>;

impl<T> Charged<T> {
    /// The value made from this one by `f`, keeping the charge: for a copy
    /// converted in place (a `CString` from its bytes).
    pub fn map<U>(self, f: impl FnOnce(T) -> U) -> Charged<U> {
        let (value, charge) = self.into_parts();
        Charged {
            value: f(value),
            charge,
        }
    }

    /// The value, dropping its charge: only for copies too small to be
    /// charged (fixed-size structs).
    pub fn into_inner(self) -> T {
        debug_assert_eq!(self.charge.bytes, 0);
        self.into_parts().0
    }

    /// The value and its charge, for a caller that must keep the charge for
    /// as long as it keeps the value in another form.
    pub fn into_parts(self) -> (T, TransientCharge) {
        let this = std::mem::ManuallyDrop::new(self);
        // SAFETY: both fields are moved out exactly once and `this` is never
        // used or dropped again.
        unsafe { (std::ptr::read(&this.value), std::ptr::read(&this.charge)) }
    }
}

impl<T> Deref for Charged<T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.value
    }
}

impl<T> DerefMut for Charged<T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.value
    }
}

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
    let host = heap.host_range(guest_ptr as u64, len as u64, || {
        let view = memory.view(&store);
        (view.data_ptr(), view.data_size())
    })?;
    // The range ended past what this env had seen of the memory (or the
    // caller did not ask its window first): the window takes the heap's
    // length over, so the next access there is a compare.
    state.refresh_guest_mem();
    Some(host)
}

/// The part of guest memory an env knows to be mapped: the guest heap's fixed
/// base and a length of the memory that the env has seen
/// ([`NapiEnv::guest_mem`]). Empty for an env with no guest heap.
///
/// A range inside the window is mapped for as long as the store's memory is,
/// because a linear memory only grows and the heap only accepts one whose
/// base never moves (`GuestHeap::new`). So an import may take the window
/// once, before its bridge call, and still use it afterwards, though the call
/// may have grown the memory: the window is then merely smaller than the
/// memory. An access the window does not hold is not an error; the caller
/// goes on to the checked path (`guest_range`, then the `MemoryView`), which
/// sees the memory's current length and reports the error if there is one.
///
/// Reads and writes are volatile and unaligned single accesses, like
/// `copy_from_guest` and `copy_to_guest`: other guest threads may write the
/// same bytes.
#[derive(Clone, Copy)]
pub struct GuestMem {
    base: *mut u8,
    len: u64,
}

impl GuestMem {
    #[inline(always)]
    pub(crate) fn new(base: *mut u8, len: u64) -> Self {
        Self { base, len }
    }

    /// Host address of the `len` guest bytes at `guest_ptr`, if the window
    /// holds them all. `None` for no bytes.
    #[inline(always)]
    fn range(self, guest_ptr: u32, len: u64) -> Option<*mut u8> {
        let end = u64::from(guest_ptr).checked_add(len)?;
        if len == 0 || end > self.len {
            return None;
        }
        // SAFETY: `end` is within a length the memory has had, and the whole
        // memory is mapped at `base`, which never moves.
        Some(unsafe { self.base.add(guest_ptr as usize) })
    }

    /// Writes `bytes` at `guest_ptr`, in one access. False, writing nothing,
    /// if the window does not hold those bytes.
    #[inline(always)]
    fn put<const N: usize>(self, guest_ptr: u32, bytes: [u8; N]) -> bool {
        let Some(dst) = self.range(guest_ptr, N as u64) else {
            return false;
        };
        // SAFETY: `range` checked that the bytes are mapped guest memory; a
        // byte array has no alignment to meet.
        unsafe { (dst as *mut [u8; N]).write_volatile(bytes) };
        true
    }

    /// The `N` bytes at `guest_ptr`, read in one access, if the window holds
    /// them.
    #[inline(always)]
    fn get<const N: usize>(self, guest_ptr: u32) -> Option<[u8; N]> {
        let src = self.range(guest_ptr, N as u64)?;
        // SAFETY: as in `put`.
        Some(unsafe { (src as *const [u8; N]).read_volatile() })
    }

    #[inline(always)]
    pub fn write_u8(self, guest_ptr: u32, val: u8) -> bool {
        self.put(guest_ptr, [val])
    }

    #[inline(always)]
    pub fn write_u32(self, guest_ptr: u32, val: u32) -> bool {
        self.put(guest_ptr, val.to_le_bytes())
    }

    #[inline(always)]
    pub fn write_u64(self, guest_ptr: u32, val: u64) -> bool {
        self.put(guest_ptr, val.to_le_bytes())
    }

    #[inline(always)]
    pub fn read_u32(self, guest_ptr: u32) -> Option<u32> {
        self.get(guest_ptr).map(u32::from_le_bytes)
    }

    /// Writes the `u32`s of `values` from `guest_ptr` on. False, writing
    /// nothing, if the window does not hold them all (or there are none).
    #[inline(always)]
    pub fn write_u32s(self, guest_ptr: u32, values: &[u32]) -> bool {
        let Some(dst) = self.range(guest_ptr, 4 * values.len() as u64) else {
            return false;
        };
        for (index, value) in values.iter().enumerate() {
            // SAFETY: `range` checked that all `4 * values.len()` bytes are
            // mapped guest memory.
            unsafe { (dst.add(4 * index) as *mut [u8; 4]).write_volatile(value.to_le_bytes()) };
        }
        true
    }

    /// Reads `out.len()` `u32`s from `guest_ptr` on. False, with `out`
    /// unspecified, if the window does not hold them all (or `out` is empty).
    #[inline(always)]
    pub fn read_u32s(self, guest_ptr: u32, out: &mut [u32]) -> bool {
        let Some(src) = self.range(guest_ptr, 4 * out.len() as u64) else {
            return false;
        };
        for (index, value) in out.iter_mut().enumerate() {
            // SAFETY: as in `write_u32s`.
            *value = u32::from_le_bytes(unsafe {
                (src.add(4 * index) as *const [u8; 4]).read_volatile()
            });
        }
        true
    }

    /// The guest's NUL-terminated string at `guest_ptr`, copied with its NUL
    /// into `out`, when the window holds all `N` bytes from `guest_ptr` and
    /// the string and its NUL fit them; returns the length without the NUL.
    /// `None` otherwise, with `out` unspecified: the caller then takes the
    /// checked path (`read_guest_name_inline`, `read_guest_c_string_max`).
    ///
    /// One pass, eight bytes a step: each word is read from the guest once
    /// and that same value is both stored and searched for a NUL, so the copy
    /// ends in the NUL the scan found whatever other guest threads write
    /// meanwhile. Bytes of `out` past that NUL are unspecified (those of the
    /// last word are the guest's, the rest are not written).
    #[inline(always)]
    pub fn read_c_str<const N: usize>(
        self,
        guest_ptr: u32,
        out: &mut MaybeUninit<[u8; N]>,
    ) -> Option<usize> {
        const ONES: u64 = u64::from_ne_bytes([0x01; 8]);
        const HIGHS: u64 = u64::from_ne_bytes([0x80; 8]);
        const { assert!(N.is_multiple_of(8)) };
        let src = self.range(guest_ptr, N as u64)?;
        let dst = out.as_mut_ptr().cast::<u8>();
        let mut index = 0;
        while index < N {
            // SAFETY: `range` checked that `N` bytes from `src` are mapped
            // guest memory, `out` has as many, and `index` is a multiple of
            // 8 below `N`.
            let word = unsafe {
                let word = (src.add(index) as *const [u8; 8]).read_volatile();
                (dst.add(index) as *mut [u8; 8]).write(word);
                u64::from_le_bytes(word)
            };
            // The lowest set bit marks the first zero byte exactly (bits
            // above it may be set for bytes that are not zero).
            let zeros = word.wrapping_sub(ONES) & !word & HIGHS;
            if zeros != 0 {
                return Some(index + (zeros.trailing_zeros() / 8) as usize);
            }
            index += 8;
        }
        None
    }

    /// Copies the `len` guest bytes at `guest_ptr` into `out`, when there are
    /// at most `N` of them and the window holds them. False otherwise (also
    /// for no bytes), with `out` unspecified.
    #[inline(always)]
    pub fn read_into<const N: usize>(
        self,
        guest_ptr: u32,
        len: usize,
        out: &mut MaybeUninit<[u8; N]>,
    ) -> bool {
        if len > N {
            return false;
        }
        let Some(src) = self.range(guest_ptr, len as u64) else {
            return false;
        };
        // SAFETY: `range` checked that the `len` bytes are mapped guest
        // memory, and `out` has room for `N >= len`.
        unsafe { copy_from_guest(src, out.as_mut_ptr().cast(), len) };
        true
    }

    /// The guest address of host address `host_addr`, if it lies inside the
    /// window.
    #[inline(always)]
    pub fn guest_ptr_of(self, host_addr: u64) -> Option<u32> {
        let offset = host_addr.wrapping_sub(self.base as u64);
        if offset < self.len {
            u32::try_from(offset).ok()
        } else {
            None
        }
    }

    /// Host address of the guest's own `len`-byte buffer at `guest_ptr`, for
    /// the engine to write a result into in place of a host scratch copy.
    /// `None` if the window does not hold it whole (or it is empty).
    ///
    /// The engine must only write there, at most `len` bytes, and must not
    /// read back what it wrote to decide anything: other guest threads may
    /// write the same bytes, as they may the backing store of any
    /// `ArrayBuffer`, which is guest memory that V8 writes just the same.
    #[inline(always)]
    pub fn out_buffer(self, guest_ptr: u32, len: u64) -> Option<*mut u8> {
        self.range(guest_ptr, len)
    }
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

/// `write_guest_bytes`, kept out of line for the scalar writers below: they
/// only get here for a range their env's [`GuestMem`] does not hold.
#[cold]
#[inline(never)]
fn write_guest_bytes_slow(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, data: &[u8]) -> bool {
    write_guest_bytes(env, guest_ptr, data)
}

// The scalar writers: nearly every import ends in one. Each first asks the
// env's window, which costs one read of the env's data, a compare and the
// store; `write_guest_bytes` is for what the window does not hold.

#[inline]
pub fn write_guest_u32(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: u32) -> bool {
    let mem = env.data().guest_mem();
    put_guest_u32(env, mem, guest_ptr, val)
}

#[inline]
pub fn write_guest_i32(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: i32) -> bool {
    write_guest_u32(env, guest_ptr, val as u32)
}

#[inline]
pub fn write_guest_u64(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: u64) -> bool {
    let mem = env.data().guest_mem();
    put_guest_u64(env, mem, guest_ptr, val)
}

#[inline]
pub fn write_guest_i64(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: i64) -> bool {
    write_guest_u64(env, guest_ptr, val as u64)
}

#[inline]
pub fn write_guest_u8(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: u8) -> bool {
    let mem = env.data().guest_mem();
    put_guest_u8(env, mem, guest_ptr, val)
}

// The same writers for an import that already holds its env's window `mem`
// (taken with the env, in one read of the env's data, before the bridge
// call): through the window, else as `write_guest_bytes` does.

#[inline(always)]
pub fn put_guest_u32(
    env: &mut FunctionEnvMut<NapiEnv>,
    mem: GuestMem,
    guest_ptr: u32,
    val: u32,
) -> bool {
    mem.write_u32(guest_ptr, val) || write_guest_bytes_slow(env, guest_ptr, &val.to_le_bytes())
}

#[inline(always)]
pub fn put_guest_i32(
    env: &mut FunctionEnvMut<NapiEnv>,
    mem: GuestMem,
    guest_ptr: u32,
    val: i32,
) -> bool {
    put_guest_u32(env, mem, guest_ptr, val as u32)
}

#[inline(always)]
pub fn put_guest_u64(
    env: &mut FunctionEnvMut<NapiEnv>,
    mem: GuestMem,
    guest_ptr: u32,
    val: u64,
) -> bool {
    mem.write_u64(guest_ptr, val) || write_guest_bytes_slow(env, guest_ptr, &val.to_le_bytes())
}

#[inline(always)]
pub fn put_guest_i64(
    env: &mut FunctionEnvMut<NapiEnv>,
    mem: GuestMem,
    guest_ptr: u32,
    val: i64,
) -> bool {
    put_guest_u64(env, mem, guest_ptr, val as u64)
}

#[inline(always)]
pub fn put_guest_f64(
    env: &mut FunctionEnvMut<NapiEnv>,
    mem: GuestMem,
    guest_ptr: u32,
    val: f64,
) -> bool {
    put_guest_u64(env, mem, guest_ptr, val.to_bits())
}

#[inline(always)]
pub fn put_guest_u8(
    env: &mut FunctionEnvMut<NapiEnv>,
    mem: GuestMem,
    guest_ptr: u32,
    val: u8,
) -> bool {
    mem.write_u8(guest_ptr, val) || write_guest_bytes_slow(env, guest_ptr, &[val])
}

/// A copy of the `len` guest bytes at `src`, charged to the env's budget.
/// `None` if the budget refused it.
///
/// # Safety
/// They must be mapped guest memory.
#[inline]
unsafe fn guest_bytes_at(
    env: &FunctionEnvMut<NapiEnv>,
    src: *const u8,
    len: usize,
) -> Option<HostCopy> {
    let charge = TransientCharge::for_copy(env, len)?;
    let mut out = Vec::with_capacity(len);
    // SAFETY: as the caller ensures; `out` has `len` bytes of capacity, all
    // written before `set_len`.
    unsafe {
        copy_from_guest(src, out.as_mut_ptr(), len);
        out.set_len(len);
    }
    Some(Charged { value: out, charge })
}

/// A charged host copy of `len` guest bytes from `guest_ptr`. `None` if they
/// are not in guest memory or the budget refused the copy.
pub fn read_guest_bytes(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    len: usize,
) -> Option<HostCopy> {
    if let Some(src) = guest_range(env, guest_ptr, len) {
        // SAFETY: the range is mapped guest memory.
        return unsafe { guest_bytes_at(env, src, len) };
    }
    // A guest cannot legitimately reference more bytes than its own linear
    // memory holds, so reject a length that exceeds it before allocating. This
    // bounds the host copy to memory the guest was already charged for and
    // stops a bogus guest-supplied length from allocating gigabytes here (the
    // subsequent bounds-checked read would fail, but only after the `vec!`).
    env.data().memory.as_ref()?;
    if len as u64 > guest_data_size(env) {
        return None;
    }
    let charge = TransientCharge::for_copy(env, len)?;
    let mut out = vec![0u8; len];
    let (state, store) = env.data_and_store_mut();
    state
        .memory
        .as_ref()?
        .view(&store)
        .read(guest_ptr as u64, &mut out)
        .ok()?;
    Some(Charged { value: out, charge })
}

/// Reads a little-endian u32 from guest memory without allocating, for an
/// import that holds its env's window `mem`: through the window, else by the
/// checked path.
#[inline(always)]
pub fn get_guest_u32(
    env: &mut FunctionEnvMut<NapiEnv>,
    mem: GuestMem,
    guest_ptr: u32,
) -> Option<u32> {
    match mem.read_u32(guest_ptr) {
        Some(value) => Some(value),
        None => read_guest_u32_slow(env, guest_ptr),
    }
}

#[cold]
#[inline(never)]
fn read_guest_u32_slow(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32) -> Option<u32> {
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

/// The guest address of a host address inside the guest's memory (the data
/// of a backing store): by the env's window when it holds the address, else
/// by the memory's current base and length.
#[inline]
pub fn host_ptr_to_guest_ptr(env: &mut FunctionEnvMut<NapiEnv>, host_addr: u64) -> Option<u32> {
    if let Some(guest_ptr) = env.data().guest_mem().guest_ptr_of(host_addr) {
        return Some(guest_ptr);
    }
    host_ptr_to_guest_ptr_checked(env, host_addr)
}

#[cold]
#[inline(never)]
fn host_ptr_to_guest_ptr_checked(
    env: &mut FunctionEnvMut<NapiEnv>,
    host_addr: u64,
) -> Option<u32> {
    let (state, store_ref) = env.data_and_store_mut();
    let view = state.memory.as_ref()?.view(&store_ref);
    let host_base = view.data_ptr() as u64;
    let memory_len = view.data_size();
    if host_addr < host_base || host_addr >= host_base + memory_len {
        return None;
    }
    // The memory is longer than this env's window: let the window catch up,
    // so the next backing store up there is found without a view.
    if let Some(heap) = state.guest_heap.as_ref() {
        heap.observe(view.data_ptr(), memory_len);
        state.refresh_guest_mem();
    }
    u32::try_from(host_addr - host_base).ok()
}

pub fn read_guest_u32_array(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    count: usize,
) -> Option<HostCopy<u32>> {
    // Guard the byte-length multiply against overflow; the read below is then
    // clamped to the guest's memory size by `read_guest_bytes`.
    let byte_len = count.checked_mul(4)?;
    let (bytes, mut charge) = read_guest_bytes(env, guest_ptr, byte_len)?.into_parts();
    // The words take as much memory again until the bytes are freed.
    if byte_len >= UNCHARGED_HOST_COPY_BYTES && !charge.add(&env.data().budget, byte_len) {
        return None;
    }
    let mut result = Vec::with_capacity(count);
    for chunk in bytes.chunks_exact(4) {
        result.push(u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]));
    }
    Some(Charged {
        value: result,
        charge,
    })
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
pub fn read_guest_c_string(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32) -> Option<HostCopy> {
    read_guest_c_string_max(env, guest_ptr, usize::MAX)
}

/// Room for a short guest name and its NUL (`read_guest_name_inline`).
pub const GUEST_NAME_INLINE: usize = 128;

/// The guest's NUL-terminated string at `guest_ptr`, copied with its NUL into
/// `out`, for property names: they are short, and this costs no allocation.
/// Returns the length without the NUL. `None` if the string and its NUL do not
/// fit `out`, if the full inline window extends past guest memory, or with
/// no guest heap: the caller then reads it with `read_guest_c_string_max`.
/// A copy that holds a NUL the scan did not see (another guest thread wrote
/// it meanwhile) gives the empty string, as `CString::new(..).unwrap_or_default()`
/// does on that path.
pub fn read_guest_name_inline(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    out: &mut [u8; GUEST_NAME_INLINE],
) -> Option<usize> {
    let src = guest_range(env, guest_ptr, GUEST_NAME_INLINE)?;
    // SAFETY: the range is mapped guest memory.
    let len = unsafe { guest_nul_offset(src, GUEST_NAME_INLINE) }?;
    // SAFETY: `len` < `GUEST_NAME_INLINE` bytes of that range, into `out`.
    unsafe { copy_from_guest(src, out.as_mut_ptr(), len) };
    if out[..len].contains(&0) {
        out[0] = 0;
        return Some(0);
    }
    out[len] = 0;
    Some(len)
}

/// Whether the `len` guest bytes at `src` equal the `len` bytes at `expected`,
/// reading the guest side as `copy_from_guest` does.
///
/// # Safety
/// The `len` bytes at `src` must be mapped guest memory, and `expected` must
/// be valid for `len` bytes.
#[inline]
unsafe fn guest_bytes_equal(mut src: *const u8, mut expected: *const u8, mut len: usize) -> bool {
    unsafe {
        while len >= 8 {
            let guest = (src as *const Unaligned<u64>).read_volatile().0;
            let host = (expected as *const Unaligned<u64>).read().0;
            if guest != host {
                return false;
            }
            src = src.add(8);
            expected = expected.add(8);
            len -= 8;
        }
        while len > 0 {
            if src.read_volatile() != expected.read() {
                return false;
            }
            src = src.add(1);
            expected = expected.add(1);
            len -= 1;
        }
    }
    true
}

/// Room for a numbered name and its NUL ([`NameCache`]).
const NAME_KEY_INLINE: usize = 40;
/// Sets of a [`NameCache`], of two ways each. The provider keeps one string
/// per way and isolate (`kNapiNameKeySlots` in napi_v8_agent.h).
const NAME_CACHE_SETS: usize = 256;

/// The number the host gave a property name, for the bridge's
/// `snapi_bridge_*_named_property_keyed`: `key` identifies the name's bytes
/// in the whole process and is never used for other bytes, and `slot` is
/// where an isolate keeps that name's string. The default, key 0, is a name
/// with no number.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NameKey {
    pub slot: u32,
    pub key: u64,
}

/// The next [`NameKey::key`]. Process-wide, so that a key means the same
/// bytes to every isolate, whichever store numbered them.
static NEXT_NAME_KEY: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);

#[derive(Clone, Copy)]
struct CachedName {
    /// 0 until the name is seen a second time at `guest_ptr`.
    key: u64,
    guest_ptr: u32,
    /// Bytes of the name with its NUL; 0 for an unused way.
    len: u8,
    /// The way of its set that matched last.
    recent: bool,
    bytes: [u8; NAME_KEY_INLINE],
}

impl CachedName {
    const UNUSED: Self = Self {
        key: 0,
        guest_ptr: 0,
        len: 0,
        recent: false,
        bytes: [0; NAME_KEY_INLINE],
    };
}

/// A store's property names by the guest address they were read from.
///
/// A guest passes a property name as a C string, nearly always a literal,
/// so the same address with the same bytes on every call. Reading it takes
/// a scan for its NUL and a copy, and the engine then makes a string of it.
/// A name found here costs one comparison of its bytes instead, and comes
/// with a [`NameKey`] the provider keeps the string under.
///
/// Guest memory is the guest's to change, so an entry is only a guess: a
/// lookup compares the guest's bytes, the NUL included, with the entry's,
/// which is the name the guest passes if they are equal.
#[derive(Default)]
pub(crate) struct NameCache {
    /// Empty until the first name; then `2 * NAME_CACHE_SETS` ways.
    ways: Vec<CachedName>,
}

impl NameCache {
    #[inline]
    fn set_of(guest_ptr: u32) -> usize {
        // Fibonacci hashing: literals lie close together, at any alignment.
        (guest_ptr.wrapping_mul(0x9E37_79B1) >> 24) as usize
    }
}

/// The name the guest passes at `guest_ptr`, if the store's [`NameCache`]
/// has it: copied with its NUL into `out`, with its key. A name seen for the
/// second time gets its key here.
#[inline(always)]
pub fn cached_guest_name(
    env: &mut FunctionEnvMut<NapiEnv>,
    mem: GuestMem,
    guest_ptr: u32,
    out: &mut MaybeUninit<[u8; GUEST_NAME_INLINE]>,
) -> Option<NameKey> {
    const { assert!(NAME_KEY_INLINE <= GUEST_NAME_INLINE && NAME_CACHE_SETS == 256) };
    let first = 2 * NameCache::set_of(guest_ptr);
    let (way, mut key, recent) = {
        let state = env.data();
        let ways = &state.name_cache.ways;
        // `None` for a cache with no name yet; otherwise both ways exist.
        let a = ways.get(first)?;
        let (way, cached) = if a.len != 0 && a.guest_ptr == guest_ptr {
            (first, a)
        } else {
            let b = ways.get(first + 1)?;
            if b.len == 0 || b.guest_ptr != guest_ptr {
                return None;
            }
            (first + 1, b)
        };
        let len = usize::from(cached.len);
        // A name read before lies inside the env's window on guest memory
        // (a length the memory had, at its fixed base); if this one does
        // not, the caller reads it the long way.
        let src = mem.range(guest_ptr, len as u64)?;
        // SAFETY: the `len` bytes at `src` are mapped guest memory, and
        // `len` is at most the size of `bytes`.
        if !unsafe { guest_bytes_equal(src, cached.bytes.as_ptr(), len) } {
            return None;
        }
        // All of `bytes`, for a copy of fixed size: the name ends at its NUL.
        // SAFETY: `out` has room for `GUEST_NAME_INLINE >= NAME_KEY_INLINE`
        // bytes, and the two do not overlap.
        unsafe {
            std::ptr::copy_nonoverlapping(
                cached.bytes.as_ptr(),
                out.as_mut_ptr().cast::<u8>(),
                NAME_KEY_INLINE,
            )
        };
        (way, cached.key, cached.recent)
    };
    if key == 0 || !recent {
        let ways = &mut env.data_mut().name_cache.ways;
        if key == 0 {
            key = NEXT_NAME_KEY.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            ways[way].key = key;
        }
        ways[way].recent = true;
        ways[way ^ 1].recent = false;
    }
    Some(NameKey {
        slot: way as u32,
        key,
    })
}

/// Remembers `name` (with its NUL) as what the guest has at `guest_ptr`, if
/// it is short enough. It gets no key yet: an address whose bytes change on
/// every call (a buffer the guest formats names into) then never makes the
/// provider keep a string.
pub fn remember_guest_name(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, name: &[u8]) {
    if name.is_empty() || name.len() > NAME_KEY_INLINE {
        return;
    }
    let ways = &mut env.data_mut().name_cache.ways;
    if ways.is_empty() {
        ways.resize(2 * NAME_CACHE_SETS, CachedName::UNUSED);
    }
    let first = 2 * NameCache::set_of(guest_ptr);
    // The way that already guesses for this address, else an unused one,
    // else one whose name has no key, else the one that did not match last.
    let way = [first, first + 1]
        .into_iter()
        .find(|&way| ways[way].len != 0 && ways[way].guest_ptr == guest_ptr)
        .or_else(|| [first, first + 1].into_iter().find(|&way| ways[way].len == 0))
        .or_else(|| [first, first + 1].into_iter().find(|&way| ways[way].key == 0))
        .unwrap_or(if ways[first].recent { first + 1 } else { first });
    let entry = &mut ways[way];
    entry.key = 0;
    entry.guest_ptr = guest_ptr;
    entry.len = name.len() as u8;
    entry.recent = false;
    entry.bytes[..name.len()].copy_from_slice(name);
}

/// As `read_guest_c_string`, but also `None` for a string longer than `max`
/// bytes, without a scan past them. Strings the bridge hands to V8 with no
/// length (names, error codes and messages, symbol descriptions) are read with
/// `V8_MAX_STRING_BYTES`.
pub fn read_guest_c_string_max(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    max: usize,
) -> Option<HostCopy> {
    try_read_guest_c_string_max(env, guest_ptr, max)
        .ok()
        .flatten()
}

/// The budget refused a host copy of guest memory.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CopyRefused;

/// As [`read_guest_c_string_max`], telling a copy the budget refused
/// (`Err`) from a string that is not there (`Ok(None)`: a bad pointer, no
/// NUL, or longer than `max`). An import that used to take a missing string
/// as empty must still fail on a refusal, or it acts on a different string.
pub fn try_read_guest_c_string_max(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    max: usize,
) -> Result<Option<HostCopy>, CopyRefused> {
    // Names are short: scan a first chunk in place, which needs no lookup of
    // the memory's size while it lies within the length already seen.
    const CHUNK: usize = 256;
    let chunk = CHUNK.min(max.saturating_add(1));
    let mut scanned = 0;
    if let Some(src) = guest_range(env, guest_ptr, chunk) {
        // SAFETY: the chunk is mapped guest memory.
        if let Some(len) = unsafe { guest_nul_offset(src, chunk) } {
            // SAFETY: the string's bytes are in the chunk.
            return unsafe { guest_bytes_at(env, src, len) }
                .map(Some)
                .ok_or(CopyRefused);
        }
        scanned = chunk;
    }
    // A longer string (script sources can reach megabytes), or one that starts
    // within a chunk of the memory's end: the rest of the memory, in place.
    let start = u64::from(guest_ptr);
    let Some(search) = c_string_search_len(guest_data_size(env), start, max) else {
        return Ok(None);
    };
    if let Some(src) = guest_range(env, guest_ptr, search) {
        // The chunk lay within the memory and `max + 1` bytes, so within
        // `search`. That length comes from this env's view, the chunk's from
        // the heap: check anyway.
        let Some(tail) = search.checked_sub(scanned) else {
            return Ok(None);
        };
        // SAFETY: the range is mapped guest memory, and `scanned` <= `search`.
        let Some(nul) = (unsafe { guest_nul_offset(src.add(scanned), tail) }) else {
            return Ok(None);
        };
        let len = scanned + nul;
        // SAFETY: the string's bytes are in the range.
        return unsafe { guest_bytes_at(env, src, len) }
            .map(Some)
            .ok_or(CopyRefused);
    }
    // No guest heap: through the `MemoryView`. Find the NUL a block at a
    // time, then read the string once.
    let (state, store) = env.data_and_store_mut();
    let Some(memory) = state.memory.clone() else {
        return Ok(None);
    };
    let view = memory.view(&store);
    let end = start + search as u64;
    let mut block = [0u8; 4096];
    let mut offset = start;
    while offset < end {
        let len = (end - offset).min(block.len() as u64) as usize;
        if view.read(offset, &mut block[..len]).is_err() {
            return Ok(None);
        }
        if let Some(nul) = block[..len].iter().position(|&byte| byte == 0) {
            let Ok(prefix) = usize::try_from(offset - start) else {
                return Ok(None);
            };
            // The bytes were just read, so only the budget can refuse them.
            return read_guest_bytes(env, guest_ptr, prefix + nul)
                .map(Some)
                .ok_or(CopyRefused);
        }
        offset += len as u64;
    }
    Ok(None)
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::AtomicBool;
    use std::sync::{Arc, Mutex};

    use wasmer::{AsStoreMut, FunctionEnv, FunctionEnvMut, Memory, MemoryType, Pages, Store};

    use std::mem::MaybeUninit;

    use super::{
        GUEST_NAME_INLINE, V8_MAX_STRING_BYTES, c_string_search_len, copy_from_guest,
        copy_to_guest, get_guest_u32, guest_data_size, guest_nul_offset, read_guest_bytes,
        read_guest_c_string_max, read_guest_name_inline, write_guest_bytes, write_guest_u8,
        write_guest_u32, write_guest_u64,
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
            state.set_guest_heap(heap);
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
                    read_guest_c_string_max(&mut env, 4096, max).map(|copy| copy.into_parts().0),
                    Some(text(max)),
                    "{case}"
                );
                put(&mut env, 4096, &with_nul(max + 1));
                assert_eq!(
                    read_guest_c_string_max(&mut env, 4096, max).map(|copy| copy.into_parts().0),
                    None,
                    "{case}, one more"
                );
                assert_eq!(
                    read_guest_c_string_max(&mut env, 4096, usize::MAX)
                        .map(|copy| copy.into_parts().0),
                    Some(text(max + 1))
                );
                // The NUL is the memory's last byte.
                let at_end = size - max_u32 - 1;
                put(&mut env, at_end, &with_nul(max));
                let case = format!("{path}, max {max}, ending at the end");
                assert_eq!(
                    read_guest_c_string_max(&mut env, at_end, max).map(|copy| copy.into_parts().0),
                    Some(text(max)),
                    "{case}"
                );
                let one_more = size - max_u32 - 2;
                put(&mut env, one_more, &with_nul(max + 1));
                assert_eq!(
                    read_guest_c_string_max(&mut env, one_more, max)
                        .map(|copy| copy.into_parts().0),
                    None,
                    "{case}, one more"
                );
                assert_eq!(
                    read_guest_c_string_max(&mut env, one_more, usize::MAX)
                        .map(|copy| copy.into_parts().0),
                    Some(text(max + 1))
                );
                // `max + 1` bytes up to the end and no NUL.
                put(&mut env, at_end, &text(max + 1));
                let case = format!("{path}, max {max}, unterminated");
                assert_eq!(
                    read_guest_c_string_max(&mut env, at_end, max).map(|copy| copy.into_parts().0),
                    None,
                    "{case}"
                );
                assert_eq!(
                    read_guest_c_string_max(&mut env, at_end, usize::MAX)
                        .map(|copy| copy.into_parts().0),
                    None,
                    "{case}"
                );
            }
        }
    }

    /// `read_guest_name_inline` copies a name of up to `GUEST_NAME_INLINE - 1`
    /// bytes with its NUL and leaves everything else to the caller's other
    /// path: a longer name, one whose buffer-sized window runs past the end
    /// of guest memory, and a memory with no guest heap.
    #[test]
    fn inline_names_are_short_names_only() {
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
            let inline = heap.is_some();
            let mut state = NapiEnv::new(
                ResourceBudget::unlimited(),
                None,
                Arc::new(Mutex::new(EnvRegistry::default())),
                Arc::new(AtomicBool::new(false)),
            );
            state.memory = Some(memory.clone());
            state.set_guest_heap(heap);
            let mut env = FunctionEnv::new(&mut store, state).into_mut(&mut store);
            let size = u32::try_from(guest_data_size(&mut env)).unwrap();
            let put = |env: &mut FunctionEnvMut<NapiEnv>, ptr: u32, bytes: &[u8]| {
                let (state, store) = env.data_and_store_mut();
                let view = state.memory.as_ref().unwrap().view(&store);
                view.write(u64::from(ptr), bytes).unwrap();
            };
            let window = GUEST_NAME_INLINE as u32;
            for len in [
                0,
                1,
                7,
                8,
                9,
                100,
                GUEST_NAME_INLINE - 1,
                GUEST_NAME_INLINE,
                300,
            ] {
                let name: Vec<u8> = (0..len).map(|i| b'a' + (i % 26) as u8).collect();
                // What follows the NUL is not part of the name.
                put(
                    &mut env,
                    4096,
                    &[name.clone(), vec![0], vec![b'#'; 8]].concat(),
                );
                let mut out = [0xffu8; GUEST_NAME_INLINE];
                let read = read_guest_name_inline(&mut env, 4096, &mut out);
                if inline && len < GUEST_NAME_INLINE {
                    assert_eq!(read, Some(len), "length {len}");
                    assert_eq!(&out[..len], &name[..], "length {len}");
                    assert_eq!(out[len], 0, "length {len}");
                } else {
                    assert_eq!(read, None, "length {len}, heap {inline}");
                }
                assert_eq!(
                    read_guest_c_string_max(&mut env, 4096, V8_MAX_STRING_BYTES)
                        .map(|copy| copy.into_parts().0),
                    Some(name),
                    "length {len}"
                );
            }
            // The window is the memory's last bytes: read. One byte later, it
            // runs past the end: the other path's.
            put(&mut env, size - window, b"last\0");
            let mut out = [0xffu8; GUEST_NAME_INLINE];
            assert_eq!(
                read_guest_name_inline(&mut env, size - window, &mut out),
                inline.then_some(4)
            );
            put(&mut env, size - window + 1, b"past\0");
            assert_eq!(
                read_guest_name_inline(&mut env, size - window + 1, &mut out),
                None
            );
            assert_eq!(
                read_guest_c_string_max(&mut env, size - window + 1, V8_MAX_STRING_BYTES)
                    .map(|copy| copy.into_parts().0),
                Some(b"past".to_vec())
            );
            assert_eq!(read_guest_name_inline(&mut env, size, &mut out), None);
        }
    }

    /// A u32 as an import reads one: by the env's window, else checked.
    fn read_guest_u32(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32) -> Option<u32> {
        let mem = env.data().guest_mem();
        get_guest_u32(env, mem, guest_ptr)
    }

    fn env_over(
        store: &mut Store,
        memory: &Memory,
        heap: Option<Arc<GuestHeap>>,
    ) -> FunctionEnv<NapiEnv> {
        let mut state = NapiEnv::new(
            ResourceBudget::unlimited(),
            None,
            Arc::new(Mutex::new(EnvRegistry::default())),
            Arc::new(AtomicBool::new(false)),
        );
        state.memory = Some(memory.clone());
        state.set_guest_heap(heap);
        FunctionEnv::new(store, state)
    }

    /// The env's window holds exactly the ranges inside the memory length it
    /// has seen, for every access size; an env with no heap has an empty one.
    /// The scalar readers and writers agree with the checked path on both
    /// sides of the window's end and of the memory's.
    #[test]
    fn window_holds_ranges_inside_the_seen_length_only() {
        let mut store = Store::default();
        let ty = MemoryType::new(Pages(2), Some(Pages(2048)), false);
        let memory = Memory::new(&mut store, ty).expect("create owned memory");
        let heap = GuestHeap::new(
            &mut store.as_store_mut(),
            &memory,
            ResourceBudget::unlimited(),
        )
        .expect("guest heap");
        for heap in [Some(heap), None] {
            let windowed = heap.is_some();
            let mut env = env_over(&mut store, &memory, heap).into_mut(&mut store);
            let size = u32::try_from(guest_data_size(&mut env)).unwrap();
            // A view of the env's own, to check what was written.
            let peek = |env: &mut FunctionEnvMut<NapiEnv>, ptr: u32, len: usize| {
                let (state, store) = env.data_and_store_mut();
                let mut bytes = vec![0u8; len];
                let view = state.memory.as_ref().unwrap().view(&store);
                view.read(u64::from(ptr), &mut bytes).unwrap();
                bytes
            };
            // Touch the last byte through the checked path, so the window
            // (when there is one) has seen the memory's whole length.
            assert!(write_guest_bytes(&mut env, size - 1, &[0]));
            let mem = env.data().guest_mem();

            for at in [0u32, 1, 4095, size - 9, size - 8, size - 5, size - 4, size - 1] {
                let fits = |len: u32| windowed && at + len <= size;
                assert_eq!(mem.write_u8(at, 0x5a), fits(1), "u8 at {at}");
                assert_eq!(mem.write_u32(at, 0xa1b2_c3d4), fits(4), "u32 at {at}");
                assert_eq!(
                    mem.read_u32(at),
                    fits(4).then_some(0xa1b2_c3d4),
                    "u32 at {at}"
                );
                assert_eq!(
                    mem.write_u64(at, 0x0102_0304_0506_0708),
                    fits(8),
                    "u64 at {at}"
                );
                assert_eq!(
                    mem.write_u32s(at, &[7, 8]),
                    fits(8),
                    "two u32 at {at}"
                );
                let mut two = [0u32; 2];
                assert_eq!(mem.read_u32s(at, &mut two), fits(8), "two u32 at {at}");
                if fits(8) {
                    assert_eq!(two, [7, 8]);
                    assert_eq!(peek(&mut env, at, 8), [7, 0, 0, 0, 8, 0, 0, 0]);
                }
                assert_eq!(mem.out_buffer(at, 8).is_some(), fits(8));
                let mut copy = MaybeUninit::<[u8; 16]>::uninit();
                assert_eq!(mem.read_into(at, 8, &mut copy), fits(8));
                assert!(!mem.read_into(at, 17, &mut copy), "more than the room");

                // The env's own writers: the same result with or without a
                // window, by the window or by the checked path.
                assert_eq!(write_guest_u8(&mut env, at, 0x11), at + 1 <= size);
                assert_eq!(write_guest_u32(&mut env, at, 0x2233_4455), at + 4 <= size);
                assert_eq!(
                    read_guest_u32(&mut env, at),
                    (at + 4 <= size).then_some(0x2233_4455)
                );
                assert_eq!(
                    write_guest_u64(&mut env, at, 0x0807_0605_0403_0201),
                    at + 8 <= size
                );
                if at + 8 <= size {
                    assert_eq!(peek(&mut env, at, 8), [1, 2, 3, 4, 5, 6, 7, 8]);
                }
            }
            // No bytes, and addresses whose end does not fit 32 bits.
            assert!(!mem.write_u32s(16, &[]));
            assert!(!mem.read_into(16, 0, &mut MaybeUninit::<[u8; 8]>::uninit()));
            assert!(mem.out_buffer(16, 0).is_none());
            assert!(!mem.write_u32(u32::MAX, 1));
            assert!(!mem.write_u64(u32::MAX - 3, 1));
            assert!(mem.out_buffer(u32::MAX, u64::MAX).is_none());
            assert!(mem.out_buffer(1, u64::MAX).is_none());
            assert!(!write_guest_u32(&mut env, u32::MAX, 1));
            assert_eq!(read_guest_u32(&mut env, size - 3), None);
        }
    }

    /// A window taken before the memory grew still holds what it held, and
    /// nothing of the new pages; the env's writers reach those through the
    /// checked path, after which a new window holds them too.
    #[test]
    fn window_follows_the_memory_as_it_grows() {
        let mut store = Store::default();
        let ty = MemoryType::new(Pages(2), Some(Pages(2048)), false);
        let memory = Memory::new(&mut store, ty).expect("create owned memory");
        let heap = GuestHeap::new(
            &mut store.as_store_mut(),
            &memory,
            ResourceBudget::unlimited(),
        )
        .expect("guest heap");
        let func_env = env_over(&mut store, &memory, Some(heap));
        let (old, before) = {
            let mut env = func_env.clone().into_mut(&mut store);
            let old = u32::try_from(guest_data_size(&mut env)).unwrap();
            assert!(write_guest_bytes(&mut env, old - 1, &[0]));
            (old, env.data().guest_mem())
        };
        assert!(before.write_u32(old - 4, 1));
        memory.grow(&mut store, Pages(1)).expect("grow");
        let mut env = func_env.into_mut(&mut store);
        let new = u32::try_from(guest_data_size(&mut env)).unwrap();
        assert_eq!(new, old + 65536);

        // The old window: as before the grow.
        assert!(before.write_u32(old - 4, 2));
        assert!(!before.write_u32(old - 3, 3));
        assert!(!before.write_u32(old, 3));
        // The env has not looked since: its window is the old one.
        assert!(!env.data().guest_mem().write_u32(new - 4, 4));
        // Its writers get there all the same, and then the window does.
        assert!(write_guest_u32(&mut env, new - 4, 5));
        assert_eq!(read_guest_u32(&mut env, new - 4), Some(5));
        let after = env.data().guest_mem();
        assert!(after.write_u32(new - 4, 6));
        assert_eq!(after.read_u32(new - 4), Some(6));
        assert!(!after.write_u32(new - 3, 7));
        assert_eq!(read_guest_u32(&mut env, old - 4), Some(2));
    }

    /// The window's one-pass string read agrees with the checked reads on
    /// every length around its room and every alignment, and its copy ends
    /// in the NUL it found.
    #[test]
    fn window_c_strings_match_the_checked_reads() {
        let mut store = Store::default();
        let ty = MemoryType::new(Pages(2), Some(Pages(2048)), false);
        let memory = Memory::new(&mut store, ty).expect("create owned memory");
        let heap = GuestHeap::new(
            &mut store.as_store_mut(),
            &memory,
            ResourceBudget::unlimited(),
        )
        .expect("guest heap");
        let mut env = env_over(&mut store, &memory, Some(heap)).into_mut(&mut store);
        let size = u32::try_from(guest_data_size(&mut env)).unwrap();
        assert!(write_guest_bytes(&mut env, size - 1, &[0]));
        let mem = env.data().guest_mem();
        const ROOM: usize = 64;
        let fillers: [&[u8]; 3] = [&[0x01], &[0x80, 0xff], &[b'a', 0x01, 0x80, 0x7f, 0xff]];
        for filler in fillers {
            for at in 0..9u32 {
                for len in 0..=ROOM + 9 {
                    let ptr = 4096 + at;
                    let name: Vec<u8> = (0..len).map(|i| filler[i % filler.len()]).collect();
                    // Bytes after the NUL, with another NUL among them.
                    let stored = [name.clone(), vec![0], vec![0x80, 0, 0x01, b'#']].concat();
                    assert!(write_guest_bytes(&mut env, ptr, &stored));
                    let mut out = MaybeUninit::<[u8; ROOM]>::new([0xee; ROOM]);
                    let read = mem.read_c_str(ptr, &mut out);
                    // SAFETY: initialized above.
                    let out = unsafe { out.assume_init() };
                    let case = format!("filler {filler:x?}, +{at}, {len} bytes");
                    if len < ROOM {
                        assert_eq!(read, Some(len), "{case}");
                        assert_eq!(&out[..len], &name[..], "{case}");
                        assert_eq!(out[len], 0, "{case}");
                    } else {
                        assert_eq!(read, None, "{case}");
                    }
                    assert_eq!(
                        read_guest_c_string_max(&mut env, ptr, V8_MAX_STRING_BYTES)
                            .map(|copy| copy.into_parts().0),
                        Some(name.clone()),
                        "{case}"
                    );
                    // The counted copy takes the same bytes.
                    if (1..=ROOM).contains(&len) {
                        let mut copy = MaybeUninit::<[u8; ROOM]>::new([0xee; ROOM]);
                        assert!(mem.read_into(ptr, len, &mut copy), "{case}");
                        // SAFETY: initialized above.
                        let copy = unsafe { copy.assume_init() };
                        assert_eq!(&copy[..len], &name[..], "{case}");
                        assert!(copy[len..].iter().all(|&b| b == 0xee), "{case}");
                        assert_eq!(
                            read_guest_bytes(&mut env, ptr, len).map(|copy| copy.into_parts().0),
                            Some(name),
                            "{case}"
                        );
                    }
                }
            }
        }
        // The room must lie inside the window whole.
        assert!(write_guest_bytes(&mut env, size - ROOM as u32, b"last\0"));
        let mut out = MaybeUninit::<[u8; ROOM]>::uninit();
        assert_eq!(mem.read_c_str(size - ROOM as u32, &mut out), Some(4));
        assert!(write_guest_bytes(&mut env, size - ROOM as u32 + 1, b"past\0"));
        assert_eq!(mem.read_c_str(size - ROOM as u32 + 1, &mut out), None);
        assert_eq!(mem.read_c_str(size, &mut out), None);
        assert_eq!(mem.read_c_str(u32::MAX, &mut out), None);
    }
}
