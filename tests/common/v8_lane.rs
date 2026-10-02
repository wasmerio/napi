//! Native harness for tests that drive V8 through the bridge with an
//! embedder lane and a test accountant, as a managed embedder does. Included
//! with `#[path]` by the test binaries that need it.
//!
//! No test may configure the runtime without a lane: that would start the
//! standalone pool and switch the process to the lenient mixed-mode rule.

#![allow(dead_code)]

use std::{
    ffi::{c_char, c_void},
    ptr,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    thread,
    time::Duration,
};

pub const MIB: u64 = 1024 * 1024;
pub const NAPI_PENDING_EXCEPTION: i32 = 10;
/// `NapiWebAssemblyPolicy::kAllowMetered`.
pub const POLICY_METERED: u32 = 2;
pub const POLICY_RESTRICTED: u32 = 0;

type LaneScope = Option<unsafe extern "C" fn(*mut c_void) -> *mut c_void>;
type LaneLeave = Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool>;
type LaneSignal = Option<unsafe extern "C" fn(*mut c_void)>;

#[repr(C)]
pub struct WasmAccounting {
    size: u32,
    max_memories: u32,
    max_reserved_bytes: u64,
    code_budget_bytes: u64,
    charge_memory: unsafe extern "C" fn(*mut c_void, u64) -> bool,
    uncharge_memory: unsafe extern "C" fn(*mut c_void, u64),
    charge_code: unsafe extern "C" fn(*mut c_void, u64) -> bool,
    uncharge_code: unsafe extern "C" fn(*mut c_void, u64),
    on_code_limit: unsafe extern "C" fn(*mut c_void, u32, u64, u64),
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy)]
pub struct WasmUsage {
    pub memories: u64,
    pub reserved_bytes: u64,
    pub code_bytes: u64,
}

unsafe extern "C" {
    fn snapi_bridge_init() -> i32;
    fn snapi_bridge_unofficial_configure_runtime(flags: *const c_char, length: u32) -> i32;
    fn snapi_bridge_unofficial_create_env(
        api_version: i32,
        guest_heap: *const c_void,
        webassembly_policy: u32,
        env_out: *mut *mut c_void,
    ) -> i32;
    fn snapi_bridge_unofficial_release_env(env: *mut c_void) -> i32;
    fn snapi_bridge_create_string_utf8(
        env: *mut c_void,
        text: *const c_char,
        length: u32,
        out: *mut u32,
    ) -> i32;
    fn snapi_bridge_run_script(env: *mut c_void, script: u32, out: *mut u32) -> i32;
    fn snapi_bridge_get_value_string_utf8(
        env: *mut c_void,
        id: u32,
        buf: *mut c_char,
        size: usize,
        written: *mut usize,
    ) -> i32;
    fn snapi_bridge_get_global(env: *mut c_void, out: *mut u32) -> i32;
    fn snapi_bridge_set_named_property(
        env: *mut c_void,
        object: u32,
        name: *const c_char,
        value: u32,
    ) -> i32;
    fn snapi_bridge_unofficial_message_create(
        env: *mut c_void,
        value: u32,
        message_out: *mut u32,
    ) -> i32;
    fn snapi_bridge_unofficial_message_take(
        env: *mut c_void,
        message: u32,
        value_out: *mut u32,
    ) -> i32;
    fn snapi_bridge_unofficial_collect_garbage(env: *mut c_void) -> i32;
    fn snapi_bridge_unofficial_terminate_execution(env: *mut c_void) -> i32;
    fn snapi_bridge_unofficial_cancel_terminate_execution(env: *mut c_void) -> i32;

    fn snapi_v8_lane_new(
        max_queued_tasks: usize,
        scope_context: *mut c_void,
        enter_scope: LaneScope,
        leave_scope: LaneLeave,
        on_overload: LaneSignal,
    ) -> *mut c_void;
    fn snapi_v8_lane_run(handle: *mut c_void);
    fn snapi_v8_lane_stop(handle: *mut c_void);
    fn snapi_v8_lane_delete(handle: *mut c_void);
    fn snapi_v8_lane_swap_current(handle: *mut c_void) -> *mut c_void;
    fn snapi_v8_lane_set_page_accountant(
        handle: *mut c_void,
        context: *mut c_void,
        charge: unsafe extern "C" fn(*mut c_void, u64) -> bool,
        uncharge: unsafe extern "C" fn(*mut c_void, u64),
        release: unsafe extern "C" fn(*mut c_void),
    ) -> bool;
    fn snapi_v8_lane_set_wasm_accounting(
        handle: *mut c_void,
        accounting: *const WasmAccounting,
    ) -> bool;
    fn snapi_v8_lane_wasm_usage(handle: *mut c_void, out: *mut WasmUsage);
}

