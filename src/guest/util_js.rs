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
export function wasmer_napi_guest_write_u8(memory, ptr, value) {
  const bytes = view(memory, ptr + 1);
  if (ptr + 1 > bytes.length) return false;
  bytes[ptr] = value;
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
// 64-bit stores go through these scratch arrays, which also covers unaligned
// pointers.
const scratch = new DataView(new ArrayBuffer(8));
const scratchBytes = new Uint8Array(scratch.buffer);
function write8(memory, ptr) {
  const bytes = view(memory, ptr + 8);
  if (ptr + 8 > bytes.length) return false;
  bytes.set(scratchBytes, ptr);
  return true;
}
export function wasmer_napi_guest_write_u64(memory, ptr, low, high) {
  scratch.setUint32(0, low, true);
  scratch.setUint32(4, high, true);
  return write8(memory, ptr);
}
export function wasmer_napi_guest_write_f64(memory, ptr, value) {
  scratch.setFloat64(0, value, true);
  return write8(memory, ptr);
}
// napi_get_value_{int32,uint32,int64,double}: stores `value` at `ptr` with
// Rust's saturating float-to-int casts (NaN becomes 0). `kind` is 0 for i32,
// 1 for u32, 2 for i64 and 3 for f64. Returns false for a non-number or a
// store outside guest memory.
export function wasmer_napi_guest_write_number(memory, value, ptr, kind) {
  if (typeof value !== 'number') return false;
  if (kind === 3) return wasmer_napi_guest_write_f64(memory, ptr, value);
  const x = value !== value ? 0 : Math.trunc(value);
  if (kind === 0) return wasmer_napi_guest_write_u32(memory, ptr, x <= -2147483648 ? -2147483648 : x >= 2147483647 ? 2147483647 : x);
  if (kind === 1) return wasmer_napi_guest_write_u32(memory, ptr, x <= 0 ? 0 : x >= 4294967295 ? 4294967295 : x);
  scratch.setBigInt64(0, x <= -9223372036854775808 ? -9223372036854775808n : x >= 9223372036854775807 ? 9223372036854775807n : BigInt(x), true);
  return write8(memory, ptr);
}
// Length in units (1 or 2 bytes) of the NUL-terminated string at `ptr`,
// scanning at most `max` units; -1 if no terminator lies within that bound and
// the memory.
function terminated(memory, ptr, unit, max) {
  let bytes = view(memory, ptr + unit);
  for (;;) {
    const limit = Math.min(bytes.length, ptr + max * unit);
    if (unit === 1) {
      const length = bytes.subarray(ptr, limit).indexOf(0);
      if (length >= 0) return length;
    } else {
      for (let i = ptr; i + 1 < limit; i += 2) if (bytes[i] === 0 && bytes[i + 1] === 0) return (i - ptr) / 2;
    }
    if (limit === ptr + max * unit) return -1;
    // The view may predate growth; retry once with the current memory.
    const current = view(memory, bytes.length + 1);
    if (current.length <= bytes.length) return -1;
    bytes = current;
  }
}
export function wasmer_napi_guest_c_string_length(memory, ptr, max) {
  return terminated(memory, ptr, 1, max);
}

// Strings. `encoding` is 0 for UTF-8, 1 for Latin-1 and 2 for UTF-16, with
// lengths in code units (bytes for the first two).
const utf8Decoder = new TextDecoder();
const utf8Encoder = new TextEncoder();
const CHUNK = 8192;
function fromCodes(codes) {
  if (codes.length <= CHUNK) return String.fromCharCode.apply(null, codes);
  let text = '';
  for (let i = 0; i < codes.length; i += CHUNK) text += String.fromCharCode.apply(null, codes.subarray(i, i + CHUNK));
  return text;
}
function decodeUtf8(bytes, start, end) {
  // Short ASCII (property names, most DOM strings) is decoded in place;
  // TextDecoder rejects shared memory, so anything else is decoded from a copy.
  if (end - start <= 64) {
    let i = start;
    while (i < end && bytes[i] < 0x80) i++;
    if (i === end) return String.fromCharCode.apply(null, bytes.subarray(start, end));
  }
  return utf8Decoder.decode(bytes.slice(start, end));
}
function utf8Length(text) {
  let length = 0;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    if (c < 0x80) length += 1;
    else if (c < 0x800) length += 2;
    else if (c >= 0xd800 && c <= 0xdbff && (text.charCodeAt(i + 1) & 0xfc00) === 0xdc00) { length += 4; i++; }
    else length += 3; // includes lone surrogates, encoded as U+FFFD
  }
  return length;
}
// The string at `ptr` of `length` units, or NUL-terminated within `max` units
// when `length` is negative. Undefined if it is not inside guest memory.
export function wasmer_napi_guest_string(memory, ptr, length, encoding, max) {
  const unit = encoding === 2 ? 2 : 1;
  if (length < 0 && (length = terminated(memory, ptr, unit, max)) < 0) return undefined;
  const end = ptr + length * unit;
  const bytes = view(memory, end);
  if (end > bytes.length) return undefined;
  if (encoding === 0) return decodeUtf8(bytes, ptr, end);
  if (encoding === 1) return fromCodes(bytes.subarray(ptr, end));
  if ((ptr & 1) === 0) return fromCodes(new Uint16Array(bytes.buffer, ptr, length));
  const codes = new Uint16Array(length);
  for (let i = 0; i < length; i++) codes[i] = bytes[ptr + 2 * i] | (bytes[ptr + 2 * i + 1] << 8);
  return fromCodes(codes);
}
// napi_get_value_string_{utf8,latin1,utf16}. Without a buffer (or with a zero
// size) the full length is reported. Otherwise at most size - 1 units are
// copied (UTF-8 never splits a character), followed by a NUL, and the copied
// length is reported. The length is also stored at a non-zero `resultPtr`.
// Returns the length, -1 if `value` is not a string, or -2 if a write would
// leave guest memory.
export function wasmer_napi_guest_write_string(memory, value, buf, size, resultPtr, encoding) {
  if (typeof value !== 'string') return -1;
  let length;
  if (buf === 0 || size === 0) {
    length = encoding === 0 ? utf8Length(value) : value.length;
  } else if (encoding === 0) {
    let bytes = view(memory, buf + size);
    if (buf + size > bytes.length) return -2;
    const limit = Math.min(value.length, size - 1);
    let i = 0;
    for (; i < limit; i++) {
      const c = value.charCodeAt(i);
      if (c >= 0x80) break;
      bytes[buf + i] = c;
    }
    length = i;
    if (i < limit) {
      const encoded = utf8Encoder.encode(value);
      length = Math.min(encoded.length, size - 1);
      while (length > 0 && length < encoded.length && (encoded[length] & 0xc0) === 0x80) length--;
      bytes.set(encoded.subarray(0, length), buf);
    }
    bytes[buf + length] = 0;
  } else {
    const unit = encoding === 2 ? 2 : 1;
    length = Math.min(value.length, size - 1);
    const end = buf + (length + 1) * unit;
    const bytes = view(memory, end);
    if (end > bytes.length) return -2;
    if (unit === 1) {
      for (let i = 0; i < length; i++) bytes[buf + i] = value.charCodeAt(i);
      bytes[buf + length] = 0;
    } else if ((buf & 1) === 0) {
      const units = new Uint16Array(bytes.buffer, buf, length + 1);
      for (let i = 0; i < length; i++) units[i] = value.charCodeAt(i);
      units[length] = 0;
    } else {
      for (let i = 0; i < length; i++) {
        const c = value.charCodeAt(i);
        bytes[buf + 2 * i] = c;
        bytes[buf + 2 * i + 1] = c >>> 8;
      }
      bytes[buf + 2 * length] = 0;
      bytes[buf + 2 * length + 1] = 0;
    }
  }
  if (resultPtr !== 0 && !wasmer_napi_guest_write_u32(memory, resultPtr, length)) return -2;
  return length;
}
"#)]
extern "C" {
    fn wasmer_napi_guest_byte_length(memory: &JsValue) -> f64;
    fn wasmer_napi_guest_read(memory: &JsValue, ptr: u32, out: &mut [u8]) -> bool;
    fn wasmer_napi_guest_write(memory: &JsValue, ptr: u32, data: &[u8]) -> bool;
    fn wasmer_napi_guest_write_u8(memory: &JsValue, ptr: u32, value: u8) -> bool;
    fn wasmer_napi_guest_write_u32(memory: &JsValue, ptr: u32, value: u32) -> bool;
    fn wasmer_napi_guest_write_u64(memory: &JsValue, ptr: u32, low: u32, high: u32) -> bool;
    fn wasmer_napi_guest_write_f64(memory: &JsValue, ptr: u32, value: f64) -> bool;
    fn wasmer_napi_guest_write_number(memory: &JsValue, value: &JsValue, ptr: u32, kind: u32) -> bool;
    fn wasmer_napi_guest_c_string_length(memory: &JsValue, ptr: u32, max: u32) -> i32;
    fn wasmer_napi_guest_string(
        memory: &JsValue,
        ptr: u32,
        length: f64,
        encoding: u32,
        max: u32,
    ) -> Option<js_sys::JsString>;
    fn wasmer_napi_guest_write_string(
        memory: &JsValue,
        value: &JsValue,
        buf: u32,
        size: u32,
        result_ptr: u32,
        encoding: u32,
    ) -> f64;
}

