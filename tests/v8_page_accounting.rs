//! Page-allocator accounting for buffers V8 commits outside the
//! `ArrayBuffer::Allocator`: resizable `ArrayBuffer`s and growable
//! `SharedArrayBuffer`s reserve address space and commit it page by page.
//!
//! Every test binds an embedder lane with its own accountant, as a managed
//! embedder does. No test may configure the runtime without a lane: that
//! would start the standalone pool and switch the process to the lenient
//! mixed-mode rule.

use std::{
    ffi::{c_char, c_void},
    ptr,
    sync::{
        Arc, Barrier, Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

const MIB: u64 = 1024 * 1024;
const NO_ACCESS: i32 = 0;
const READ_WRITE: i32 = 2;

type LaneScope = Option<unsafe extern "C" fn(*mut c_void) -> *mut c_void>;
type LaneLeave = Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool>;
type LaneSignal = Option<unsafe extern "C" fn(*mut c_void)>;

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
    fn snapi_v8_page_charge_denials() -> u64;
    fn snapi_v8_unattributed_page_reservations() -> u64;
    fn snapi_v8_standalone_pool_created() -> bool;
    fn snapi_v8_test_page_reserve(
        hint: *mut c_void,
        length: usize,
        alignment: usize,
    ) -> *mut c_void;
    fn snapi_bridge_unofficial_collect_garbage(env: *mut c_void) -> i32;
    fn snapi_v8_test_page_set_permissions(
        address: *mut c_void,
        length: usize,
        access: i32,
        metered: bool,
    ) -> bool;
    fn snapi_v8_test_page_free(address: *mut c_void, length: usize) -> bool;
}

/// A soft accountant with a fixed limit, standing in for the embedder's.
#[derive(Default)]
struct Accountant {
    limit: u64,
    charged: AtomicU64,
    denied: AtomicU64,
    peak: AtomicU64,
    released: AtomicBool,
    uncharged_on: Mutex<Vec<thread::ThreadId>>,
}

impl Accountant {
    fn charged(&self) -> u64 {
        self.charged.load(Ordering::SeqCst)
    }
}

unsafe extern "C" fn charge(context: *mut c_void, bytes: u64) -> bool {
    let accountant = unsafe { &*context.cast::<Accountant>() };
    let granted = accountant
        .charged
        .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |current| {
            current
                .checked_add(bytes)
                .filter(|total| *total <= accountant.limit)
        })
        .map(|previous| {
            accountant
                .peak
                .fetch_max(previous + bytes, Ordering::SeqCst)
        })
        .is_ok();
    if !granted {
        accountant.denied.fetch_add(1, Ordering::SeqCst);
    }
    granted
}

unsafe extern "C" fn uncharge(context: *mut c_void, bytes: u64) {
    let accountant = unsafe { &*context.cast::<Accountant>() };
    let previous = accountant.charged.fetch_sub(bytes, Ordering::SeqCst);
    assert!(previous >= bytes, "uncharged more than was charged");
    accountant
        .uncharged_on
        .lock()
        .unwrap()
        .push(thread::current().id());
}

unsafe extern "C" fn release(context: *mut c_void) {
    let accountant = unsafe { Arc::from_raw(context.cast::<Accountant>()) };
    assert!(!accountant.released.swap(true, Ordering::SeqCst));
}

/// An embedder lane with a running worker and an attached accountant.
struct Lane {
    handle: *mut c_void,
    worker: Option<thread::JoinHandle<()>>,
    accountant: Arc<Accountant>,
}

