use std::{
    ffi::{c_char, c_void},
    ptr,
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
    thread,
};

use wasmer_napi::NapiCtx;

unsafe extern "C" {
    fn snapi_bridge_init() -> i32;
    fn snapi_bridge_unofficial_configure_runtime(flags: *const c_char, length: u32) -> i32;
    fn snapi_bridge_unofficial_create_env(
        api_version: i32,
        guest_heap: *const c_void,
        webassembly_policy: u32,
        env_out: *mut *mut c_void,
    ) -> i32;
    fn snapi_bridge_unofficial_attach_legacy_env(env: *mut c_void) -> i32;
    fn snapi_bridge_unofficial_take_fatal_requested(env: *mut c_void) -> i32;
    fn snapi_bridge_unofficial_release_env(env: *mut c_void) -> i32;
    fn snapi_bridge_test_native_env_identity(env: *mut c_void) -> usize;
    fn snapi_bridge_test_signal_legacy_fatal(identity: usize);
}

#[test]
fn legacy_fatal_lookup_is_safe_during_concurrent_teardown() {
    // Force this binary to link the N-API provider's native bridge.
    let _ctx = NapiCtx::default();
    assert_eq!(unsafe { snapi_bridge_init() }, 1);
    assert_eq!(
        unsafe { snapi_bridge_unofficial_configure_runtime(ptr::null(), 0) },
        0
    );

    for _ in 0..4 {
        let mut env = ptr::null_mut();
        assert_eq!(
            unsafe { snapi_bridge_unofficial_create_env(8, ptr::null(), 0, &mut env) },
            0
        );
        assert!(!env.is_null());
        assert_eq!(unsafe { snapi_bridge_unofficial_attach_legacy_env(env) }, 0);
        let identity = unsafe { snapi_bridge_test_native_env_identity(env) };
        assert_ne!(identity, 0);
        unsafe { snapi_bridge_test_signal_legacy_fatal(identity) };
        assert_eq!(
            unsafe { snapi_bridge_unofficial_take_fatal_requested(env) },
            1
        );

        let stop = Arc::new(AtomicBool::new(false));
        let calls = Arc::new(AtomicUsize::new(0));
        let worker_stop = Arc::clone(&stop);
        let worker_calls = Arc::clone(&calls);
        let worker = thread::spawn(move || {
            while !worker_stop.load(Ordering::Acquire) {
                unsafe { snapi_bridge_test_signal_legacy_fatal(identity) };
                worker_calls.fetch_add(1, Ordering::Release);
            }
            // Lookup of a removed identity must remain harmless.
            for _ in 0..1000 {
                unsafe { snapi_bridge_test_signal_legacy_fatal(identity) };
            }
        });
        while calls.load(Ordering::Acquire) < 100 {
            thread::yield_now();
        }
        assert_eq!(unsafe { snapi_bridge_unofficial_release_env(env) }, 0);
        stop.store(true, Ordering::Release);
        worker.join().unwrap();
    }
}
