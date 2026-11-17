#![cfg(all(
    feature = "experimental-async",
    any(
        all(
            target_os = "linux",
            target_arch = "x86_64",
            target_env = "gnu",
            target_pointer_width = "64"
        ),
        all(
            target_os = "macos",
            any(target_arch = "x86_64", target_arch = "aarch64"),
            target_pointer_width = "64"
        )
    )
))]

//! Native V8 JSPI promises must stay independent from parked Wasmer roots.

use std::{
    future::Future,
    process::{Child, Command},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    task::{Context, Poll, Waker},
    thread,
    time::{Duration, Instant},
};

use wasmer::{
    AsStoreAsync, AsStoreMut, Extern, Function, FunctionType, Instance, Module, Store, Value,
    sys::{NativeEngineStackBounds, NativeInvocationScope},
};
use wasmer_napi::NapiCtx;

unsafe extern "C" {
    fn snapi_bridge_native_current_stack_context(
        low: *mut usize,
        base: *mut usize,
        saved_size: *mut usize,
        applied_size: *mut usize,
        origin: *mut usize,
    ) -> i32;
}

fn native_bridge_scope_active() -> bool {
    let (mut low, mut base, mut saved_size, mut applied_size, mut origin) = (0, 0, 0, 0, 0);
    // This host-unregistered diagnostic reports cached scope metadata. The
    // zero result outside a bridge scope reads no V8 private fields.
    unsafe {
        snapi_bridge_native_current_stack_context(
            &mut low,
            &mut base,
            &mut saved_size,
            &mut applied_size,
            &mut origin,
        ) != 0
    }
}

// Imports `env.wait(i32) -> i32`, and exports `run(i32) -> i32` which adds two
// to the imported result. This is instantiated by V8 in the N-API isolate.
const JSPI_WASM: &[u8] = &[
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // header
    0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f, // (i32) -> i32
    0x02, 0x0c, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x04, 0x77, 0x61, 0x69, 0x74, 0x00, 0x00, 0x03, 0x02,
    0x01, 0x00, // one defined function
    0x07, 0x07, 0x01, 0x03, 0x72, 0x75, 0x6e, 0x00, 0x01, // export `run`
    0x0a, 0x0b, 0x01, 0x09, 0x00, 0x20, 0x00, 0x10, 0x00, 0x41, 0x02, 0x6a, 0x0b,
];