/// Per-context wasm caps for [`Lane::new`].
#[derive(Clone, Copy, Debug)]
pub struct Caps {
    pub max_memories: u32,
    pub max_reserved_bytes: u64,
    pub code_budget_bytes: u64,
}

impl Default for Caps {
    fn default() -> Self {
        Self {
            max_memories: 64,
            max_reserved_bytes: u64::MAX,
            code_budget_bytes: 64 * MIB,
        }
    }
}

/// A test accountant with one limit over every pool; the code charge is
/// terminal in spirit (it refuses past the limit) but has no side effects.
#[derive(Default)]
pub struct Accountant {
    pub limit: u64,
    pub charged: AtomicU64,
    pub buffers: AtomicU64,
    pub memory: AtomicU64,
    pub code: AtomicU64,
    pub peak_code: AtomicU64,
    pub denied: AtomicU64,
    pub released: AtomicBool,
    pub code_limits: Mutex<Vec<(u32, u64, u64)>>,
}

impl Accountant {
    pub fn charged(&self) -> u64 {
        self.charged.load(Ordering::SeqCst)
    }
    pub fn memory(&self) -> u64 {
        self.memory.load(Ordering::SeqCst)
    }
    pub fn code(&self) -> u64 {
        self.code.load(Ordering::SeqCst)
    }
    pub fn buffers(&self) -> u64 {
        self.buffers.load(Ordering::SeqCst)
    }

    fn charge(&self, pool: &AtomicU64, bytes: u64) -> bool {
        let granted = self
            .charged
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |current| {
                current
                    .checked_add(bytes)
                    .filter(|total| *total <= self.limit)
            })
            .is_ok();
        if granted {
            pool.fetch_add(bytes, Ordering::SeqCst);
        } else {
            self.denied.fetch_add(1, Ordering::SeqCst);
        }
        granted
    }

    fn uncharge(&self, pool: &AtomicU64, bytes: u64) {
        let previous = pool.fetch_sub(bytes, Ordering::SeqCst);
        assert!(previous >= bytes, "uncharged more than was charged");
        self.charged.fetch_sub(bytes, Ordering::SeqCst);
    }
}

fn accountant<'a>(context: *mut c_void) -> &'a Accountant {
    unsafe { &*context.cast::<Accountant>() }
}

unsafe extern "C" fn charge_buffer(context: *mut c_void, bytes: u64) -> bool {
    let a = accountant(context);
    a.charge(&a.buffers, bytes)
}
unsafe extern "C" fn uncharge_buffer(context: *mut c_void, bytes: u64) {
    let a = accountant(context);
    a.uncharge(&a.buffers, bytes)
}
unsafe extern "C" fn charge_memory(context: *mut c_void, bytes: u64) -> bool {
    let a = accountant(context);
    a.charge(&a.memory, bytes)
}
unsafe extern "C" fn uncharge_memory(context: *mut c_void, bytes: u64) {
    let a = accountant(context);
    a.uncharge(&a.memory, bytes)
}
unsafe extern "C" fn charge_code(context: *mut c_void, bytes: u64) -> bool {
    let a = accountant(context);
    let granted = a.charge(&a.code, bytes);
    a.peak_code.fetch_max(a.code(), Ordering::SeqCst);
    granted
}
unsafe extern "C" fn uncharge_code(context: *mut c_void, bytes: u64) {
    let a = accountant(context);
    a.uncharge(&a.code, bytes)
}
unsafe extern "C" fn on_code_limit(context: *mut c_void, reason: u32, committed: u64, limit: u64) {
    accountant(context)
        .code_limits
        .lock()
        .unwrap()
        .push((reason, committed, limit));
}
unsafe extern "C" fn release(context: *mut c_void) {
    let a = unsafe { Arc::from_raw(context.cast::<Accountant>()) };
    assert!(!a.released.swap(true, Ordering::SeqCst));
}

