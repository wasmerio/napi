//! Exercises the bridge's env registry lookup under many environments living
//! in one process: the per-thread lookup cache must never report a released
//! environment as live, must survive stale pointers from other threads, and
//! must keep steady-state calls off the shared registry.

use std::{
    ffi::{c_char, c_void},
    ptr,
    sync::{
        Arc, Mutex, MutexGuard, Once,
        atomic::{AtomicBool, AtomicPtr, AtomicUsize, Ordering},
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
        env_out: *mut *mut c_void,
    ) -> i32;
    fn snapi_bridge_unofficial_release_env(env: *mut c_void) -> i32;
    fn snapi_bridge_unofficial_env_alive(env: *mut c_void) -> i32;
    fn snapi_bridge_unofficial_take_fatal_requested(env: *mut c_void) -> i32;
    fn snapi_bridge_swap_active_callback_ctx(env: *mut c_void, ctx: *mut c_void) -> *mut c_void;
    fn snapi_bridge_get_undefined(env: *mut c_void, out_id: *mut u32) -> i32;
    fn snapi_bridge_get_null(env: *mut c_void, out_id: *mut u32) -> i32;
    fn snapi_bridge_get_boolean(env: *mut c_void, value: i32, out_id: *mut u32) -> i32;
    fn snapi_bridge_typeof(env: *mut c_void, id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_test_registry_lookup_count() -> u64;
}

const NAPI_OK: i32 = 0;
const NAPI_INVALID_ARG: i32 = 1;
const NAPI_BOOLEAN: i32 = 2;

/// Tests in this file share process-wide bridge state (the registry lookup
/// counter in particular), so they run one at a time.
fn serialize() -> MutexGuard<'static, ()> {
    static LOCK: Mutex<()> = Mutex::new(());
    static INIT: Once = Once::new();
    let guard = LOCK.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    INIT.call_once(|| {
        assert_eq!(unsafe { snapi_bridge_init() }, 1);
        assert_eq!(
            unsafe { snapi_bridge_unofficial_configure_runtime(ptr::null(), 0) },
            0
        );
    });
    guard
}

fn create_env() -> *mut c_void {
    let mut env = ptr::null_mut();
    assert_eq!(
        unsafe { snapi_bridge_unofficial_create_env(8, ptr::null(), &mut env) },
        NAPI_OK
    );
    assert!(!env.is_null());
    env
}

fn release_env(env: *mut c_void) {
    assert_eq!(unsafe { snapi_bridge_unofficial_release_env(env) }, NAPI_OK);
    assert_eq!(unsafe { snapi_bridge_unofficial_env_alive(env) }, 0);
}

/// A handful of V8-touching bridge calls; returns the first non-ok status.
fn exercise(env: *mut c_void) -> i32 {
    let mut id = 0u32;
    let status = unsafe { snapi_bridge_get_undefined(env, &mut id) };
    if status != NAPI_OK {
        return status;
    }
    let status = unsafe { snapi_bridge_get_null(env, &mut id) };
    if status != NAPI_OK {
        return status;
    }
    let status = unsafe { snapi_bridge_get_boolean(env, 1, &mut id) };
    if status != NAPI_OK {
        return status;
    }
    let mut kind = -1;
    let status = unsafe { snapi_bridge_typeof(env, id, &mut kind) };
    if status != NAPI_OK {
        return status;
    }
    assert_eq!(kind, NAPI_BOOLEAN);
    NAPI_OK
}

#[test]
fn warm_thread_resolves_env_without_the_registry() {
    let _guard = serialize();
    let _ctx = NapiCtx::default();
    let env = create_env();
    // Warm the cache, then measure.
    assert_eq!(exercise(env), NAPI_OK);
    let before = unsafe { snapi_bridge_test_registry_lookup_count() };
    for _ in 0..10_000 {
        assert_eq!(exercise(env), NAPI_OK);
    }
    let during = unsafe { snapi_bridge_test_registry_lookup_count() };
    assert_eq!(
        during - before,
        0,
        "steady-state bridge calls must not consult the shared registry"
    );
    release_env(env);
    // A stale handle falls back to the registry (and is refused there).
    let after_release = unsafe { snapi_bridge_test_registry_lookup_count() };
    let mut id = 0;
    assert_eq!(
        unsafe { snapi_bridge_get_undefined(env, &mut id) },
        NAPI_INVALID_ARG
    );
    assert!(unsafe { snapi_bridge_test_registry_lookup_count() } > after_release);
}