const NAPI_GUEST: &str = r#"(module
    (import "env" "memory" (memory 1 1024 shared))
    (import "napi" "napi_wasm_init_env" (func $new_env (result i32)))
    (import "napi" "napi_create_string_utf8"
        (func $string (param i32 i32 i32 i32) (result i32)))
    (import "napi" "napi_run_script" (func $run (param i32 i32 i32) (result i32)))
    (import "napi" "napi_get_value_int32" (func $integer (param i32 i32 i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
        (func $configure_runtime (param i32) (result i32)))
    (import "napi_extension_wasmer_v0" "unofficial_napi_event_loop_checkpoint"
        (func $checkpoint (param i32 i32 i32 i32) (result i32)))
    (import "test" "record" (func $record (param i32)))
    (import "test" "async_barrier" (func $async_barrier))
    (global $napi_env (mut i32) (i32.const 0))
    (func $check (param i32) local.get 0 if unreachable end)
    (func (export "configure_runtime_options") (param i32) (result i32)
        local.get 0 call $configure_runtime)
    (func (export "init")
        call $new_env global.set $napi_env
        global.get $napi_env i32.eqz if unreachable end)
    (func $eval (param $ptr i32) (param $len i32) (result i32)
        global.get $napi_env local.get $ptr local.get $len i32.const 16
        call $string call $check
        global.get $napi_env i32.const 16 i32.load i32.const 20
        call $run call $check
        global.get $napi_env i32.const 20 i32.load i32.const 24
        call $integer call $check
        i32.const 24 i32.load)
    (func (export "start_root")
        (param $setup_ptr i32) (param $setup_len i32)
        (param $pending_ptr i32) (param $pending_len i32)
        (result i32)
        local.get $setup_ptr local.get $setup_len call $eval
        i32.const 42 i32.ne if unreachable end
        local.get $pending_ptr local.get $pending_len call $eval
        i32.const 42 i32.ne if unreachable end
        i32.const 1 call $record
        call $async_barrier
        i32.const 42)
    (func (export "settle_root")
        (param $release_ptr i32) (param $release_len i32)
        (param $read_ptr i32) (param $read_len i32)
        (result i32)
        i32.const 2 call $record
        local.get $release_ptr local.get $release_len call $eval
        i32.const 42 i32.ne if unreachable end
        global.get $napi_env i32.const 0 i32.const 1 i32.const 512
        call $checkpoint call $check
        local.get $read_ptr local.get $read_len call $eval)
)"#;

const JSPI_SETUP: &str = r#"(() => {
  const bytes = new Uint8Array([__JSPI_WASM_BYTES__]);
  if (typeof WebAssembly.Suspending !== 'function' ||
      typeof WebAssembly.promising !== 'function') return -1;
  globalThis.__nativeJspiEntered = 0;
  globalThis.__nativeJspiSuccessSettled = false;
  globalThis.__nativeJspiFailureSettled = false;
  globalThis.__nativeJspiSuccessResult = undefined;
  globalThis.__nativeJspiFailureError = undefined;
  globalThis.__nativeJspiExpectedError = new Error('original JSPI rejection');

  let resolveSuccess;
  let rejectFailure;
  const successGate = new Promise(resolve => { resolveSuccess = resolve; });
  const failureGate = new Promise((_, reject) => { rejectFailure = reject; });
  globalThis.__nativeJspiResolveSuccess = resolveSuccess;
  globalThis.__nativeJspiRejectFailure = rejectFailure;

  const module = new WebAssembly.Module(bytes);
  const start = gate => {
    const wait = new WebAssembly.Suspending(value => {
      globalThis.__nativeJspiEntered++;
      return gate.then(() => value);
    });
    const instance = new WebAssembly.Instance(module, { env: { wait } });
    return WebAssembly.promising(instance.exports.run)(40);
  };
  globalThis.__nativeJspiSuccess = start(successGate);
  globalThis.__nativeJspiFailure = start(failureGate);
  globalThis.__nativeJspiSuccess.then(
    value => {
      globalThis.__nativeJspiSuccessSettled = true;
      globalThis.__nativeJspiSuccessResult = value;
    },
    error => {
      globalThis.__nativeJspiSuccessSettled = true;
      globalThis.__nativeJspiSuccessError = error;
    });
  globalThis.__nativeJspiFailure.then(
    value => {
      globalThis.__nativeJspiFailureSettled = true;
      globalThis.__nativeJspiFailureResult = value;
    },
    error => {
      globalThis.__nativeJspiFailureSettled = true;
      globalThis.__nativeJspiFailureError = error;
    });
  return 42;
})()"#;

const PENDING_CHECK: &str = r#"(
  __nativeJspiEntered === 2 &&
  __nativeJspiSuccess instanceof Promise &&
  __nativeJspiFailure instanceof Promise &&
  !__nativeJspiSuccessSettled &&
  !__nativeJspiFailureSettled
) ? 42 : -1"#;

const RELEASE_GATES: &str = r#"(__nativeJspiResolveSuccess(40),
  __nativeJspiRejectFailure(__nativeJspiExpectedError), 42)"#;

const READ_SETTLED: &str = r#"(
  __nativeJspiSuccessSettled &&
  __nativeJspiSuccessResult === 42 &&
  __nativeJspiFailureSettled &&
  __nativeJspiFailureError === __nativeJspiExpectedError
) ? 42 : -1"#;

fn jspi_setup_script() -> String {
    let bytes = JSPI_WASM
        .iter()
        .map(u8::to_string)
        .collect::<Vec<_>>()
        .join(",");
    JSPI_SETUP.replace("__JSPI_WASM_BYTES__", &bytes)
}

