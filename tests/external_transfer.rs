use std::{
    ffi::{c_char, c_void},
    ptr,
};

use wasmer_napi::NapiCtx;

unsafe extern "C" {
    fn snapi_bridge_init() -> i32;
    fn snapi_bridge_unofficial_configure_runtime(flags: *const c_char, length: u32) -> i32;
    fn snapi_bridge_unofficial_create_env(
        api_version: i32,
        guest_heap: *const c_void,
        env_out: *mut *mut c_void,
    ) -> i32;
    fn snapi_bridge_unofficial_release_env(env: *mut c_void) -> i32;
    fn napi_v8_test_fail_next_external_buffer_after_transfer();
    fn snapi_bridge_create_external_buffer_finalized(
        env: *mut c_void,
        data_addr: u64,
        byte_length: u32,
        finalize_hint: *mut c_void,
        backing_store_token_out: *mut u64,
        out_id: *mut u32,
        ownership_transferred_out: *mut i32,
    ) -> i32;
}

#[test]
fn bridge_reports_finalizer_ownership_even_when_buffer_creation_fails() {
    let _ctx = NapiCtx::default();
    assert_eq!(unsafe { snapi_bridge_init() }, 1);
    assert_eq!(
        unsafe { snapi_bridge_unofficial_configure_runtime(ptr::null(), 0) },
        0
    );
    let mut env = ptr::null_mut();
    assert_eq!(
        unsafe { snapi_bridge_unofficial_create_env(8, ptr::null(), &mut env) },
        0
    );
    let mut bytes = [0u8; 8];
    let mut backing_store_token = 0;
    let mut out = 0;
    let mut transferred = 0;
    unsafe { napi_v8_test_fail_next_external_buffer_after_transfer() };
    assert_eq!(
        unsafe {
            snapi_bridge_create_external_buffer_finalized(
                env,
                bytes.as_mut_ptr() as u64,
                bytes.len() as u32,
                ptr::null_mut(),
                &mut backing_store_token,
                &mut out,
                &mut transferred,
            )
        },
        9
    );
    assert_eq!(transferred, 1);
    assert_eq!(out, 0);
    assert_eq!(unsafe { snapi_bridge_unofficial_release_env(env) }, 0);
}
