use wasmer::FunctionEnvMut;

use crate::NapiEnv;

use super::util::read_guest_bytes;

const JS_SOURCE_SIZE: usize = 12;
const BYTECODE_OPEN_SIZE: usize = 48;
const MODULE_CREATE_PREFIX_SIZE: usize = 40;
const MODULE_HOOKS_SIZE: usize = 16;

fn u32_at(bytes: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        bytes.get(offset..offset.checked_add(4)?)?.try_into().ok()?,
    ))
}

fn read_versioned(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: i32,
    prefix_size: usize,
    version: u32,
) -> Option<Vec<u8>> {
    if guest_ptr <= 0 {
        return None;
    }
    let bytes = read_guest_bytes(env, guest_ptr, prefix_size)?;
    if u32_at(&bytes, 0)? < prefix_size as u32 || u32_at(&bytes, 4)? != version {
        return None;
    }
    Some(bytes)
}

pub(crate) fn read_output_header(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: i32,
    minimum_size: usize,
    version: u32,
) -> Option<u32> {
    let bytes = read_guest_bytes(env, guest_ptr, 8)?;
    let size = u32_at(&bytes, 0)?;
    (size >= minimum_size as u32 && u32_at(&bytes, 4)? == version).then_some(size)
}

pub(crate) fn read_js_source(
    env: &mut FunctionEnvMut<NapiEnv>,
    guest_ptr: i32,
) -> Option<(u32, u32)> {
    let bytes = read_guest_bytes(env, guest_ptr, JS_SOURCE_SIZE)?;
    match (u32_at(&bytes, 0)?, u32_at(&bytes, 4)?, u32_at(&bytes, 8)?) {
        (0, text, 0) if text != 0 => Some((text, 0)),
        (1, 0, bytecode) if bytecode != 0 => Some((0, bytecode)),
        _ => None,
    }
}

pub(crate) struct BytecodeOpen {
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
    guest_ptr: i32,
) -> Option<BytecodeOpen> {
    let bytes = read_versioned(env, guest_ptr, BYTECODE_OPEN_SIZE, 2)?;
    let cache_ptr = u32_at(&bytes, 36)? as i32;
    let cache_length = u32_at(&bytes, 40)? as usize;
    let has_cache = *bytes.get(44)?;
    let cache_policy = *bytes.get(45)?;
    let cache = if has_cache != 0 && cache_length != 0 {
        if cache_ptr <= 0 {
            return None;
        }
        read_guest_bytes(env, cache_ptr, cache_length)?
    } else {
        Vec::new()
    };
    Some(BytecodeOpen {
        source_text: u32_at(&bytes, 8)?,
        filename: u32_at(&bytes, 12)?,
        shape: u32_at(&bytes, 16)? as i32,
        params_or_undefined: u32_at(&bytes, 20)?,
        host_defined_option_id: u32_at(&bytes, 24)?,
        line_offset: u32_at(&bytes, 28)? as i32,
        column_offset: u32_at(&bytes, 32)? as i32,
        cache,
        has_cache,
        cache_policy,
    })
}

pub(crate) struct ModuleCreate {
    pub kind: i32,
    pub wrapper: u32,
    pub url: u32,
    pub context_or_undefined: u32,
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
    guest_ptr: i32,
) -> Option<ModuleCreate> {
    let bytes = read_versioned(env, guest_ptr, MODULE_CREATE_PREFIX_SIZE, 1)?;
    let kind = u32_at(&bytes, 8)? as i32;
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
            let (text, bytecode) = read_js_source(env, u32_at(&bytes, 24)? as i32)?;
            (
                text,
                bytecode,
                u32_at(&bytes, 28)? as i32,
                u32_at(&bytes, 32)? as i32,
                u32_at(&bytes, 36)?,
                0,
                0,
            )
        }
        2 => (0, 0, 0, 0, 0, u32_at(&bytes, 24)?, u32_at(&bytes, 28)?),
        _ => return None,
    };
    Some(ModuleCreate {
        kind,
        wrapper: u32_at(&bytes, 12)?,
        url: u32_at(&bytes, 16)?,
        context_or_undefined: u32_at(&bytes, 20)?,
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
    guest_ptr: i32,
) -> Option<(u32, u32)> {
    let bytes = read_versioned(env, guest_ptr, MODULE_HOOKS_SIZE, 1)?;
    Some((u32_at(&bytes, 8)?, u32_at(&bytes, 12)?))
}
