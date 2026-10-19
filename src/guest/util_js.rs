// ============================================================
// Guest memory helpers
// ============================================================
//
// Every N-API import touches guest memory: it reads arguments (property names,
// string bytes) and writes results (handles, statuses). Wasmer's JS backend
// `MemoryView` performs each access as several wasm-bindgen calls (`buffer`,
// `byteLength`, a new `Uint8Array`, `length`, `subarray`, the copy), and the
// C string reader repeated that per byte. These helpers do one host call per
// access over a view cached per guest memory.

use wasm_bindgen::{JsValue, prelude::wasm_bindgen};
use wasmer::{FunctionEnvMut, js::AsJs};

use crate::{
    GuestBackingStoreMapping, HostBufferCopy, NapiEnv, snapi::snapi_bridge_create_reference,
};

const JS_BACKING_TOKEN_MARKER: u64 = 1 << 63;
const JS_BACKING_TOKEN_OFFSET_MASK: u64 = u32::MAX as u64;

#[wasm_bindgen(inline_js = r#"
// One Uint8Array per guest memory, replaced whenever an access reaches past it.
// A shared memory never detaches: after growth its old view stays valid for its
// old length and `buffer` yields a longer one. A non-shared memory detaches on
// growth, which leaves the old view empty. Either way the access refreshes.
const views = new WeakMap();
function view(memory, end) {
  let bytes = views.get(memory);
  if (bytes === undefined || end > bytes.length) {
    bytes = new Uint8Array(memory.buffer);
    views.set(memory, bytes);
  }
  return bytes;
}
export function wasmer_napi_guest_byte_length(memory) {
  return memory.buffer.byteLength;
}
export function wasmer_napi_guest_read(memory, ptr, out) {
  const end = ptr + out.length;
  const bytes = view(memory, end);
  if (end > bytes.length) return false;
  out.set(bytes.subarray(ptr, end));
  return true;
}
export function wasmer_napi_guest_write(memory, ptr, data) {
  const end = ptr + data.length;
  const bytes = view(memory, end);
  if (end > bytes.length) return false;
  bytes.set(data, ptr);
  return true;
}
export function wasmer_napi_guest_write_u32(memory, ptr, value) {
  const bytes = view(memory, ptr + 4);
  if (ptr + 4 > bytes.length) return false;
  bytes[ptr] = value;
  bytes[ptr + 1] = value >>> 8;
  bytes[ptr + 2] = value >>> 16;
  bytes[ptr + 3] = value >>> 24;
  return true;
}
// Length of the NUL-terminated string at `ptr`, scanning at most `max` bytes;
// -1 if no terminator lies within that bound and the memory.
export function wasmer_napi_guest_c_string_length(memory, ptr, max) {
  let bytes = view(memory, ptr + 1);
  if (ptr >= bytes.length) return -1;
  // The terminator may lie past a view that predates growth.
  if (bytes.length - ptr < max) bytes = view(memory, Math.min(ptr + max, memory.buffer.byteLength));
  const length = bytes.subarray(ptr, Math.min(bytes.length, ptr + max)).indexOf(0);
  return length;
}
"#)]
extern "C" {
    fn wasmer_napi_guest_byte_length(memory: &JsValue) -> f64;
    fn wasmer_napi_guest_read(memory: &JsValue, ptr: u32, out: &mut [u8]) -> bool;
    fn wasmer_napi_guest_write(memory: &JsValue, ptr: u32, data: &[u8]) -> bool;
    fn wasmer_napi_guest_write_u32(memory: &JsValue, ptr: u32, value: u32) -> bool;
    fn wasmer_napi_guest_c_string_length(memory: &JsValue, ptr: u32, max: u32) -> i32;
}

/// The guest's `WebAssembly.Memory`, a cheap externref clone.
fn guest_memory(env: &mut FunctionEnvMut<NapiEnv>) -> Option<JsValue> {
    let (state, store) = env.data_and_store_mut();
    Some(state.memory.as_ref()?.as_jsvalue(&store))
}

pub fn write_guest_bytes(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, data: &[u8]) -> bool {
    let Some(memory) = guest_memory(env) else {
        return false;
    };
    wasmer_napi_guest_write(&memory, guest_ptr, data)
}

pub fn write_guest_u32(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: u32) -> bool {
    let Some(memory) = guest_memory(env) else {
        return false;
    };
    wasmer_napi_guest_write_u32(&memory, guest_ptr, val)
}