fn run_in_subprocess(test_name: &str, timeout: Duration) -> bool {
    const CHILD: &str = "NAPI_NATIVE_JSPI_CHILD";
    if std::env::var_os(CHILD).is_none() {
        struct OwnedChild(Child);
        impl Drop for OwnedChild {
            fn drop(&mut self) {
                let _ = self.0.kill();
                let _ = self.0.wait();
            }
        }
        let mut child = OwnedChild(
            Command::new(std::env::current_exe().unwrap())
                .args(["--exact", test_name, "--nocapture"])
                .env(CHILD, "1")
                .spawn()
                .unwrap(),
        );
        let started = Instant::now();
        loop {
            if let Some(status) = child.0.try_wait().unwrap() {
                assert!(status.success(), "native JSPI child failed: {status}");
                return false;
            }
            assert!(
                started.elapsed() < timeout,
                "native JSPI test child exceeded its timeout"
            );
            thread::sleep(Duration::from_millis(10));
        }
    }
    true
}

fn poll_until_ready<F: Future>(
    future: &mut std::pin::Pin<Box<F>>,
    rounds: usize,
) -> Option<F::Output> {
    let mut cx = Context::from_waker(Waker::noop());
    for _ in 0..rounds {
        if let Poll::Ready(output) = future.as_mut().poll(&mut cx) {
            return Some(output);
        }
    }
    None
}