/// Encoding of a string in guest memory.
#[derive(Clone, Copy)]
pub enum GuestStringEncoding {
    Utf8 = 0,
    Latin1 = 1,
    Utf16 = 2,
}

/// Reads `length` code units at `guest_ptr` (`u32::MAX`, i.e. a wasm32
/// `NAPI_AUTO_LENGTH`, for a NUL-terminated string) as a JS string. Invalid
/// UTF-8 is replaced with U+FFFD. `None` if the string is not inside guest
/// memory or has no terminator within `MAX_GUEST_CSTRING_SCAN` units.
pub fn read_guest_string_value(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: i32,
    length: u32,
    encoding: GuestStringEncoding,
) -> Option<JsValue> {
    if guest_ptr < 0 {
        return None;
    }
    let memory = guest_memory(env)?;
    let length = if length == u32::MAX {
        -1.0
    } else {
        f64::from(length)
    };
    let max = u32::try_from(super::MAX_GUEST_CSTRING_SCAN).unwrap_or(u32::MAX);
    wasmer_napi_guest_string(&memory, guest_ptr as u32, length, encoding as u32, max)
        .map(JsValue::from)
}

/// Implements `napi_get_value_string_*` for a value already resolved from its
/// handle, writing into guest memory directly. Returns an N-API status.
pub fn write_guest_string_value(
    env: &mut FunctionEnvMut<NapiEnv>,
    value: &JsValue,
    buf: i32,
    size: i32,
    result_ptr: i32,
    encoding: GuestStringEncoding,
) -> i32 {
    let Some(memory) = guest_memory(env) else {
        return 1;
    };
    let buf = if buf > 0 { buf as u32 } else { 0 };
    let size = if size > 0 { size as u32 } else { 0 };
    let result_ptr = if result_ptr > 0 { result_ptr as u32 } else { 0 };
    let written =
        wasmer_napi_guest_write_string(&memory, value, buf, size, result_ptr, encoding as u32);
    if written == -1.0 {
        3 // napi_string_expected
    } else if written < 0.0 {
        1 // napi_invalid_arg
    } else {
        0
    }
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
    let Some(memory) = guest_memory(env) else {
        return false;
    };
    wasmer_napi_guest_write_u64(&memory, guest_ptr, val as u32, (val >> 32) as u32)
}