#[test]
fn concurrent_environments_create_call_and_release_independently() {
    let _guard = serialize();
    let _ctx = NapiCtx::default();
    const THREADS: usize = 8;
    const ROUNDS: usize = 3;
    const CALLS: usize = 2_000;
    let workers: Vec<_> = (0..THREADS)
        .map(|_| {
            thread::spawn(|| {
                for _ in 0..ROUNDS {
                    let env = create_env();
                    for _ in 0..CALLS {
                        assert_eq!(exercise(env), NAPI_OK);
                    }
                    release_env(env);
                }
            })
        })
        .collect();
    for worker in workers {
        worker.join().unwrap();
    }
}

#[test]
fn pointer_probes_from_another_thread_survive_teardown() {
    let _guard = serialize();
    let _ctx = NapiCtx::default();
    const OWNERS: usize = 4;
    const ROUNDS: usize = 12;

    let published: Arc<Vec<AtomicPtr<c_void>>> = Arc::new(
        (0..OWNERS)
            .map(|_| AtomicPtr::new(ptr::null_mut()))
            .collect(),
    );
    let seen = Arc::new(Mutex::new(Vec::<usize>::new()));
    let stop = Arc::new(AtomicBool::new(false));
    let probes = Arc::new(AtomicUsize::new(0));

    let prober = {
        let published = Arc::clone(&published);
        let seen = Arc::clone(&seen);
        let stop = Arc::clone(&stop);
        let probes = Arc::clone(&probes);
        thread::spawn(move || {
            while !stop.load(Ordering::Acquire) {
                for slot in published.iter() {
                    let env = slot.load(Ordering::Acquire);
                    if env.is_null() {
                        continue;
                    }
                    // Pointer-only entry points: never dereference an env the
                    // registry does not vouch for, whatever its state is.
                    unsafe {
                        snapi_bridge_unofficial_env_alive(env);
                        snapi_bridge_unofficial_take_fatal_requested(env);
                        snapi_bridge_swap_active_callback_ctx(env, ptr::null_mut());
                    }
                    probes.fetch_add(1, Ordering::Relaxed);
                    let mut seen = seen.lock().unwrap();
                    if !seen.contains(&(env as usize)) {
                        seen.push(env as usize);
                    }
                }
            }
        })
    };

    let owners: Vec<_> = (0..OWNERS)
        .map(|index| {
            let published = Arc::clone(&published);
            let probes = Arc::clone(&probes);
            thread::spawn(move || {
                for _ in 0..ROUNDS {
                    let env = create_env();
                    published[index].store(env, Ordering::Release);
                    // Let the prober cache this env while it is live.
                    let target = probes.load(Ordering::Relaxed) + 50;
                    while probes.load(Ordering::Relaxed) < target {
                        assert_eq!(exercise(env), NAPI_OK);
                    }
                    release_env(env);
                    published[index].store(ptr::null_mut(), Ordering::Release);
                }
            })
        })
        .collect();
    for owner in owners {
        owner.join().unwrap();
    }
    stop.store(true, Ordering::Release);
    prober.join().unwrap();

    // Every pointer the prober ever cached belongs to a released env now and
    // must read as dead from a thread that may still pin its memory.
    let seen = seen.lock().unwrap();
    assert!(!seen.is_empty());
    let checker = {
        let seen: Vec<usize> = seen.clone();
        thread::spawn(move || {
            for env in seen {
                assert_eq!(
                    unsafe { snapi_bridge_unofficial_env_alive(env as *mut c_void) },
                    0
                );
            }
        })
    };
    checker.join().unwrap();
}

