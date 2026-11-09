use std::collections::HashSet;
use std::ffi::CString;

use wasmer::FunctionEnvMut;

use crate::NapiEnv;

use super::util::{guest_data_size, read_guest_bytes, read_guest_c_string_max};

#[repr(C)]
struct Wasm32VersionedHeader {
    size: u32,
    version: u32,
}

#[repr(C)]
struct Wasm32RuntimeOptionsV1 {
    size: u32,
    version: u32,
    engine_flags: u32,
    engine_flags_length: u32,
}

#[repr(C)]
struct Wasm32EnvCreateOptionsV1 {
    size: u32,
    version: u32,
    total_memory: u64,
    constrained_memory: u64,
    max_young_generation_size_in_bytes: u32,
    max_old_generation_size_in_bytes: u32,
    code_range_size_in_bytes: u32,
    stack_limit: u32,
    guest_heap: u32,
}

#[repr(C)]
struct Wasm32EnvHooksV1 {
    size: u32,
    version: u32,
    data: u32,
    context_token_assign_callback: u32,
    context_token_unassign_callback: u32,
    enqueue_foreground_task_callback: u32,
    fatal_error_callback: u32,
    oom_error_callback: u32,
}

#[repr(C)]
struct Wasm32JsSource {
    kind: i32,
    text: u32,
    bytecode: u32,
}

#[repr(C)]
struct Wasm32BytecodeOpenOptionsV1 {
    size: u32,
    version: u32,
    source_text: u32,
    filename: u32,
    shape: i32,
    params_or_undefined: u32,
    host_defined_option_id: u32,
    line_offset: i32,
    column_offset: i32,
    cache_bytes: u32,
    cache_byte_length: u32,
    has_cache: u8,
    cache_policy: u8,
    reserved: [u8; 2],
}

/// Version 2 appends the realm to version 1.
#[repr(C)]
struct Wasm32BytecodeOpenOptionsV2 {
    v1: Wasm32BytecodeOpenOptionsV1,
    realm_or_null: u32,
}

#[repr(C)]
struct Wasm32ModuleCreateOptionsV1 {
    size: u32,
    version: u32,
    kind: i32,
    wrapper: u32,
    url: u32,
    context_or_undefined: u32,
    payload: [u8; 16],
}

/// Version 2 appends the realm to version 1.
#[repr(C)]
struct Wasm32ModuleCreateOptionsV2 {
    v1: Wasm32ModuleCreateOptionsV1,
    realm_or_null: u32,
}

#[repr(C)]
struct Wasm32ModuleCreateJsPayload {
    source: u32,
    line_offset: i32,
    column_offset: i32,
    host_defined_option_id: u32,
}

#[repr(C)]
struct Wasm32ModuleCreateSyntheticPayload {
    export_names: u32,
    synthetic_eval_steps: u32,
    reserved: [u8; 8],
}

#[repr(C)]
struct Wasm32ModuleHooksV1 {
    size: u32,
    version: u32,
    import_module_dynamically: u32,
    initialize_import_meta_object: u32,
}

#[repr(C)]
struct Wasm32RealmOptionsV1 {
    size: u32,
    version: u32,
    sandbox_or_null: u32,
    name: u32,
    origin: u32,
    host_defined_option_id: u32,
    allow_code_gen_strings: u8,
    allow_code_gen_wasm: u8,
    own_microtask_queue: u8,
    reserved: u8,
}

const JS_SOURCE_SIZE: usize = std::mem::size_of::<Wasm32JsSource>();
const RUNTIME_OPTIONS_SIZE: usize = std::mem::size_of::<Wasm32RuntimeOptionsV1>();
const ENV_CREATE_PREFIX_SIZE: usize = std::mem::size_of::<Wasm32EnvCreateOptionsV1>();
const BYTECODE_OPEN_V1_SIZE: usize = std::mem::size_of::<Wasm32BytecodeOpenOptionsV1>();
const BYTECODE_OPEN_V2_SIZE: usize = std::mem::size_of::<Wasm32BytecodeOpenOptionsV2>();
const MODULE_CREATE_V1_SIZE: usize = std::mem::size_of::<Wasm32ModuleCreateOptionsV1>();
const MODULE_CREATE_V2_SIZE: usize = std::mem::size_of::<Wasm32ModuleCreateOptionsV2>();
const MODULE_HOOKS_SIZE: usize = std::mem::size_of::<Wasm32ModuleHooksV1>();
const ENV_HOOKS_SIZE: usize = std::mem::size_of::<Wasm32EnvHooksV1>();
const REALM_OPTIONS_SIZE: usize = std::mem::size_of::<Wasm32RealmOptionsV1>();