/// Applies the process-wide engine limits; every test of a binary must use
/// the same limits, before its first environment.
pub fn configure_engine(limits: &wasmer_napi::WasmEngineLimits) {
    // Links the provider's native bridge into the test binary.
    let _ = wasmer_napi::NapiCtx::default();
    wasmer_napi::configure_wasm_engine(limits).expect("engine limits");
}

/// An embedder lane with a running worker and an attached accountant.
pub struct Lane {
    pub handle: *mut c_void,
    worker: Option<thread::JoinHandle<()>>,
    pub accountant: Arc<Accountant>,
}

// The native lane synchronizes its queue and accountant itself.
unsafe impl Send for Lane {}
unsafe impl Sync for Lane {}

impl Lane {
    /// A lane metering WebAssembly with `caps` (`None`: buffers only).
    pub fn new(limit: u64, caps: Option<Caps>) -> Self {
        let handle = unsafe { snapi_v8_lane_new(256, ptr::null_mut(), None, None, None) };
        assert!(!handle.is_null());
        let address = handle as usize;
        let worker = thread::spawn(move || unsafe { snapi_v8_lane_run(address as *mut c_void) });
        let accountant = Arc::new(Accountant {
            limit,
            ..Default::default()
        });
        let context = Arc::into_raw(Arc::clone(&accountant)).cast_mut().cast();
        assert!(unsafe {
            snapi_v8_lane_set_page_accountant(
                handle,
                context,
                charge_buffer,
                uncharge_buffer,
                release,
            )
        });
        if let Some(caps) = caps {
            let accounting = WasmAccounting {
                size: std::mem::size_of::<WasmAccounting>() as u32,
                max_memories: caps.max_memories,
                max_reserved_bytes: caps.max_reserved_bytes,
                code_budget_bytes: caps.code_budget_bytes,
                charge_memory,
                uncharge_memory,
                charge_code,
                uncharge_code,
                on_code_limit,
            };
            assert!(unsafe { snapi_v8_lane_set_wasm_accounting(handle, &accounting) });
            // Attached once.
            assert!(!unsafe { snapi_v8_lane_set_wasm_accounting(handle, &accounting) });
        }
        Self {
            handle,
            worker: Some(worker),
            accountant,
        }
    }

    pub fn bind(&self) -> Bound {
        Bound(unsafe { snapi_v8_lane_swap_current(self.handle) })
    }

    pub fn usage(&self) -> WasmUsage {
        let mut usage = WasmUsage::default();
        unsafe { snapi_v8_lane_wasm_usage(self.handle, &mut usage) };
        usage
    }

    /// Creates an environment with `policy` (environments rebind the lane on
    /// entry), or returns the bridge status.
    pub fn try_env(&self, policy: u32) -> Result<Env, i32> {
        let _bound = self.bind();
        assert_eq!(unsafe { snapi_bridge_init() }, 1);
        assert_eq!(
            unsafe { snapi_bridge_unofficial_configure_runtime(ptr::null(), 0) },
            0
        );
        let mut env = ptr::null_mut();
        match unsafe { snapi_bridge_unofficial_create_env(8, ptr::null(), policy, &mut env) } {
            0 => {
                assert!(!env.is_null());
                Ok(Env(env))
            }
            status => Err(status),
        }
    }

    pub fn env(&self) -> Env {
        self.try_env(POLICY_METERED).expect("metered environment")
    }

    /// Stops the lane and asserts its accountant was released exactly once
    /// and holds no charge.
    pub fn finish(mut self) {
        unsafe { snapi_v8_lane_stop(self.handle) };
        self.worker.take().unwrap().join().unwrap();
        unsafe { snapi_v8_lane_delete(self.handle) };
        assert_eq!(self.accountant.charged(), 0, "teardown left a charge");
        assert!(
            self.accountant.released.load(Ordering::SeqCst),
            "a tracked region still holds the accountant after teardown"
        );
    }
}