#[test]
fn cached_handle_on_another_thread_reads_dead_after_release() {
    let _guard = serialize();
    let _ctx = NapiCtx::default();
    // The owner thread creates the env; a long-lived prober thread caches the
    // pointer while it is live, then re-probes the *same cached slot* after
    // the owner released the env. This exercises the stale-hit path that the
    // disposing thread's own cache drop cannot cover.
    let (to_prober, from_owner) = std::sync::mpsc::channel::<(usize, bool)>();
    let (to_owner, from_prober) = std::sync::mpsc::channel::<i32>();
    let prober = thread::spawn(move || {
        for (env, live) in from_owner {
            let alive = unsafe { snapi_bridge_unofficial_env_alive(env as *mut c_void) };
            let mut id = 0;
            let status = unsafe { snapi_bridge_get_undefined(env as *mut c_void, &mut id) };
            assert_eq!(
                alive,
                if live { 1 } else { 0 },
                "env_alive from the probing thread"
            );
            assert_eq!(status, if live { NAPI_OK } else { NAPI_INVALID_ARG });
            to_owner.send(alive).unwrap();
        }
    });
    for _ in 0..8 {
        let env = create_env();
        to_prober.send((env as usize, true)).unwrap();
        assert_eq!(from_prober.recv().unwrap(), 1);
        release_env(env);
        to_prober.send((env as usize, false)).unwrap();
        assert_eq!(from_prober.recv().unwrap(), 0);
        // A new env on the owner thread must work while the prober may still
        // pin the old one.
        let next = create_env();
        assert_eq!(exercise(next), NAPI_OK);
        to_prober.send((next as usize, true)).unwrap();
        assert_eq!(from_prober.recv().unwrap(), 1);
        release_env(next);
        to_prober.send((next as usize, false)).unwrap();
        assert_eq!(from_prober.recv().unwrap(), 0);
    }
    drop(to_prober);
    prober.join().unwrap();
}

#[test]
fn released_env_handle_is_refused_and_a_new_env_works_on_the_same_thread() {
    let _guard = serialize();
    let _ctx = NapiCtx::default();
    for _ in 0..4 {
        let first = create_env();
        assert_eq!(exercise(first), NAPI_OK);
        release_env(first);
        let mut id = 0;
        assert_eq!(
            unsafe { snapi_bridge_get_undefined(first, &mut id) },
            NAPI_INVALID_ARG
        );
        assert_eq!(unsafe { snapi_bridge_unofficial_env_alive(first) }, 0);
        // Whether or not the allocator hands back the same address, the new
        // environment must resolve and the old handle must stay refused.
        let second = create_env();
        assert_eq!(exercise(second), NAPI_OK);
        if second != first {
            assert_eq!(
                unsafe { snapi_bridge_get_undefined(first, &mut id) },
                NAPI_INVALID_ARG
            );
        }
        release_env(second);
    }
}

#[test]
fn one_thread_rotating_over_many_envs_keeps_every_env_resolvable() {
    let _guard = serialize();
    let _ctx = NapiCtx::default();
    // More environments than the per-thread cache holds, all created up front.
    const ENVS: usize = 6;
    let envs: Vec<*mut c_void> = (0..ENVS).map(|_| create_env()).collect();
    for _ in 0..500 {
        for &env in &envs {
            assert_eq!(exercise(env), NAPI_OK);
        }
    }
    // Release one at a time; released handles must be refused while the
    // remaining environments keep working.
    for released in 0..ENVS {
        release_env(envs[released]);
        for (index, &env) in envs.iter().enumerate() {
            let expected = if index <= released {
                NAPI_INVALID_ARG
            } else {
                NAPI_OK
            };
            let mut id = 0;
            assert_eq!(
                unsafe { snapi_bridge_get_undefined(env, &mut id) },
                expected,
                "env {index} after releasing {released}"
            );
        }
    }
}