const _: () = assert!(std::mem::size_of::<Wasm32VersionedHeader>() == 8);
const _: () = assert!(JS_SOURCE_SIZE == 12);
const _: () = assert!(RUNTIME_OPTIONS_SIZE == 16);
const _: () = assert!(ENV_CREATE_PREFIX_SIZE == 48);
const _: () = assert!(BYTECODE_OPEN_V1_SIZE == 48);
const _: () = assert!(BYTECODE_OPEN_V2_SIZE == 52);
const _: () = assert!(MODULE_CREATE_V1_SIZE == 40);
const _: () = assert!(MODULE_CREATE_V2_SIZE == 44);
const _: () = assert!(std::mem::size_of::<Wasm32ModuleCreateJsPayload>() == 16);
const _: () = assert!(std::mem::size_of::<Wasm32ModuleCreateSyntheticPayload>() == 16);
const _: () = assert!(MODULE_HOOKS_SIZE == 16);
const _: () = assert!(ENV_HOOKS_SIZE == 32);
const _: () = assert!(REALM_OPTIONS_SIZE == 28);

fn u32_at(bytes: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        bytes.get(offset..offset.checked_add(4)?)?.try_into().ok()?,
    ))
}

fn i32_at(bytes: &[u8], offset: usize) -> Option<i32> {
    Some(i32::from_le_bytes(
        bytes.get(offset..offset.checked_add(4)?)?.try_into().ok()?,
    ))
}

fn read_versioned(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    prefix_size: usize,
    version: u32,
) -> Option<Vec<u8>> {
    if guest_ptr == 0 {
        return None;
    }
    let bytes = read_guest_bytes(env, guest_ptr, prefix_size)?;
    if u32_at(&bytes, std::mem::offset_of!(Wasm32VersionedHeader, size))? < prefix_size as u32
        || u32_at(&bytes, std::mem::offset_of!(Wasm32VersionedHeader, version))? != version
    {
        return None;
    }
    Some(bytes)
}

/// Reads a struct that grows at the end, whose version `n` is `sizes[n - 1]`
/// bytes long. Returns the version and only that version's bytes.
fn read_grown(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    sizes: &[usize],
) -> Option<(u32, Vec<u8>)> {
    if guest_ptr == 0 {
        return None;
    }
    let header = read_guest_bytes(env, guest_ptr, std::mem::size_of::<Wasm32VersionedHeader>())?;
    let size = u32_at(&header, std::mem::offset_of!(Wasm32VersionedHeader, size))?;
    let version = u32_at(
        &header,
        std::mem::offset_of!(Wasm32VersionedHeader, version),
    )?;
    let prefix_size = *sizes.get(usize::try_from(version).ok()?.checked_sub(1)?)?;
    if size < prefix_size as u32 {
        return None;
    }
    Some((version, read_guest_bytes(env, guest_ptr, prefix_size)?))
}

pub(crate) fn read_output_header(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
    minimum_size: usize,
    version: u32,
) -> Option<u32> {
    let bytes = read_guest_bytes(env, guest_ptr, 8)?;
    let size = u32_at(&bytes, std::mem::offset_of!(Wasm32VersionedHeader, size))?;
    (size >= minimum_size as u32
        && u32_at(&bytes, std::mem::offset_of!(Wasm32VersionedHeader, version))? == version)
        .then_some(size)
}

pub(crate) struct EnvCreate {
    pub total_memory: u64,
    pub constrained_memory: u64,
    pub max_young_generation_size_in_bytes: u32,
    pub max_old_generation_size_in_bytes: u32,
    pub code_range_size_in_bytes: u32,
    pub stack_limit: u32,
}

pub(crate) fn read_runtime_options(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
) -> Option<Vec<u8>> {
    if guest_ptr == 0 {
        return Some(Vec::new());
    }
    let bytes = read_versioned(env, guest_ptr, RUNTIME_OPTIONS_SIZE, 1)?;
    let flags_ptr = u32_at(
        &bytes,
        std::mem::offset_of!(Wasm32RuntimeOptionsV1, engine_flags),
    )?;
    let flags_length = u32_at(
        &bytes,
        std::mem::offset_of!(Wasm32RuntimeOptionsV1, engine_flags_length),
    )? as usize;
    if flags_length == 0 {
        return Some(Vec::new());
    }
    if flags_ptr == 0 {
        return None;
    }
    read_guest_bytes(env, flags_ptr, flags_length)
}