pub fn write_guest_i64(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: i64) -> bool {
    write_guest_u64(env, guest_ptr, val as u64)
}

pub fn write_guest_f64(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: f64) -> bool {
    let Some(memory) = guest_memory(env) else {
        return false;
    };
    wasmer_napi_guest_write_f64(&memory, guest_ptr, val)
}

pub fn write_guest_u8(env: &mut FunctionEnvMut<NapiEnv>, guest_ptr: u32, val: u8) -> bool {
    let Some(memory) = guest_memory(env) else {
        return false;
    };
    wasmer_napi_guest_write_u8(&memory, guest_ptr, val)
}

/// Integer or float type of a `napi_get_value_*` number result.
#[derive(Clone, Copy)]
pub enum GuestNumberKind {
    I32 = 0,
    U32 = 1,
    I64 = 2,
    F64 = 3,
}

/// Implements `napi_get_value_{int32,uint32,int64,double}` for a value already
/// resolved from its handle, storing the result in guest memory directly.
/// Conversions saturate like Rust's `as`. Returns an N-API status.
pub fn write_guest_number_value(
    env: &mut FunctionEnvMut<NapiEnv>,
    value: &JsValue,
    guest_ptr: i32,
    kind: GuestNumberKind,
) -> i32 {
    let Some(memory) = guest_memory(env) else {
        return 1;
    };
    // Like the host bridge, a non-number is napi_invalid_arg.
    if wasmer_napi_guest_write_number(&memory, value, guest_ptr as u32, kind as u32) {
        0
    } else {
        1
    }
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