fn run_native_jspi_proof() {
    let mut store = Store::default();
    let ctx = NapiCtx::default();
    let module = Module::new(&store, wat::parse_str(NAPI_GUEST).unwrap()).unwrap();
    let hooks = ctx.runtime_hooks();
    let (mut imports, state) = hooks
        .additional_imports(&module, &mut store.as_store_mut())
        .unwrap();
    let session = state.session().unwrap().clone();
    let observations = Arc::new(Mutex::new(Vec::new()));
    let recorded = observations.clone();
    imports.define(
        "test",
        "record",
        Function::new_typed(&mut store, move |tag: i32| {
            let bounds = NativeEngineStackBounds::current()
                .expect("ordinary record import must run on a native API stack");
            recorded.lock().unwrap().push((tag, bounds));
        }),
    );

    let barrier_entered = Arc::new(AtomicBool::new(false));
    let barrier_pending = Arc::new(AtomicBool::new(false));
    let barrier_release = Arc::new(AtomicBool::new(false));
    let entered = barrier_entered.clone();
    let pending = barrier_pending.clone();
    let release = barrier_release.clone();
    imports.define(
        "test",
        "async_barrier",
        Function::new_async(&mut store, FunctionType::new([], []), move |_| {
            assert!(NativeEngineStackBounds::current().is_some());
            assert!(
                !native_bridge_scope_active(),
                "the N-API V8 bridge scope remained active at the async barrier"
            );
            entered.store(true, Ordering::SeqCst);
            let release = release.clone();
            let pending = pending.clone();
            async move {
                std::future::poll_fn(|_| {
                    if release.load(Ordering::SeqCst) {
                        Poll::Ready(())
                    } else {
                        pending.store(true, Ordering::SeqCst);
                        Poll::Pending
                    }
                })
                .await;
                Ok(Vec::new())
            }
        }),
    );

    let Some(Extern::Memory(memory)) = imports.get_export("env", "memory") else {
        panic!("NAPI did not create the imported memory");
    };
    let instance = Instance::new(&mut store, &module, &imports).unwrap();
    hooks
        .configure_instance(
            &module,
            &mut store.as_store_mut(),
            &instance,
            Some(&memory),
            state,
        )
        .unwrap();

    // Native V8 flags are process-global. The test process therefore runs in
    // the child wrapper above, and configures JSPI before the first N-API env.
    let flags = b"--experimental-wasm-jspi --stack-size=512";
    let options: Vec<_> = [16_u32, 1, 256, flags.len() as u32]
        .into_iter()
        .flat_map(u32::to_le_bytes)
        .collect();
    memory.view(&store).write(128, &options).unwrap();
    memory.view(&store).write(256, flags).unwrap();
    assert_eq!(
        instance
            .exports
            .get_typed_function::<i32, i32>(&store, "configure_runtime_options")
            .unwrap()
            .call(&mut store, 128)
            .unwrap(),
        0
    );
    instance
        .exports
        .get_typed_function::<(), ()>(&store, "init")
        .unwrap()
        .call(&mut store)
        .unwrap();

    let setup = jspi_setup_script();
    let pending = PENDING_CHECK;
    let release = RELEASE_GATES;
    let read = READ_SETTLED;
    for (offset, script) in [
        (1024, setup.as_str()),
        (4096, pending),
        (8192, release),
        (12288, read),
    ] {
        memory
            .view(&store)
            .write(offset, script.as_bytes())
            .unwrap();
    }

    let start = instance.exports.get_function("start_root").unwrap().clone();
    let settle = instance
        .exports
        .get_function("settle_root")
        .unwrap()
        .clone();
    let scope = NativeInvocationScope::bind(&mut store).unwrap();
    let store = store.into_async();

    let mut first = Box::pin(start.call_async(
        &store,
        vec![
            Value::I32(1024),
            Value::I32(setup.len() as i32),
            Value::I32(4096),
            Value::I32(pending.len() as i32),
        ],
    ));
    let mut cx = Context::from_waker(Waker::noop());
    let mut first_poll = Poll::Pending;
    for _ in 0..8 {
        first_poll = first.as_mut().poll(&mut cx);
        if first_poll.is_ready() || barrier_entered.load(Ordering::SeqCst) {
            break;
        }
    }
    assert!(
        matches!(first_poll, Poll::Pending),
        "first root did not park"
    );
    assert!(
        barrier_entered.load(Ordering::SeqCst),
        "async barrier was not entered"
    );
    assert!(
        barrier_pending.load(Ordering::SeqCst),
        "async barrier did not return Pending"
    );
    let first_bounds = {
        let observed = observations.lock().unwrap();
        assert_eq!(observed.len(), 1);
        assert_eq!(observed[0].0, 1);
        observed[0].1
    };

    // The first callFuture is still pending, retaining its stack allocation,
    // but its N-API script and all V8 bridge scopes have returned.
    let mut second = Box::pin(settle.call_async(
        &store,
        vec![
            Value::I32(8192),
            Value::I32(release.len() as i32),
            Value::I32(12288),
            Value::I32(read.len() as i32),
        ],
    ));
    let second_result = poll_until_ready(&mut second, 16)
        .expect("second root did not complete without the barrier");
    assert_eq!(second_result.unwrap()[0].i32(), Some(42));
    let second_bounds = {
        let observed = observations.lock().unwrap();
        assert_eq!(observed.len(), 2);
        assert_eq!(observed[1].0, 2);
        observed[1].1
    };
    assert!(
        first_bounds.base <= second_bounds.usable_low
            || second_bounds.base <= first_bounds.usable_low,
        "pending JSPI root and second root reused overlapping native stacks: {first_bounds:?} {second_bounds:?}"
    );
    assert!(
        first.as_mut().poll(&mut cx).is_pending(),
        "the finite barrier released before the test released it"
    );

    barrier_release.store(true, Ordering::SeqCst);
    let first_result = poll_until_ready(&mut first, 16)
        .expect("first root did not settle after releasing its finite barrier");
    assert_eq!(first_result.unwrap()[0].i32(), Some(42));
    drop(first);
    drop(second);

    scope.close();
    {
        let mut guard = store.store().write();
        // SAFETY: both roots and both JSPI promises settled, the finite host
        // barrier was released, and this test owns the session and Store.
        unsafe { session.deactivate(&mut guard.as_store_mut()) };
    }
    drop(scope);
    drop(start);
    drop(settle);
    drop(imports);
    drop(memory);
    drop(module);
    drop(hooks);
    drop(instance);
    drop(session);
    drop(store);

    assert_eq!(ctx.active_sessions(), 0);
    let usage = ctx.budget().snapshot();
    assert_eq!(usage.mem_charged, 0);
    assert_eq!(usage.live_isolates, 0);
    assert_eq!(usage.wasm_linear, 0);
    assert_eq!(usage.v8_heap_reserved, 0);
    assert_eq!(usage.v8_external, 0);
}

#[test]
fn native_v8_jspi_suspension_survives_a_second_root_on_a_disjoint_stack() {
    if !run_in_subprocess(
        "native_v8_jspi_suspension_survives_a_second_root_on_a_disjoint_stack",
        Duration::from_secs(45),
    ) {
        return;
    }
    run_native_jspi_proof();
}