pub(crate) fn read_env_create(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
) -> Option<EnvCreate> {
    let bytes = read_versioned(env, guest_ptr, ENV_CREATE_PREFIX_SIZE, 1)?;
    // A raw guest pointer is not an unofficial_napi_guest_heap provider
    // resource. Wasmer injects its own guest-heap owner after decoding, so a
    // non-null wire field is rejected rather than silently discarded.
    if u32_at(
        &bytes,
        std::mem::offset_of!(Wasm32EnvCreateOptionsV1, guest_heap),
    )? != 0
    {
        return None;
    }
    Some(EnvCreate {
        total_memory: u64::from_le_bytes(
            bytes
                .get(
                    std::mem::offset_of!(Wasm32EnvCreateOptionsV1, total_memory)
                        ..std::mem::offset_of!(Wasm32EnvCreateOptionsV1, total_memory) + 8,
                )?
                .try_into()
                .ok()?,
        ),
        constrained_memory: u64::from_le_bytes(
            bytes
                .get(
                    std::mem::offset_of!(Wasm32EnvCreateOptionsV1, constrained_memory)
                        ..std::mem::offset_of!(Wasm32EnvCreateOptionsV1, constrained_memory) + 8,
                )?
                .try_into()
                .ok()?,
        ),
        max_young_generation_size_in_bytes: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32EnvCreateOptionsV1, max_young_generation_size_in_bytes),
        )?,
        max_old_generation_size_in_bytes: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32EnvCreateOptionsV1, max_old_generation_size_in_bytes),
        )?,
        code_range_size_in_bytes: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32EnvCreateOptionsV1, code_range_size_in_bytes),
        )?,
        stack_limit: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32EnvCreateOptionsV1, stack_limit),
        )?,
    })
}

pub(crate) fn read_js_source(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
) -> Option<(u32, u32)> {
    let bytes = read_guest_bytes(env, guest_ptr, JS_SOURCE_SIZE)?;
    match (
        i32_at(&bytes, std::mem::offset_of!(Wasm32JsSource, kind))?,
        u32_at(&bytes, std::mem::offset_of!(Wasm32JsSource, text))?,
        u32_at(&bytes, std::mem::offset_of!(Wasm32JsSource, bytecode))?,
    ) {
        (0, text, 0) if text != 0 => Some((text, 0)),
        (1, 0, bytecode) if bytecode != 0 => Some((0, bytecode)),
        _ => None,
    }
}

pub(crate) struct BytecodeOpen {
    /// 1 or 2. Version 1 compiles for the current realm.
    pub version: u32,
    /// The guest realm id of version 2 (0 for the env's own realm); 0 in
    /// version 1.
    pub realm_or_null: u32,
    pub source_text: u32,
    pub filename: u32,
    pub shape: i32,
    pub params_or_undefined: u32,
    pub host_defined_option_id: u32,
    pub line_offset: i32,
    pub column_offset: i32,
    pub cache: Vec<u8>,
    pub has_cache: u8,
    pub cache_policy: u8,
}

pub(crate) fn read_bytecode_open(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
) -> Option<BytecodeOpen> {
    let (version, bytes) = read_grown(
        env,
        guest_ptr,
        &[BYTECODE_OPEN_V1_SIZE, BYTECODE_OPEN_V2_SIZE],
    )?;
    let cache_ptr = u32_at(
        &bytes,
        std::mem::offset_of!(Wasm32BytecodeOpenOptionsV1, cache_bytes),
    )?;
    let cache_length = u32_at(
        &bytes,
        std::mem::offset_of!(Wasm32BytecodeOpenOptionsV1, cache_byte_length),
    )? as usize;
    let has_cache = *bytes.get(std::mem::offset_of!(Wasm32BytecodeOpenOptionsV1, has_cache))?;
    let cache_policy = *bytes.get(std::mem::offset_of!(
        Wasm32BytecodeOpenOptionsV1,
        cache_policy
    ))?;
    let cache = if has_cache != 0 && cache_length != 0 {
        if cache_ptr == 0 {
            return None;
        }
        read_guest_bytes(env, cache_ptr, cache_length)?
    } else {
        Vec::new()
    };
    Some(BytecodeOpen {
        version,
        realm_or_null: if version >= 2 {
            u32_at(
                &bytes,
                std::mem::offset_of!(Wasm32BytecodeOpenOptionsV2, realm_or_null),
            )?
        } else {
            0
        },
        source_text: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32BytecodeOpenOptionsV1, source_text),
        )?,
        filename: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32BytecodeOpenOptionsV1, filename),
        )?,
        shape: i32_at(
            &bytes,
            std::mem::offset_of!(Wasm32BytecodeOpenOptionsV1, shape),
        )?,
        params_or_undefined: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32BytecodeOpenOptionsV1, params_or_undefined),
        )?,
        host_defined_option_id: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32BytecodeOpenOptionsV1, host_defined_option_id),
        )?,
        line_offset: i32_at(
            &bytes,
            std::mem::offset_of!(Wasm32BytecodeOpenOptionsV1, line_offset),
        )?,
        column_offset: i32_at(
            &bytes,
            std::mem::offset_of!(Wasm32BytecodeOpenOptionsV1, column_offset),
        )?,
        cache,
        has_cache,
        cache_policy,
    })
}

