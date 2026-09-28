// ============================================================
// Guest memory helpers
// ============================================================

use std::{ffi::CString, ops::Deref, sync::Arc};

use wasmer::{AsStoreMut, FunctionEnvMut};

use crate::{
    NapiEnv,
    budget::{Pool, ResourceBudget},
};

/// A host copy of guest data whose budget charge survives for its full lifetime.
/// Keeping the owner with the bytes makes nested and concurrent N-API calls
/// account their peak copies instead of briefly charging only the read itself.
pub(crate) struct HostCopy<T> {
    data: Vec<T>,
    budget: Option<Arc<ResourceBudget>>,
    charged: u64,
    reserved_elements: usize,
}

impl<T: Default> HostCopy<T> {
    pub(crate) fn zeroed(budget: Arc<ResourceBudget>, count: usize) -> Option<Self> {
        let mut copy = Self::with_capacity(budget, count)?;
        copy.data.resize_with(count, T::default);
        Some(copy)
    }
}

impl<T> HostCopy<T> {
    pub(crate) fn with_capacity(budget: Arc<ResourceBudget>, count: usize) -> Option<Self> {
        let charged = u64::try_from(count.checked_mul(std::mem::size_of::<T>())?).ok()?;
        budget.try_charge(Pool::HostTransient, charged).ok()?;
        let mut copy = Self {
            data: Vec::new(),
            budget: Some(budget),
            charged,
            reserved_elements: count,
        };
        copy.data.try_reserve_exact(count).ok()?;
        Some(copy)
    }

    pub(crate) fn empty() -> Self {
        Self {
            data: Vec::new(),
            budget: None,
            charged: 0,
            reserved_elements: 0,
        }
    }

    pub(crate) fn as_mut_ptr(&mut self) -> *mut T {
        self.data.as_mut_ptr()
    }

    pub(crate) fn push(&mut self, value: T) {
        assert!(self.data.len() < self.reserved_elements);
        self.data.push(value);
    }
}

impl HostCopy<CString> {
    /// Keep copied property names charged while the descriptor array holds
    /// them; each name may be as long as MAX_GUEST_CSTRING_SCAN.
    pub(crate) fn push_cstring(&mut self, bytes: &[u8]) -> Option<()> {
        if self.data.len() == self.reserved_elements {
            return None;
        }
        let additional = u64::try_from(bytes.len().checked_add(1)?).ok()?;
        let budget = self.budget.as_ref()?;
        budget.try_charge(Pool::HostTransient, additional).ok()?;
        self.charged += additional;
        self.data.push(CString::new(bytes).unwrap_or_default());
        Some(())
    }
}

impl<T> Default for HostCopy<T> {
    fn default() -> Self {
        Self::empty()
    }
}

impl<T> Deref for HostCopy<T> {
    type Target = Vec<T>;

    fn deref(&self) -> &Self::Target {
        &self.data
    }
}

impl<T> Drop for HostCopy<T> {
    fn drop(&mut self) {
        // Release the allocation before its budget charge so a concurrent
        // reservation cannot observe bytes that are still live as free.
        drop(std::mem::take(&mut self.data));
        if let Some(budget) = &self.budget {
            budget.uncharge(Pool::HostTransient, self.charged);
        }
    }
}

pub fn write_guest_bytes(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, data: &[u8]) -> bool {
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
    guest_ptr: i32,
    len: usize,
) -> Option<HostCopy<u8>> {
    if guest_ptr < 0 {
        return None;
    }
    let (state, store) = env.data_and_store_mut();
    let memory = state.memory.clone()?;
    let view = memory.view(&store);
    // Validate the entire range before allocating. Checking only `len` lets a
    // guest point near the end of memory and request a huge host allocation
    // that the subsequent bounds-checked read would reject too late.
    if (guest_ptr as u64).checked_add(u64::try_from(len).ok()?)? > view.data_size() {
        return None;
    }
    let mut out = HostCopy::zeroed(Arc::clone(&state.budget), len)?;
    view.read(guest_ptr as u64, &mut out.data).ok()?;
    Some(out)
}