pub fn write_guest_i32(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: i32) -> bool {
    write_guest_u32(env, guest_ptr, val as u32)
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
) -> Option<Vec<u8>> {
    if guest_ptr < 0 {
        return None;
    }
    let memory = guest_memory(env)?;
    // Reject an impossible guest range before allocating the host snapshot.
    // The bounds-checked read below is too late: `vec![0; len]` may otherwise
    // abort the host on a forged multi-gigabyte length.
    if (guest_ptr as u64).checked_add(len as u64)? > wasmer_napi_guest_byte_length(&memory) as u64 {
        return None;
    }
    let mut out = vec![0u8; len];
    wasmer_napi_guest_read(&memory, guest_ptr as u32, &mut out).then_some(out)
}

pub fn guest_data_size(env: &mut FunctionEnvMut<NapiEnv>) -> u64 {
    let Some(memory) = guest_memory(env) else {
        return 0;
    };
    wasmer_napi_guest_byte_length(&memory) as u64
}

pub fn allocate_guest_buffer(env: &mut FunctionEnvMut<NapiEnv>, byte_len: usize) -> Option<u32> {
    // Exact-size large transfers avoid doubling peak memory for package
    // archives and Wasm binaries while retaining cheap pooled size classes for
    // the small buffers used by ordinary Node I/O.
    let requested = byte_len.max(1);
    let required_capacity = if requested >= 16 * 1024 * 1024 {
        requested
    } else {
        requested.checked_next_power_of_two()?
    };
    let pooled = env
        .data()
        .guest_buffer_pool
        .iter()
        .enumerate()
        .filter(|(_, (_, capacity))| *capacity >= required_capacity)
        .min_by_key(|(_, (_, capacity))| *capacity)
        .map(|(index, _)| index);
    let (guest_ptr, capacity) = if let Some(index) = pooled {
        env.data_mut().guest_buffer_pool.swap_remove(index)
    } else {
        let malloc_fn = env.data().malloc_fn.clone()?;
        let allocation_len = i32::try_from(required_capacity).ok()?;
        let guest_ptr: i32 = {
            let (_, mut store_ref) = env.data_and_store_mut();
            match malloc_fn.call(&mut store_ref, allocation_len) {
                Ok(ptr) => ptr,
                Err(error) => {
                    eprintln!(
                        "[wasmer-napi] guest buffer allocation trapped: requested={required_capacity} allocated={} error={error}",
                        env.data().guest_buffer_allocated_bytes,
                    );
                    return None;
                }
            }
        };
        if guest_ptr <= 0 {
            eprintln!(
                "[wasmer-napi] guest buffer allocation failed: requested={required_capacity} allocated={} result={guest_ptr}",
                env.data().guest_buffer_allocated_bytes,
            );
            return None;
        }
        let allocated = env
            .data()
            .guest_buffer_allocated_bytes
            .saturating_add(required_capacity);
        env.data_mut().guest_buffer_allocated_bytes = allocated;
        (guest_ptr as u32, required_capacity)
    };
    env.data_mut()
        .guest_buffer_capacities
        .insert(guest_ptr, capacity);
    Some(guest_ptr)
}

pub fn allocate_guest_bytes(env: &mut FunctionEnvMut<NapiEnv>, data: &[u8]) -> Option<u32> {
    let guest_ptr = allocate_guest_buffer(env, data.len())?;
    if !data.is_empty() && !write_guest_bytes(env, guest_ptr, data) {
        recycle_guest_allocation(env, guest_ptr);
        return None;
    }
    Some(guest_ptr)
}

pub fn recycle_guest_allocation(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32) -> bool {
    if guest_ptr == 0 {
        return true;
    }
    let Some(capacity) = env.data_mut().guest_buffer_capacities.remove(&guest_ptr) else {
        return false;
    };
    env.data_mut().guest_buffer_pool.push((guest_ptr, capacity));
    true
}

pub fn forget_guest_allocation(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32) {
    env.data_mut().guest_buffer_capacities.remove(&guest_ptr);
}