pub(crate) struct ModuleCreate {
    /// 1 or 2.
    pub version: u32,
    pub kind: i32,
    pub wrapper: u32,
    pub url: u32,
    /// The deprecated context's value id (0 for NULL).
    pub context_or_undefined: u32,
    /// The guest realm id of version 2 (0 for NULL); 0 in version 1.
    pub realm_or_null: u32,
    pub source_text: u32,
    pub source_bytecode: u32,
    pub line_offset: i32,
    pub column_offset: i32,
    pub host_defined_option_id: u32,
    pub export_names: u32,
    pub synthetic_eval_steps: u32,
}

pub(crate) fn read_module_create(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
) -> Option<ModuleCreate> {
    let (version, bytes) = read_grown(
        env,
        guest_ptr,
        &[MODULE_CREATE_V1_SIZE, MODULE_CREATE_V2_SIZE],
    )?;
    let kind = i32_at(
        &bytes,
        std::mem::offset_of!(Wasm32ModuleCreateOptionsV1, kind),
    )?;
    let payload_offset = std::mem::offset_of!(Wasm32ModuleCreateOptionsV1, payload);
    let (
        source_text,
        source_bytecode,
        line_offset,
        column_offset,
        host_defined_option_id,
        export_names,
        synthetic_eval_steps,
    ) = match kind {
        1 => {
            let source_offset =
                payload_offset + std::mem::offset_of!(Wasm32ModuleCreateJsPayload, source);
            let (text, bytecode) = read_js_source(env, u32_at(&bytes, source_offset)?)?;
            (
                text,
                bytecode,
                i32_at(
                    &bytes,
                    payload_offset + std::mem::offset_of!(Wasm32ModuleCreateJsPayload, line_offset),
                )?,
                i32_at(
                    &bytes,
                    payload_offset
                        + std::mem::offset_of!(Wasm32ModuleCreateJsPayload, column_offset),
                )?,
                u32_at(
                    &bytes,
                    payload_offset
                        + std::mem::offset_of!(Wasm32ModuleCreateJsPayload, host_defined_option_id),
                )?,
                0,
                0,
            )
        }
        2 => (
            0,
            0,
            0,
            0,
            0,
            u32_at(
                &bytes,
                payload_offset
                    + std::mem::offset_of!(Wasm32ModuleCreateSyntheticPayload, export_names),
            )?,
            u32_at(
                &bytes,
                payload_offset
                    + std::mem::offset_of!(
                        Wasm32ModuleCreateSyntheticPayload,
                        synthetic_eval_steps
                    ),
            )?,
        ),
        _ => return None,
    };
    Some(ModuleCreate {
        version,
        kind,
        wrapper: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32ModuleCreateOptionsV1, wrapper),
        )?,
        url: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32ModuleCreateOptionsV1, url),
        )?,
        context_or_undefined: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32ModuleCreateOptionsV1, context_or_undefined),
        )?,
        realm_or_null: if version >= 2 {
            u32_at(
                &bytes,
                std::mem::offset_of!(Wasm32ModuleCreateOptionsV2, realm_or_null),
            )?
        } else {
            0
        },
        source_text,
        source_bytecode,
        line_offset,
        column_offset,
        host_defined_option_id,
        export_names,
        synthetic_eval_steps,
    })
}