impl Lane {
    fn new(limit: u64) -> Self {
        // Links the provider's native bridge into this test binary.
        let _ = wasmer_napi::NapiCtx::default();
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
            snapi_v8_lane_set_page_accountant(handle, context, charge, uncharge, release)
        });
        // The first accountant wins; a second attach leaves ownership here.
        let second = Arc::into_raw(Arc::clone(&accountant)).cast_mut().cast();
        assert!(!unsafe {
            snapi_v8_lane_set_page_accountant(handle, second, charge, uncharge, release)
        });
        drop(unsafe { Arc::from_raw(second.cast::<Accountant>()) });
        Self {
            handle,
            worker: Some(worker),
            accountant,
        }
    }

    fn bind(&self) -> Bound {
        Bound(unsafe { snapi_v8_lane_swap_current(self.handle) })
    }

    /// Create a context bound to this lane (environments rebind it on entry).
    fn env(&self) -> Env {
        let _bound = self.bind();
        assert_eq!(unsafe { snapi_bridge_init() }, 1);
        assert_eq!(
            unsafe { snapi_bridge_unofficial_configure_runtime(ptr::null(), 0) },
            0
        );
        let mut env = ptr::null_mut();
        assert_eq!(
            unsafe { snapi_bridge_unofficial_create_env(8, ptr::null(), 0, &mut env) },
            0
        );
        assert!(!env.is_null());
        Env(env)
    }

    /// Stop the lane and assert its accountant was released exactly once.
    fn finish(mut self) {
        unsafe { snapi_v8_lane_stop(self.handle) };
        self.worker.take().unwrap().join().unwrap();
        unsafe { snapi_v8_lane_delete(self.handle) };
        assert!(
            self.accountant.released.load(Ordering::SeqCst),
            "a tracked region still holds the accountant after teardown"
        );
    }
}

// The native lane synchronizes its queue and accountant itself.
unsafe impl Send for Lane {}
unsafe impl Sync for Lane {}

struct Bound(*mut c_void);

impl Drop for Bound {
    fn drop(&mut self) {
        unsafe { snapi_v8_lane_swap_current(self.0) };
    }
}

struct Env(*mut c_void);

// The bridge serializes access per environment.
unsafe impl Send for Env {}
unsafe impl Sync for Env {}