pub struct Bound(*mut c_void);

impl Drop for Bound {
    fn drop(&mut self) {
        unsafe { snapi_v8_lane_swap_current(self.0) };
    }
}

pub struct Env(pub *mut c_void);

// The bridge serializes access per environment.
unsafe impl Send for Env {}
unsafe impl Sync for Env {}

impl Env {
    /// Runs `source`, which must evaluate to a string, or returns the status.
    pub fn try_eval(&self, source: &str) -> Result<String, i32> {
        let mut script = 0;
        let mut result = 0;
        assert_eq!(
            unsafe {
                snapi_bridge_create_string_utf8(
                    self.0,
                    source.as_ptr().cast(),
                    source.len() as u32,
                    &mut script,
                )
            },
            0
        );
        let status = unsafe { snapi_bridge_run_script(self.0, script, &mut result) };
        if status != 0 {
            return Err(status);
        }
        let mut buf = vec![0u8; 4096];
        let mut written = 0;
        assert_eq!(
            unsafe {
                snapi_bridge_get_value_string_utf8(
                    self.0,
                    result,
                    buf.as_mut_ptr().cast(),
                    buf.len(),
                    &mut written,
                )
            },
            0,
            "script must return a string"
        );
        Ok(String::from_utf8_lossy(&buf[..written]).into_owned())
    }

    pub fn eval(&self, source: &str) -> String {
        match self.try_eval(source) {
            Ok(value) => value,
            Err(status) => panic!("script failed with status {status}: {source}"),
        }
    }

    /// Defines `globalThis.wasmgen` (see `wasmgen.js`).
    pub fn load_wasmgen(&self) {
        self.eval(&format!("{WASMGEN}\n''"));
    }

    pub fn terminate(&self) {
        assert_eq!(
            unsafe { snapi_bridge_unofficial_terminate_execution(self.0) },
            0
        );
    }

    pub fn cancel_terminate(&self) {
        assert_eq!(
            unsafe { snapi_bridge_unofficial_cancel_terminate_execution(self.0) },
            0
        );
    }

    pub fn release(self) {
        assert_eq!(unsafe { snapi_bridge_unofficial_release_env(self.0) }, 0);
    }

    /// Serializes the value of `expr` into a worker message.
    pub fn message(&self, expr: &str) -> u32 {
        let (mut code, mut value, mut message) = (0, 0, 0);
        unsafe {
            let length = expr.len() as u32;
            assert_eq!(
                snapi_bridge_create_string_utf8(self.0, expr.as_ptr().cast(), length, &mut code),
                0
            );
            assert_eq!(snapi_bridge_run_script(self.0, code, &mut value), 0);
            assert_eq!(
                snapi_bridge_unofficial_message_create(self.0, value, &mut message),
                0
            );
        }
        message
    }

    /// Receives a worker message as the global `name`.
    pub fn receive(&self, message: u32, name: &std::ffi::CStr) {
        let (mut received, mut global) = (0, 0);
        unsafe {
            assert_eq!(
                snapi_bridge_unofficial_message_take(self.0, message, &mut received),
                0
            );
            assert_eq!(snapi_bridge_get_global(self.0, &mut global), 0);
            assert_eq!(
                snapi_bridge_set_named_property(self.0, global, name.as_ptr(), received),
                0
            );
        }
    }

    /// Collects garbage until `done` holds (the sweeper may finish on the lane).
    pub fn gc_until(&self, done: impl Fn() -> bool) -> bool {
        for _ in 0..200 {
            assert_eq!(
                unsafe { snapi_bridge_unofficial_collect_garbage(self.0) },
                0
            );
            if done() {
                return true;
            }
            thread::sleep(Duration::from_millis(10));
        }
        false
    }
}

/// Evaluates to the constructor name of what `expr` throws, or "ok".
pub fn outcome(expr: &str) -> String {
    format!(
        "(() => {{ try {{ {expr}; return 'ok'; }} catch (e) {{ return e.constructor.name; }} }})()"
    )
}

/// Defines `globalThis.wasmgen` with module builders, see `wasmgen.js`.
pub const WASMGEN: &str = include_str!("wasmgen.js");