pub(crate) fn read_module_hooks(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
) -> Option<(u32, u32)> {
    let bytes = read_versioned(env, guest_ptr, MODULE_HOOKS_SIZE, 1)?;
    Some((
        u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32ModuleHooksV1, import_module_dynamically),
        )?,
        u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32ModuleHooksV1, initialize_import_meta_object),
        )?,
    ))
}

/// `unofficial_napi_realm_options` read from guest memory. Value fields are
/// guest napi_value ids; 0 means NULL.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct RealmOptions {
    pub sandbox_or_null: u32,
    pub name: u32,
    pub origin: u32,
    pub host_defined_option_id: u32,
    pub allow_code_gen_strings: bool,
    pub allow_code_gen_wasm: bool,
    pub own_microtask_queue: bool,
}

impl Default for RealmOptions {
    /// The defaults of `unofficial_napi_realm_options_init`.
    fn default() -> Self {
        Self {
            sandbox_or_null: 0,
            name: 0,
            origin: 0,
            host_defined_option_id: 0,
            allow_code_gen_strings: true,
            allow_code_gen_wasm: true,
            own_microtask_queue: false,
        }
    }
}

/// Reads realm options at `guest_ptr`. A null pointer means the defaults;
/// `None` means the descriptor is out of bounds or has the wrong size or
/// version.
pub(crate) fn read_realm_options(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
) -> Option<RealmOptions> {
    if guest_ptr == 0 {
        return Some(RealmOptions::default());
    }
    let bytes = read_guest_bytes(env, guest_ptr, REALM_OPTIONS_SIZE)?;
    if u32_at(&bytes, 0)? != REALM_OPTIONS_SIZE as u32 || u32_at(&bytes, 4)? != 1 {
        return None;
    }
    let flag = |offset: usize| bytes.get(offset).map(|byte| *byte != 0);
    Some(RealmOptions {
        sandbox_or_null: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32RealmOptionsV1, sandbox_or_null),
        )?,
        name: u32_at(&bytes, std::mem::offset_of!(Wasm32RealmOptionsV1, name))?,
        origin: u32_at(&bytes, std::mem::offset_of!(Wasm32RealmOptionsV1, origin))?,
        host_defined_option_id: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32RealmOptionsV1, host_defined_option_id),
        )?,
        allow_code_gen_strings: flag(std::mem::offset_of!(
            Wasm32RealmOptionsV1,
            allow_code_gen_strings
        ))?,
        allow_code_gen_wasm: flag(std::mem::offset_of!(
            Wasm32RealmOptionsV1,
            allow_code_gen_wasm
        ))?,
        own_microtask_queue: flag(std::mem::offset_of!(
            Wasm32RealmOptionsV1,
            own_microtask_queue
        ))?,
    })
}

const INVALID_ARG: i32 = 1;
pub(crate) const MAX_SNAPSHOT_GUEST_DATA: usize = 16 * 1024 * 1024;

/// Check required result ranges before the provider can change engine state.
pub(crate) fn guest_outputs(
    env: &mut FunctionEnvMut<NapiEnv>,
    outputs: &[(u32, usize)],
) -> Result<(), i32> {
    let size = guest_data_size(env);
    for (i, &(ptr, len)) in outputs.iter().enumerate() {
        let start = u64::from(ptr);
        let end = start.checked_add(len as u64).ok_or(INVALID_ARG)?;
        if ptr == 0 || end > size {
            return Err(INVALID_ARG);
        }
        if outputs[..i].iter().any(|&(other, len)| {
            let other = u64::from(other);
            start < other + len as u64 && other < end
        }) {
            return Err(INVALID_ARG);
        }
    }
    Ok(())
}

pub(crate) struct BindingDescriptor {
    pub name: Vec<u8>,
    pub properties: Vec<BindingProperty>,
    pub member_lengths: Option<Vec<u8>>,
}

pub(crate) struct BindingProperty {
    pub name: CString,
    pub method: u32,
    pub getter: u32,
    pub setter: u32,
    pub value: u32,
    pub attrs: i32,
    pub data: u32,
    pub kind: u32,
}