pub fn host_ptr_to_guest_ptr(env: &mut FunctionEnvMut<NapiEnv>, host_addr: u64) -> Option<u32> {
    #[cfg(feature = "js")]
    {
        let _ = (env, host_addr);
        return None;
    }

    #[cfg(not(feature = "js"))]
    {
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
}

pub fn resolve_guest_backing_store_mapping(
    mapping: &GuestBackingStoreMapping,
    host_addr: u64,
    byte_len: usize,
) -> Option<u32> {
    let delta = usize::try_from(host_addr.checked_sub(mapping.host_addr)?).ok()?;
    let end = delta.checked_add(byte_len)?;
    if end > mapping.byte_len {
        return None;
    }
    let guest_delta = u32::try_from(delta).ok()?;
    mapping.guest_ptr.checked_add(guest_delta)
}

pub fn resolve_guest_backing_store_token(
    env: &NapiEnv,
    guest_env: u32,
    _handle_id: u32,
    backing_store_token: u64,
    host_addr: u64,
    byte_len: usize,
) -> Option<u32> {
    if backing_store_token & JS_BACKING_TOKEN_MARKER != 0 {
        if let Some(mapping) = env
            .guest_data_backing_stores
            .get(&(guest_env, backing_store_token))
            && byte_len <= mapping.byte_len
        {
            return Some(mapping.guest_ptr);
        }
        {
            let base_token = backing_store_token & !JS_BACKING_TOKEN_OFFSET_MASK;
            let byte_offset = (backing_store_token & JS_BACKING_TOKEN_OFFSET_MASK) as usize;
            if let Some(mapping) = env.guest_data_backing_stores.get(&(guest_env, base_token)) {
                let end = byte_offset.checked_add(byte_len)?;
                if end <= mapping.byte_len {
                    return mapping
                        .guest_ptr
                        .checked_add(u32::try_from(byte_offset).ok()?);
                }
            }
        }
        return None;
    }

    resolve_guest_backing_store_mapping(
        env.guest_data_backing_stores
            .get(&(guest_env, backing_store_token))?,
        host_addr,
        byte_len,
    )
}

pub fn resolve_or_copy_host_data_to_guest(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_env: u32,
    handle_id: u32,
    backing_store_token: u64,
    host_addr: u64,
    byte_len: usize,
    covers_full_backing_store: bool,
) -> Option<u32> {
    if backing_store_token != 0
        && let Some(guest_data_ptr) = resolve_guest_backing_store_token(
            env.data(),
            guest_env,
            handle_id,
            backing_store_token,
            host_addr,
            byte_len,
        )
    {
        env.data_mut()
            .guest_data_ptrs
            .insert((guest_env, handle_id), guest_data_ptr);
        return Some(guest_data_ptr);
    }
    if let Some(&guest_data_ptr) = env.data().guest_data_ptrs.get(&(guest_env, handle_id)) {
        return Some(guest_data_ptr);
    }
    if host_addr == 0 {
        return Some(0);
    }
    if let Some(guest_ptr) = host_ptr_to_guest_ptr(env, host_addr) {
        return Some(guest_ptr);
    }
    if byte_len == 0 {
        return Some(0);
    }
    let host_slice = unsafe { std::slice::from_raw_parts(host_addr as *const u8, byte_len) };
    let guest_ptr = allocate_guest_bytes(env, host_slice)?;
    let snapi_env = env.data().resolve_napi_env(guest_env as i32);
    if snapi_env.is_null() {
        forget_guest_allocation(env, guest_ptr);
        return None;
    }
    let mut host_reference_id = 0u32;
    if unsafe { snapi_bridge_create_reference(snapi_env, handle_id, 1, &mut host_reference_id) }
        != 0
    {
        forget_guest_allocation(env, guest_ptr);
        return None;
    }
    if backing_store_token != 0 {
        env.data_mut().guest_data_backing_stores.insert(
            (guest_env, backing_store_token),
            GuestBackingStoreMapping {
                host_addr,
                guest_ptr,
                byte_len,
                covers_full_backing_store,
            },
        );
    }
    env.data_mut()
        .guest_data_ptrs
        .insert((guest_env, handle_id), guest_ptr);
    let owner_id = env.data().current_host_buffer_owner();
    env.data_mut().host_buffer_copies.push(HostBufferCopy {
        owner_id,
        guest_env,
        handle_id,
        host_reference_id,
        backing_store_token,
        guest_ptr,
        byte_len,
        guest_allocation_recyclable: true,
        persistent: false,
        reference_holds: 0,
        // Standard N-API pointers are writable. Synchronization is phase based:
        // publish before host JS runs, refresh before native code resumes.
        needs_flush: true,
    });
    Some(guest_ptr)
}

pub fn read_guest_u32_array(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: i32,
    count: usize,
) -> Option<Vec<u32>> {
    let bytes = read_guest_bytes(env, guest_ptr, count.checked_mul(4)?)?;
    let mut result = Vec::with_capacity(count);
    for chunk in bytes.chunks_exact(4) {
        result.push(u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]));
    }
    Some(result)
}

pub fn read_guest_c_string(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: i32) -> Option<Vec<u8>> {
    if guest_ptr < 0 {
        return None;
    }
    let memory = guest_memory(env)?;
    let max = u32::try_from(super::MAX_GUEST_CSTRING_SCAN).unwrap_or(u32::MAX);
    let len = usize::try_from(wasmer_napi_guest_c_string_length(&memory, guest_ptr as u32, max)).ok()?;
    let mut out = vec![0u8; len];
    wasmer_napi_guest_read(&memory, guest_ptr as u32, &mut out).then_some(out)
}
