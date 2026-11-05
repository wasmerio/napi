//! The deprecated guest API (version 1 of the option structs) on the bridge's
//! realm calls, for both guest backends, as lib/src/napi_compat.cc does for
//! native code. Ids are guest value ids, 0 is NULL, and a realm id is the
//! value id of the realm's global or sandbox, 0 for the env's own realm.

use crate::snapi::*;

const NAPI_OK: i32 = 0;
const NAPI_INVALID_ARG: i32 = 1;

// napi_valuetype.
const NAPI_UNDEFINED: i32 = 0;
const NAPI_NULL: i32 = 1;
const NAPI_OBJECT: i32 = 6;
const NAPI_FUNCTION: i32 = 7;

fn check(status: i32) -> Result<(), i32> {
    if status == NAPI_OK {
        Ok(())
    } else {
        Err(status)
    }
}

/// The type of `value`, or None when it is NULL or not a live value.
fn type_of(env: SnapiEnv, value: u32) -> Option<i32> {
    if value == 0 {
        return None;
    }
    let mut value_type = 0;
    (unsafe { snapi_bridge_typeof(env, value, &mut value_type) } == NAPI_OK).then_some(value_type)
}

fn is_nullish(env: SnapiEnv, value: u32) -> bool {
    value == 0 || matches!(type_of(env, value), Some(NAPI_UNDEFINED | NAPI_NULL))
}

/// The env's own global, whichever realm is current.
fn env_global(env: SnapiEnv) -> Result<u32, i32> {
    let mut scope = 0u64;
    check(unsafe { snapi_bridge_unofficial_enter_realm(env, 0, &mut scope) })?;
    let mut global = 0u32;
    let status = unsafe { snapi_bridge_get_global(env, &mut global) };
    let left = unsafe { snapi_bridge_unofficial_leave_realm(env, scope) };
    check(status)?;
    check(left)?;
    Ok(global)
}

fn is_env_global(env: SnapiEnv, value: u32) -> Result<bool, i32> {
    let global = env_global(env)?;
    let mut equal = 0i32;
    check(unsafe { snapi_bridge_strict_equals(env, value, global, &mut equal) })?;
    Ok(equal != 0)
}

/// The id of the realm `value` is the global or sandbox of, 0 if none.
fn realm_of(env: SnapiEnv, value: u32) -> Result<u32, i32> {
    let mut realm = 0u32;
    check(unsafe { snapi_bridge_unofficial_get_realm(env, value, &mut realm) })?;
    Ok(realm)
}

/// The current realm, which the env's context meant before realms. The env's
/// own global means the env's realm even when it is also a realm's sandbox.
pub(crate) fn current_realm(env: SnapiEnv) -> Result<u32, i32> {
    let mut global = 0u32;
    check(unsafe { snapi_bridge_get_global(env, &mut global) })?;
    let realm = realm_of(env, global)?;
    if realm == 0 || is_env_global(env, global)? {
        return Ok(0);
    }
    Ok(realm)
}

/// The realm a context argument names: a contextified object's, the env's
/// for its own global, and for NULL, undefined or null the current realm, or
/// the env's when `nullish_is_current` is false. Anything else is
/// napi_invalid_arg.
pub(crate) fn realm_from_context(
    env: SnapiEnv,
    context: u32,
    nullish_is_current: bool,
) -> Result<u32, i32> {
    match type_of(env, context) {
        None if context == 0 => {}
        Some(NAPI_UNDEFINED | NAPI_NULL) => {}
        Some(NAPI_OBJECT | NAPI_FUNCTION) => {
            let realm = realm_of(env, context).map_err(|_| NAPI_INVALID_ARG)?;
            if realm != 0 || is_env_global(env, context)? {
                return Ok(realm);
            }
            return Err(NAPI_INVALID_ARG);
        }
        _ => return Err(NAPI_INVALID_ARG),
    }
    if nullish_is_current {
        current_realm(env)
    } else {
        Ok(0)
    }
}

/// The realm bytecode-open options compile for: the current realm in
/// version 1, `realm_or_null` in version 2.
pub(crate) fn bytecode_open_realm(
    env: SnapiEnv,
    version: u32,
    realm_or_null: u32,
) -> Result<u32, i32> {
    if version == 1 {
        current_realm(env)
    } else {
        Ok(realm_or_null)
    }
}

/// The realm module-create options make the module in. Version 2 takes
/// `realm_or_null`, and then the deprecated context must be nullish; else the
/// context names the realm, nullish meaning the current realm in version 1
/// and the env's in version 2.
pub(crate) fn module_create_realm(
    env: SnapiEnv,
    version: u32,
    context_or_undefined: u32,
    realm_or_null: u32,
) -> Result<u32, i32> {
    if version >= 2 && realm_or_null != 0 {
        if !is_nullish(env, context_or_undefined) {
            return Err(NAPI_INVALID_ARG);
        }
        return Ok(realm_or_null);
    }
    realm_from_context(env, context_or_undefined, version == 1)
}