/// Decode the wasm32 descriptor fully before allocating callbacks.
#[allow(clippy::too_many_arguments)]
pub(crate) fn read_binding(
    env: &mut FunctionEnvMut<NapiEnv>,
    kind: u32,
    name_ptr: u32,
    name_length: u32,
    callback: u32,
    parent: u32,
    count: u32,
    properties: u32,
    lengths: u32,
) -> Result<BindingDescriptor, i32> {
    const PROPERTY_SIZE: usize = 32;
    const MAX_MEMBER_NAME: usize = 64 * 1024;
    if name_ptr == 0 || name_length == 0 || name_length == u32::MAX || callback == 0
        || (kind == 2 && (parent != 0 || count != 0 || properties != 0 || lengths != 0))
        || (count != 0 && properties == 0)
    {
        return Err(INVALID_ARG);
    }
    let name = read_guest_bytes(env, name_ptr, name_length as usize).ok_or(INVALID_ARG)?;
    std::str::from_utf8(&name).map_err(|_| INVALID_ARG)?;
    let count = count as usize;
    let property_size = count.checked_mul(PROPERTY_SIZE).ok_or(INVALID_ARG)?;
    let descriptors = if count == 0 {
        Vec::new()
    } else {
        read_guest_bytes(env, properties, property_size).ok_or(INVALID_ARG)?
    };
    let member_lengths = if lengths == 0 {
        None
    } else {
        Some(read_guest_bytes(env, lengths, count).ok_or(INVALID_ARG)?)
    };
    let mut keys = HashSet::new();
    let mut properties = Vec::new();
    for (index, bytes) in descriptors.chunks_exact(PROPERTY_SIZE).enumerate() {
        let read = |offset| u32_at(bytes, offset).ok_or(INVALID_ARG);
        let name_ptr = read(0)?;
        let method = read(8)?;
        let getter = read(12)?;
        let setter = read(16)?;
        let value = read(20)?;
        let attrs = read(24)? as i32;
        let data = read(28)?;
        let property_kind = match (method != 0, getter != 0, setter != 0, value != 0) {
            (false, false, false, true) => 0,
            (true, false, false, false) => 1,
            (false, true, false, false) => 2,
            (false, false, true, false) => 3,
            (false, true, true, false) => 4,
            _ => return Err(INVALID_ARG),
        };
        if name_ptr == 0 || read(4)? != 0 || attrs & !0x407 != 0
            || (property_kind != 1 && member_lengths.as_ref().is_some_and(|l| l[index] != 0))
        {
            return Err(INVALID_ARG);
        }
        let key = read_guest_c_string_max(env, name_ptr, MAX_MEMBER_NAME).ok_or(INVALID_ARG)?;
        let key_text = std::str::from_utf8(&key).map_err(|_| INVALID_ARG)?;
        let is_static = attrs & 0x400 != 0;
        if (is_static && matches!(key_text, "length" | "name" | "prototype"))
            || (!is_static && key_text == "constructor")
            || !keys.insert((is_static, key.clone()))
        {
            return Err(INVALID_ARG);
        }
        properties.push(BindingProperty {
            name: CString::new(key).map_err(|_| INVALID_ARG)?,
            method, getter, setter, value, attrs, data, kind: property_kind,
        });
    }
    Ok(BindingDescriptor { name, properties, member_lengths })
}

pub(crate) struct EnvHooks {
    pub fatal: u32,
    pub oom: u32,
    pub requested: u64,
}

pub(crate) fn read_env_hooks(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: u32,
) -> Option<EnvHooks> {
    let bytes = read_versioned(env, guest_ptr, ENV_HOOKS_SIZE, 1)?;
    let hook_fields = [
        (
            std::mem::offset_of!(Wasm32EnvHooksV1, context_token_assign_callback),
            1u64 << 0,
        ),
        (
            std::mem::offset_of!(Wasm32EnvHooksV1, context_token_unassign_callback),
            1u64 << 1,
        ),
        (
            std::mem::offset_of!(Wasm32EnvHooksV1, enqueue_foreground_task_callback),
            1u64 << 2,
        ),
        (
            std::mem::offset_of!(Wasm32EnvHooksV1, fatal_error_callback),
            1u64 << 3,
        ),
        (
            std::mem::offset_of!(Wasm32EnvHooksV1, oom_error_callback),
            1u64 << 4,
        ),
    ];
    let mut requested = 0;
    for (offset, bit) in hook_fields {
        if u32_at(&bytes, offset)? != 0 {
            requested |= bit;
        }
    }
    Some(EnvHooks {
        fatal: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32EnvHooksV1, fatal_error_callback),
        )?,
        oom: u32_at(
            &bytes,
            std::mem::offset_of!(Wasm32EnvHooksV1, oom_error_callback),
        )?,
        requested,
    })
}