impl Env {
    fn eval(&self, source: &str) -> String {
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
        assert_eq!(
            unsafe { snapi_bridge_run_script(self.0, script, &mut result) },
            0,
            "script threw: {source}"
        );
        let mut buf = vec![0u8; 256];
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
            "script must return a string: {source}"
        );
        String::from_utf8_lossy(&buf[..written]).into_owned()
    }

    fn release(self) {
        assert_eq!(unsafe { snapi_bridge_unofficial_release_env(self.0) }, 0);
    }

    /// Serializes the value of `expr` into a worker message.
    fn message(&self, expr: &str) -> u32 {
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
    fn receive(&self, message: u32, name: &std::ffi::CStr) {
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
    fn gc_until(&self, done: impl Fn() -> bool) -> bool {
        for _ in 0..100 {
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
fn outcome(expr: &str) -> String {
    format!(
        "(() => {{ try {{ {expr}; return 'ok'; }} catch (e) {{ return e.constructor.name; }} }})()"
    )
}

#[test]
fn resizable_buffers_charge_commits_and_deny_softly() {
    let lane = Lane::new(64 * MIB);
    let env = lane.env();
    let acct = Arc::clone(&lane.accountant);
    let denials = unsafe { snapi_v8_page_charge_denials() };

    // A reservation alone is not charged.
    env.eval("globalThis.ab = new ArrayBuffer(0, { maxByteLength: 1 << 30 }); ''");
    assert_eq!(acct.charged(), 0);

    env.eval("ab.resize(16 << 20); new Uint8Array(ab).fill(1); ''");
    assert_eq!(acct.charged(), 16 * MIB);

    // Shrinking decommits the suffix (ResizeInPlace at an offset).
    env.eval("ab.resize((4 << 20) + 1); ''");
    assert_eq!(acct.charged(), 4 * MIB + 4096);

    // Growth past the limit is a RangeError; the buffer and context survive.
    assert_eq!(env.eval(&outcome("ab.resize(128 << 20)")), "RangeError");
    assert_eq!(acct.charged(), 4 * MIB + 4096);
    assert_eq!(
        env.eval("String(ab.byteLength)"),
        ((4 << 20) + 1).to_string()
    );
    assert!(unsafe { snapi_v8_page_charge_denials() } > denials);
    assert!(acct.denied.load(Ordering::SeqCst) > 0);

    // Growable SharedArrayBuffers share the path.
    env.eval("globalThis.sab = new SharedArrayBuffer(0, { maxByteLength: 1 << 30 }); sab.grow(8 << 20); ''");
    assert_eq!(acct.charged(), 12 * MIB + 4096);
    assert_eq!(env.eval(&outcome("sab.grow(256 << 20)")), "RangeError");
    assert_eq!(env.eval("String(sab.byteLength)"), (8u64 << 20).to_string());

    // An initial commit past the limit fails construction gracefully.
    assert_eq!(
        env.eval(&outcome(
            "new ArrayBuffer(100 << 20, { maxByteLength: 200 << 20 })"
        )),
        "RangeError"
    );
    assert_eq!(acct.charged(), 12 * MIB + 4096);

    // Freed capacity can be used again.
    env.eval("ab.resize(0); ab.resize(48 << 20); ''");
    assert_eq!(acct.charged(), 56 * MIB);

    env.release();
    assert_eq!(acct.charged(), 0, "teardown left a charge behind");
    lane.finish();
}

#[test]
fn many_buffers_are_charged_and_released_on_teardown() {
    let lane = Lane::new(256 * MIB);
    let env = lane.env();
    let acct = Arc::clone(&lane.accountant);
    env.eval(
        "globalThis.bufs = [];
         for (let i = 0; i < 512; i++) {
           const b = new ArrayBuffer(0, { maxByteLength: 16 << 20 });
           b.resize(64 << 10);
           bufs.push(b);
         } ''",
    );
    assert_eq!(acct.charged(), 512 * 64 * 1024);
    env.release();
    assert_eq!(acct.charged(), 0);
    lane.finish();
}

#[test]
fn concurrent_contexts_charge_their_own_accountants() {
    const LIMIT: u64 = 32 * MIB;
    let workers: Vec<_> = (0..4)
        .map(|_| {
            thread::spawn(|| {
                let lane = Lane::new(LIMIT);
                let env = lane.env();
                let grown = env.eval(
                    "const b = new ArrayBuffer(0, { maxByteLength: 1 << 30 });
                     let n = 0;
                     try { for (;;) { b.resize((n + 1) << 20); n++; } } catch (e) {
                       if (!(e instanceof RangeError)) throw e;
                     }
                     globalThis.keep = b; String(n)",
                );
                assert_eq!(grown, (LIMIT / MIB).to_string());
                assert_eq!(lane.accountant.charged(), LIMIT);
                env.release();
                assert_eq!(lane.accountant.charged(), 0);
                lane.finish();
            })
        })
        .collect();
    for worker in workers {
        worker.join().unwrap();
    }
}

#[test]
fn shared_buffer_grown_by_two_contexts_is_charged_once() {
    let lane = Lane::new(64 * MIB);
    let acct = Arc::clone(&lane.accountant);
    let first = lane.env();
    first.eval("globalThis.sab = new SharedArrayBuffer(0, { maxByteLength: 1 << 30 }); sab.grow(8 << 20); ''");
    assert_eq!(acct.charged(), 8 * MIB);

    // Post the buffer to a second context (a worker sharing the lane).
    let message = first.message("sab");
    let second = thread::scope(|scope| {
        scope
            .spawn(|| {
                let second = lane.env();
                second.receive(message, c"shared");
                second.eval("shared.grow(16 << 20); new Uint8Array(shared)[0] = 7; ''");
                second
            })
            .join()
            .unwrap()
    });
    assert_eq!(
        acct.charged(),
        16 * MIB,
        "a shared commit was charged twice"
    );
    assert_eq!(
        first.eval("String(sab.byteLength + new Uint8Array(sab)[0])"),
        (16 * MIB + 7).to_string()
    );

    // The reserving context's teardown does not free a buffer still shared.
    first.release();
    assert_eq!(acct.charged(), 16 * MIB);
    second.release();
    assert_eq!(acct.charged(), 0);
    lane.finish();
}

#[test]
fn classification_and_fail_closed_rule() {
    let lane = Lane::new(MIB);
    // Initializes the runtime (and the metering allocator) on this lane.
    lane.env().release();
    assert!(!unsafe { snapi_v8_standalone_pool_created() });
    let acct = Arc::clone(&lane.accountant);
    let page = 4096;

    // A buffer-shaped reservation no context can be billed for is refused.
    let unattributed = unsafe { snapi_v8_unattributed_page_reservations() };
    assert!(unsafe { snapi_v8_test_page_reserve(ptr::null_mut(), MIB as usize, page) }.is_null());
    assert!(unsafe { snapi_v8_unattributed_page_reservations() } > unattributed);

    // Large-alignment reservations are V8's own and pass unmetered.
    let cage = unsafe { snapi_v8_test_page_reserve(ptr::null_mut(), 1 << 30, 1 << 30) };
    assert!(!cage.is_null());
    assert!(unsafe { snapi_v8_test_page_set_permissions(cage, page, READ_WRITE, true) });
    assert_eq!(acct.charged(), 0);
    assert!(unsafe { snapi_v8_test_page_free(cage, 1 << 30) });

    let _bound = lane.bind();
    let region = unsafe { snapi_v8_test_page_reserve(ptr::null_mut(), 4 * MIB as usize, page) };
    assert!(!region.is_null());
    assert!(unsafe { snapi_v8_test_page_set_permissions(region, 512 << 10, READ_WRITE, true) });
    assert_eq!(acct.charged(), 512 << 10);
    // A decommit inside the committed prefix keeps the conservative charge.
    let middle = unsafe { region.cast::<u8>().add(page) }.cast();
    assert!(unsafe { snapi_v8_test_page_set_permissions(middle, page, NO_ACCESS, true) });
    assert_eq!(acct.charged(), 512 << 10);
    // A suffix decommit returns it.
    let tail = unsafe { region.cast::<u8>().add(256 << 10) }.cast();
    assert!(unsafe { snapi_v8_test_page_set_permissions(tail, 256 << 10, NO_ACCESS, true) });
    assert_eq!(acct.charged(), 256 << 10);
    // A commit over the limit is refused before the kernel call.
    assert!(!unsafe {
        snapi_v8_test_page_set_permissions(region, 2 * MIB as usize, READ_WRITE, true)
    });
    assert_eq!(acct.charged(), 256 << 10);
    assert!(unsafe { snapi_v8_test_page_free(region, 4 * MIB as usize) });
    assert_eq!(acct.charged(), 0);
    drop(_bound);
    lane.finish();
}

#[test]
fn shared_buffer_grown_concurrently_is_charged_for_its_final_size() {
    let lane = Lane::new(256 * MIB);
    let acct = Arc::clone(&lane.accountant);
    // A context is driven by the thread that created it, like a worker.
    let (message_tx, message_rx) = std::sync::mpsc::channel();
    let grown = Barrier::new(2);
    let released = Barrier::new(2);
    // Interleaved grows to different sizes; a smaller grow that loses the
    // race throws, the larger one wins.
    let grow = |env: &Env, offset: u32| {
        env.eval(&format!(
            "for (let l = 0; l < 64; l++) {{
               try {{ sab.grow((l << 20) + {offset}); }} catch (e) {{
                 if (!(e instanceof RangeError)) throw e;
               }}
             }} ''"
        ))
    };
    let (a, b) = thread::scope(|scope| {
        let (lane, grown, released, acct, grow) = (&lane, &grown, &released, &acct, &grow);
        let a = scope.spawn(move || {
            let env = lane.env();
            env.eval("globalThis.sab = new SharedArrayBuffer(0, { maxByteLength: 1 << 30 }); ''");
            message_tx.send(env.message("sab")).unwrap();
            grown.wait();
            grow(&env, 1 << 20);
            released.wait();
            let result = env.eval("String(sab.byteLength)");
            env.release();
            result
        });
        let b = scope.spawn(move || {
            let env = lane.env();
            env.receive(message_rx.recv().unwrap(), c"sab");
            grown.wait();
            grow(&env, 1 << 19);
            released.wait();
            let result = env.eval("String(sab.byteLength)");
            assert_eq!(acct.charged(), 64 * MIB);
            env.release();
            result
        });
        (a.join().unwrap(), b.join().unwrap())
    });
    assert_eq!(a, (64u64 << 20).to_string());
    assert_eq!(b, a);
    assert_eq!(acct.charged(), 0);
    lane.finish();
}

#[test]
fn garbage_collected_buffers_are_uncharged_while_the_context_lives() {
    let lane = Lane::new(256 * MIB);
    let acct = Arc::clone(&lane.accountant);
    let lane_thread = lane.worker.as_ref().unwrap().thread().id();
    let env = lane.env();
    env.eval(
        "globalThis.bufs = [];
         for (let i = 0; i < 64; i++) {
           const b = new ArrayBuffer(0, { maxByteLength: 16 << 20 });
           b.resize(1 << 20);
           bufs.push(b);
         } ''",
    );
    assert_eq!(acct.charged(), 64 * MIB);
    env.eval("bufs = null; ''");
    assert!(
        env.gc_until(|| acct.charged() == 0),
        "dead buffers stayed charged"
    );
    let threads = acct.uncharged_on.lock().unwrap().clone();
    let on_lane = threads.iter().filter(|id| **id == lane_thread).count();
    println!("{} uncharges, {on_lane} on the lane thread", threads.len());
    assert!(
        threads
            .iter()
            .all(|id| *id == lane_thread || *id == thread::current().id())
    );
    env.release();
    lane.finish();
}

#[test]
fn transferred_buffers_move_their_charge() {
    let lane = Lane::new(64 * MIB);
    let acct = Arc::clone(&lane.accountant);
    let env = lane.env();
    env.eval("globalThis.ab = new ArrayBuffer(0, { maxByteLength: 64 << 20 }); ab.resize(12 << 20); new Uint8Array(ab)[1] = 9; ''");
    assert_eq!(acct.charged(), 12 * MIB);

    // A resizable transfer keeps the buffer resizable; the old one detaches.
    assert_eq!(
        env.eval(
            "globalThis.t = ab.transfer(); String(ab.detached) + t.resizable + new Uint8Array(t)[1]"
        ),
        "truetrue9"
    );
    assert!(
        env.gc_until(|| acct.charged() == 12 * MIB),
        "charged {}",
        acct.charged()
    );
    assert_eq!(
        env.eval("globalThis.t2 = t.transfer(20 << 20); String(t2.byteLength)"),
        (20u64 << 20).to_string()
    );
    assert!(
        env.gc_until(|| acct.charged() == 20 * MIB),
        "charged {}",
        acct.charged()
    );
    // A transfer to a new length copies: old and new are charged together
    // until the old one is freed, so near the limit it is a RangeError.
    assert_eq!(acct.peak.load(Ordering::SeqCst), 32 * MIB);

    // A fixed-length transfer leaves the page allocator entirely.
    assert_eq!(
        env.eval(
            "globalThis.f = t2.transferToFixedLength(); String(f.resizable) + new Uint8Array(f)[1]"
        ),
        "false9"
    );
    assert!(
        env.gc_until(|| acct.charged() == 0),
        "charged {}",
        acct.charged()
    );
    env.release();
    lane.finish();
}

#[test]
fn excluded_ranges_are_trimmed_and_overflow_stays_correct() {
    const GIB: usize = 1 << 30;
    let page = 4096;
    let lane = Lane::new(64 * MIB);
    lane.env().release();
    let acct = Arc::clone(&lane.accountant);

    // A cage-shaped reservation is excluded from metering. Unmapping part of
    // it must end the exclusion there, or a buffer reserved in the freed
    // space would commit unmetered.
    let cage = unsafe { snapi_v8_test_page_reserve(ptr::null_mut(), GIB, GIB) };
    assert!(!cage.is_null());
    let upper = unsafe { cage.cast::<u8>().add(GIB / 2) }.cast::<c_void>();
    assert!(unsafe { snapi_v8_test_page_free(upper, GIB / 2) });
    {
        let _bound = lane.bind();
        let region = unsafe { snapi_v8_test_page_reserve(upper, 4 * MIB as usize, page) };
        assert!(!region.is_null());
        assert_eq!(region, upper, "the kernel ignored the placement hint");
        assert!(unsafe {
            snapi_v8_test_page_set_permissions(region, MIB as usize, READ_WRITE, true)
        });
        assert_eq!(
            acct.charged(),
            MIB,
            "a buffer inside a freed exclusion went unmetered"
        );
        assert!(unsafe { snapi_v8_test_page_free(region, 4 * MIB as usize) });
        assert_eq!(acct.charged(), 0);
    }
    assert!(unsafe { snapi_v8_test_page_free(cage, GIB / 2) });

    // More cage-shaped reservations than the table holds: the rest take the
    // locked path, still pass through, and buffers are still charged.
    let cages: Vec<_> = (0..20)
        .map(|_| unsafe { snapi_v8_test_page_reserve(ptr::null_mut(), GIB, GIB) })
        .collect();
    for cage in &cages {
        assert!(!cage.is_null());
        assert!(unsafe { snapi_v8_test_page_set_permissions(*cage, page, READ_WRITE, true) });
        assert!(unsafe { snapi_v8_test_page_set_permissions(*cage, page, NO_ACCESS, true) });
    }
    assert_eq!(acct.charged(), 0);
    {
        let _bound = lane.bind();
        let region = unsafe { snapi_v8_test_page_reserve(ptr::null_mut(), 4 * MIB as usize, page) };
        assert!(unsafe {
            snapi_v8_test_page_set_permissions(region, MIB as usize, READ_WRITE, true)
        });
        assert_eq!(acct.charged(), MIB);
        assert!(unsafe { snapi_v8_test_page_free(region, 4 * MIB as usize) });
    }
    for cage in cages {
        assert!(unsafe { snapi_v8_test_page_free(cage, GIB) });
    }
    assert_eq!(acct.charged(), 0);
    lane.finish();
}

/// Hot-path cost of the wrapper for V8's own pages (heap pages live in
/// excluded process-wide reservations). Run with `--ignored --nocapture`.
#[test]
#[ignore]
fn set_permissions_hot_path_overhead() {
    const ITERATIONS: usize = 200_000;
    let lane = Lane::new(64 * MIB);
    let env = lane.env();
    // Keep one tracked region alive so the filter cannot short-circuit on an
    // empty region map.
    env.eval("globalThis.ab = new ArrayBuffer(0, { maxByteLength: 1 << 20 }); ab.resize(4096); ''");
    let cage = unsafe { snapi_v8_test_page_reserve(ptr::null_mut(), 1 << 30, 1 << 30) };
    assert!(!cage.is_null());
    let run = |metered: bool| {
        let start = Instant::now();
        for _ in 0..ITERATIONS {
            assert!(unsafe { snapi_v8_test_page_set_permissions(cage, 4096, READ_WRITE, metered) });
            assert!(unsafe { snapi_v8_test_page_set_permissions(cage, 4096, NO_ACCESS, metered) });
        }
        start.elapsed().as_nanos() as f64 / (2 * ITERATIONS) as f64
    };
    for round in 0..5 {
        let inner = run(false);
        let metered = run(true);
        println!(
            "round {round}: inner {inner:.1} ns/op, metered {metered:.1} ns/op, delta {:.1} ns",
            metered - inner
        );
    }
    assert!(unsafe { snapi_v8_test_page_free(cage, 1 << 30) });
    env.release();
    lane.finish();
}