/// Copy a bounded diagnostic string supplied to `napi_fatal_error`.
/// `NAPI_AUTO_LENGTH` is a NUL-terminated string; explicit lengths may
/// include NUL bytes and are deliberately kept intact in the diagnostic.
pub(crate) fn read_guest_fatal_text(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: i32,
    len: i32,
) -> String {
    const MAX_DIAGNOSTIC_BYTES: usize = 4096;
    if guest_ptr <= 0 {
        return "(null)".to_owned();
    }
    let bytes = if len == -1 {
        read_guest_c_string(env, guest_ptr)
    } else {
        usize::try_from(len)
            .ok()
            .and_then(|len| read_guest_bytes(env, guest_ptr, len.min(MAX_DIAGNOSTIC_BYTES)))
    };
    bytes
        .map(|bytes| {
            String::from_utf8_lossy(&bytes[..bytes.len().min(MAX_DIAGNOSTIC_BYTES)]).into_owned()
        })
        .unwrap_or_else(|| "(invalid guest string)".to_owned())
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
    guest_ptr: i32,
    count: usize,
) -> Option<HostCopy<u32>> {
    // Guard the byte-length multiply against overflow; the read below is then
    // clamped to the guest's memory size by `read_guest_bytes`.
    let byte_len = count.checked_mul(4)?;
    let bytes = read_guest_bytes(env, guest_ptr, byte_len)?;
    let mut result = HostCopy::zeroed(Arc::clone(&env.data().budget), count)?;
    for (slot, chunk) in result.data.iter_mut().zip(bytes.chunks_exact(4)) {
        *slot = u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
    }
    Some(result)
}

pub fn read_guest_c_string(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: i32,
) -> Option<HostCopy<u8>> {
    if guest_ptr < 0 {
        return None;
    }
    let len = {
        let (state, store) = env.data_and_store_mut();
        let memory = state.memory.clone()?;
        let view = memory.view(&store);
        let mut len = None;
        for i in 0..super::MAX_GUEST_CSTRING_SCAN {
            let mut b = [0u8; 1];
            view.read(guest_ptr as u64 + i as u64, &mut b).ok()?;
            if b[0] == 0 {
                len = Some(i);
                break;
            }
        }
        len?
    };
    read_guest_bytes(env, guest_ptr, len)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn host_copies_charge_for_their_lifetime_and_rollback_failed_allocations() {
        let budget = ResourceBudget::with_memory_limit(8);
        let first = HostCopy::<u8>::zeroed(Arc::clone(&budget), 6).unwrap();
        assert_eq!(budget.snapshot().host_transient, 6);
        assert!(HostCopy::<u32>::zeroed(Arc::clone(&budget), 1).is_none());
        assert_eq!(budget.snapshot().host_transient, 6);
        drop(first);
        assert_eq!(budget.snapshot().host_transient, 0);

        let unlimited = ResourceBudget::unlimited();
        assert!(HostCopy::<u8>::zeroed(Arc::clone(&unlimited), usize::MAX).is_none());
        assert_eq!(unlimited.snapshot().host_transient, 0);

        let array_bytes = 2 * std::mem::size_of::<CString>() as u64;
        let names_budget = ResourceBudget::with_memory_limit(array_bytes + 5);
        let mut names = HostCopy::<CString>::with_capacity(Arc::clone(&names_budget), 2).unwrap();
        assert_eq!(names_budget.snapshot().host_transient, array_bytes);
        assert!(names.push_cstring(b"ab").is_some());
        assert_eq!(names_budget.snapshot().host_transient, array_bytes + 3);
        assert!(names.push_cstring(b"too long").is_none());
        assert_eq!(names_budget.snapshot().host_transient, array_bytes + 3);
        drop(names);
        assert_eq!(names_budget.snapshot().host_transient, 0);
    }
}
