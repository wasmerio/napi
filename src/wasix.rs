//! N-API bindings paired with each actual WASIX v2 instance.

use crate::{NapiCtx, NapiRuntimeHooks, NapiSession};
#[cfg(not(target_arch = "wasm32"))]
use crate::{NativeGuestChunk, NativeGuestRefill, Pool, ResourceBudget};
#[cfg(not(target_arch = "wasm32"))]
use std::sync::{Arc, Mutex, Weak};
#[cfg(not(target_arch = "wasm32"))]
use wasixv2_exec::GuestMemory;
use wasixv2_exec::{ExecEnv, InstanceAttachment, InstantiationHook};
#[cfg(not(target_arch = "wasm32"))]
use wasixv2_host::{LoaderCleanup, LocalFuture};
use wasixv2_syscalls::LoaderControl;
#[cfg(not(target_arch = "wasm32"))]
use wasixv2_syscalls::{Caller, HostMemoryLease};
use wasmer::{FunctionEnv, Imports, Instance, Memory, Module, RuntimeError, StoreMut};

/// Registers N-API independently of the process ABI.
pub struct NapiHook {
    #[cfg(not(target_arch = "wasm32"))]
    context: NapiCtx,
    hooks: NapiRuntimeHooks,
}
impl NapiHook {
    pub fn new(context: NapiCtx) -> Self {
        Self {
            hooks: context.runtime_hooks(),
            #[cfg(not(target_arch = "wasm32"))]
            context,
        }
    }
}

struct Attachment<S> {
    #[cfg(not(target_arch = "wasm32"))]
    context: NapiCtx,
    session: NapiSession,
    exec: FunctionEnv<ExecEnv<S>>,
}

impl<S: LoaderControl + Send + Sync + 'static> InstantiationHook<S> for NapiHook {
    fn requires_owning_thread(&self) -> bool {
        cfg!(not(target_arch = "wasm32"))
    }

    fn provides_import(&self, namespace: &str, name: &str) -> bool {
        NapiCtx::provides_import(namespace, name)
    }

    fn prepare_imports(
        &self,
        module: &Module,
        store: &mut StoreMut<'_>,
        exec: &FunctionEnv<ExecEnv<S>>,
        imports: &mut Imports,
    ) -> Result<Option<Box<dyn InstanceAttachment>>, RuntimeError> {
        let state = self
            .hooks
            .add_imports(module, store, imports)
            .map_err(|error| RuntimeError::new(error.to_string()))?;
        let Some(session) = state.session().cloned() else {
            return Ok(None);
        };
        Ok(Some(Box::new(Attachment {
            #[cfg(not(target_arch = "wasm32"))]
            context: self.context.clone(),
            session,
            exec: exec.clone(),
        })))
    }
}

impl<S: LoaderControl + Send + Sync + 'static> InstanceAttachment for Attachment<S> {
    fn configure(
        &mut self,
        _: &Module,
        store: &mut StoreMut<'_>,
        instance: &Instance,
        memory: &Memory,
    ) -> Result<(), RuntimeError> {
        #[cfg(not(target_arch = "wasm32"))]
        {
            let budget = self.context.budget();
            let accounted = memory
                .host_memory_access(&*store)
                .is_ok_and(|access| access.is_accounted_by(&budget.accounting_identity()));
            let arena = self
                .context
                .native_guest_arena(&*store, memory)
                .ok_or_else(|| {
                    RuntimeError::new("native N-API requires stable native guest memory")
                })?;
            self.session
                .configure_leased_instance(
                    store,
                    instance,
                    memory,
                    arena,
                    Arc::new(Refill {
                        exec: Mutex::new(self.exec.clone()),
                        budget,
                        accounted,
                    }),
                )
                .map_err(|error| RuntimeError::new(error.to_string()))
        }
        #[cfg(target_arch = "wasm32")]
        {
            let _ = self.exec.as_ref(&*store).task();
            self.session
                .configure_instance(store, instance, Some(memory))
                .map_err(|error| RuntimeError::new(error.to_string()))
        }
    }

    fn deactivate(&mut self, store: &mut StoreMut<'_>) {
        // The existing attachment guard settles native generations and takes
        // the original Store's write custody before calling this on its owner.
        unsafe { self.session.deactivate(store) };
    }
}

#[cfg(not(target_arch = "wasm32"))]
struct Refill<S> {
    exec: Mutex<FunctionEnv<ExecEnv<S>>>,
    budget: Arc<ResourceBudget>,
    accounted: bool,
}
// SAFETY: FunctionEnv is an exact Store-local handle, not an owning Store.
// Only the current operation captures an async loan; it is never retained here.
#[cfg(not(target_arch = "wasm32"))]
unsafe impl<S: LoaderControl + Send + Sync + 'static> NativeGuestRefill for Refill<S> {
    fn reserve(
        &self,
        store: &mut StoreMut<'_>,
        bytes: u32,
    ) -> Result<Option<NativeGuestChunk>, RuntimeError> {
        // Match InstanceHooks' store-local handle custody. Only clone under
        // this lock; no Store access, callout or suspension holds its guard.
        let exec = self
            .exec
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .clone();
        let (syscalls, task, cleanup) = {
            let data = exec.as_ref(&*store);
            let Some(cleanup) = data.loader_cleanup().cloned() else {
                return Ok(None);
            };
            (data.syscalls().clone(), data.task(), cleanup)
        };
        // The actual backing authority, rather than a context-wide flag,
        // establishes whether the engine already charges this funding source.
        let charge = if self.accounted {
            None
        } else {
            let Some(charge) = store.parked(|| LeaseCharge::new(self.budget.clone(), bytes).ok())
            else {
                return Ok(None);
            };
            Some(charge)
        };
        if !wasmer::sys::native_host_work_can_suspend() {
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                wasixv2_exec::poll_native_host_reservation(&exec, store, bytes)
            }));
            // Refusal, reconciliation and rollback can call the accountant.
            // Release the checked Store borrow before any of those callouts.
            return store.parked(|| match result {
                Ok(std::task::Poll::Ready(Ok(lease))) => {
                    Ok(account_lease(lease, charge).map(|lease| chunk(lease, cleanup)))
                }
                Ok(std::task::Poll::Ready(Err(_)) | std::task::Poll::Pending) => {
                    drop(charge);
                    Ok(None)
                }
                Err(first) => {
                    if let Err(secondary) =
                        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| drop(charge)))
                    {
                        discard_panic(secondary);
                    }
                    std::panic::resume_unwind(first)
                }
            });
        }
        // Install the exact borrowed suffix first. Capture its checked async
        // loan only through a counted temporary; release all T/frame/Store
        // wrappers before the driver can yield this suffix.
        store.parked(|| {
            let env = capture_environment(&exec);
            let Some(env) = env else {
                return Ok(None);
            };
            wasmer::sys::run_native_host_future(async move {
                let memory = GuestMemory::new(&env);
                let caller = Caller::new(task, &memory);
                let reservation = syscalls.loader_reserve_host(&caller, bytes, 65536);
                let Ok(lease) = ReservationFuture(Charged {
                    value: Some(reservation),
                    charge,
                })
                .await
                else {
                    return Ok(None);
                };
                Ok(Some(chunk(lease, cleanup)))
            })
        })
    }
}

#[cfg(not(target_arch = "wasm32"))]
fn capture_environment<T: Send + 'static>(
    environment: &FunctionEnv<T>,
) -> Option<wasmer::AsyncFunctionEnvMut<T>> {
    // A precharge callout may detach the exact environment. Restore the
    // counted current-Store wrapper before replaying any capture failure.
    let outcome = wasmer::Store::with_current(|current| {
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            environment.clone().into_mut(current).as_async_mut()
        }))
    });
    match outcome {
        Some(Ok(environment)) => environment,
        Some(Err(failure)) => std::panic::resume_unwind(failure),
        None => None,
    }
}

#[cfg(not(target_arch = "wasm32"))]
struct LeaseCharge {
    budget: Arc<ResourceBudget>,
    bytes: u64,
}
#[cfg(not(target_arch = "wasm32"))]
impl LeaseCharge {
    fn new(budget: Arc<ResourceBudget>, bytes: u32) -> Result<Self, crate::OverBudget> {
        budget.try_charge(Pool::WasmLinear, u64::from(bytes))?;
        Ok(Self {
            budget,
            bytes: u64::from(bytes),
        })
    }
    fn reconcile(&mut self, bytes: u32) -> Result<(), crate::OverBudget> {
        let bytes = u64::from(bytes);
        if bytes > self.bytes {
            self.budget
                .try_charge(Pool::WasmLinear, bytes - self.bytes)?;
            self.bytes = bytes;
        } else if bytes < self.bytes {
            let excess = self.bytes - bytes;
            self.bytes = bytes;
            self.budget.uncharge(Pool::WasmLinear, excess);
        }
        Ok(())
    }
}
#[cfg(not(target_arch = "wasm32"))]
impl Drop for LeaseCharge {
    fn drop(&mut self) {
        let bytes = std::mem::take(&mut self.bytes);
        let released = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            self.budget.uncharge(Pool::WasmLinear, bytes)
        }));
        if let Err(failure) = released {
            if std::thread::panicking() {
                discard_panic(failure);
            } else {
                std::panic::resume_unwind(failure);
            }
        }
    }
}

#[cfg(not(target_arch = "wasm32"))]
fn discard_panic(failure: Box<dyn std::any::Any + Send>) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| drop(failure)));
}

// Keep a pending reservation, exact lease, or release continuation before its
// charge. Explicit Drop also preserves the first destructor panic.
#[cfg(not(target_arch = "wasm32"))]
struct Charged<T> {
    value: Option<T>,
    charge: Option<LeaseCharge>,
}
#[cfg(not(target_arch = "wasm32"))]
impl<T> Charged<T> {
    fn dispose(&mut self) {
        use std::panic::{AssertUnwindSafe, catch_unwind, resume_unwind};
        let unwinding = std::thread::panicking();
        let value = catch_unwind(AssertUnwindSafe(|| drop(self.value.take())));
        let charge = catch_unwind(AssertUnwindSafe(|| drop(self.charge.take())));
        let failure = match (value, charge) {
            (Err(first), Err(second)) => {
                discard_panic(second);
                Some(first)
            }
            (Err(first), Ok(())) | (Ok(()), Err(first)) => Some(first),
            (Ok(()), Ok(())) => None,
        };
        if let Some(failure) = failure {
            if unwinding {
                discard_panic(failure);
            } else {
                resume_unwind(failure);
            }
        }
    }
}
#[cfg(not(target_arch = "wasm32"))]
impl<T> Drop for Charged<T> {
    fn drop(&mut self) {
        self.dispose();
    }
}

#[cfg(not(target_arch = "wasm32"))]
fn account_lease(
    lease: Box<dyn HostMemoryLease>,
    charge: Option<LeaseCharge>,
) -> Option<Box<dyn HostMemoryLease>> {
    let Some(charge) = charge else {
        return Some(lease);
    };
    let mut owned = Charged {
        value: Some(lease),
        charge: Some(charge),
    };
    let bytes = owned.value.as_ref().unwrap().length();
    owned.charge.as_mut().unwrap().reconcile(bytes).ok()?;
    Some(Box::new(owned))
}

// SAFETY: all pointer/range custody stays with the exact underlying lease.
// Release and Drop destroy that custody before releasing its accounting charge.
#[cfg(not(target_arch = "wasm32"))]
unsafe impl HostMemoryLease for Charged<Box<dyn HostMemoryLease>> {
    fn address(&self) -> u32 {
        self.value.as_ref().unwrap().address()
    }
    fn length(&self) -> u32 {
        self.value.as_ref().unwrap().length()
    }
    fn as_ptr(&self) -> *mut u8 {
        self.value.as_ref().unwrap().as_ptr()
    }
    fn release(mut self: Box<Self>) -> LocalFuture<'static, ()> {
        let future = self.value.take().unwrap().release();
        let charge = self.charge.take();
        Box::pin(ReleaseFuture(Charged {
            value: Some(future),
            charge,
        }))
    }
}

#[cfg(not(target_arch = "wasm32"))]
struct ReservationFuture<'a>(
    Charged<LocalFuture<'a, wasixv2_syscalls::SysResult<Box<dyn HostMemoryLease>>>>,
);
#[cfg(not(target_arch = "wasm32"))]
impl std::future::Future for ReservationFuture<'_> {
    type Output = wasixv2_syscalls::SysResult<Box<dyn HostMemoryLease>>;
    fn poll(
        self: std::pin::Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
    ) -> std::task::Poll<Self::Output> {
        use std::task::Poll;
        let this = self.get_mut();
        match this.0.value.as_mut().unwrap().as_mut().poll(cx) {
            Poll::Pending => Poll::Pending,
            Poll::Ready(Ok(lease)) => {
                let lease = account_lease(lease, this.0.charge.take());
                this.0.dispose();
                Poll::Ready(lease.ok_or_else(|| wasixv2_syscalls::Errno::ENOMEM.into()))
            }
            Poll::Ready(Err(error)) => {
                this.0.dispose();
                Poll::Ready(Err(error))
            }
        }
    }
}
#[cfg(not(target_arch = "wasm32"))]
struct ReleaseFuture(Charged<LocalFuture<'static, ()>>);
#[cfg(not(target_arch = "wasm32"))]
impl std::future::Future for ReleaseFuture {
    type Output = ();
    fn poll(
        self: std::pin::Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
    ) -> std::task::Poll<()> {
        let this = self.get_mut();
        let result = this.0.value.as_mut().unwrap().as_mut().poll(cx);
        if result.is_ready() {
            this.0.dispose();
        }
        result
    }
}

#[cfg(not(target_arch = "wasm32"))]
fn chunk(lease: Box<dyn HostMemoryLease>, cleanup: Arc<LoaderCleanup>) -> NativeGuestChunk {
    let address = lease.address();
    let length = lease.length();
    let pointer = lease.as_ptr();
    let owner = Box::new(LeaseOwner {
        lease: Some(lease),
        cleanup: Arc::downgrade(&cleanup),
    });
    // SAFETY: the producer pairs exact private logical-range and native pin
    // custody. LeaseOwner retains both through the final backing-store Drop.
    unsafe { NativeGuestChunk::new(address, length, pointer, owner) }
}

#[cfg(not(target_arch = "wasm32"))]
struct LeaseOwner {
    lease: Option<Box<dyn HostMemoryLease>>,
    cleanup: Weak<LoaderCleanup>,
}
#[cfg(not(target_arch = "wasm32"))]
impl Drop for LeaseOwner {
    fn drop(&mut self) {
        let Some(lease) = self.lease.take() else {
            return;
        };
        // The closure crosses threads; its local future is constructed by the
        // original runner. Closed-scope rejection drops the exact lease, whose
        // contract requires safe host-only release without an unpolled job.
        let job = Box::new(move || lease.release());
        if let Some(cleanup) = self.cleanup.upgrade() {
            if let Err(job) = cleanup.submit(job) {
                drop(job);
            }
        } else {
            drop(job);
        }
    }
}

#[cfg(all(test, not(target_arch = "wasm32")))]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use wasixv2_host::TokioHost;

    #[derive(Default)]
    struct Counts {
        released: AtomicUsize,
        dropped: AtomicUsize,
    }
    struct Lease {
        bytes: Box<[u8; 4]>,
        counts: Arc<Counts>,
    }
    // SAFETY: the Box owns stable, private writable storage until this exact
    // lease is dropped. Neither release nor Drop requires a Store or executor.
    unsafe impl HostMemoryLease for Lease {
        fn address(&self) -> u32 {
            65536
        }
        fn length(&self) -> u32 {
            self.bytes.len() as u32
        }
        fn as_ptr(&self) -> *mut u8 {
            self.bytes.as_ptr().cast_mut()
        }
        fn release(self: Box<Self>) -> LocalFuture<'static, ()> {
            Box::pin(async move {
                self.counts.released.fetch_add(1, Ordering::SeqCst);
                drop(self);
            })
        }
    }
    impl Drop for Lease {
        fn drop(&mut self) {
            self.counts.dropped.fetch_add(1, Ordering::SeqCst);
        }
    }
    fn backing(scope: &Arc<LoaderCleanup>, counts: &Arc<Counts>) -> NativeGuestChunk {
        chunk(
            Box::new(Lease {
                bytes: Box::new([42; 4]),
                counts: counts.clone(),
            }),
            scope.clone(),
        )
    }

    #[tokio::test]
    async fn backing_drop_transfers_to_the_original_runner() {
        tokio::task::LocalSet::new()
            .run_until(async {
                let scope = LoaderCleanup::new(Arc::new(TokioHost::default()));
                let counts = Arc::new(Counts::default());
                let backing = backing(&scope, &counts);
                drop(backing);
                assert_eq!(counts.released.load(Ordering::SeqCst), 0);
                assert_eq!(counts.dropped.load(Ordering::SeqCst), 0);
                scope.run(async {}).await;
                assert_eq!(counts.released.load(Ordering::SeqCst), 1);
                assert_eq!(counts.dropped.load(Ordering::SeqCst), 1);
            })
            .await;
    }

    #[tokio::test]
    async fn closed_or_expired_runner_releases_without_an_unpolled_job() {
        for expire in [false, true] {
            let scope = LoaderCleanup::new(Arc::new(TokioHost::default()));
            let counts = Arc::new(Counts::default());
            let backing = backing(&scope, &counts);
            scope.run(async {}).await;
            assert!(!scope.is_open());
            let weak = Arc::downgrade(&scope);
            if expire {
                drop(scope);
                assert!(weak.upgrade().is_none());
            }
            drop(backing);
            assert_eq!(counts.released.load(Ordering::SeqCst), 0);
            assert_eq!(counts.dropped.load(Ordering::SeqCst), 1);
        }
    }
    #[test]
    fn detached_environment_capture_restores_the_current_store_before_replaying() {
        use wasmer::sys::vm::{StoreObject, VMFunctionEnvironment};
        use wasmer::{AsStoreMut, Store};
        let mut store = Store::default();
        let environment = FunctionEnv::new(&mut store, ());
        drop(
            VMFunctionEnvironment::list_mut(store.objects_mut().as_sys_mut())
                .pop()
                .unwrap(),
        );
        let failure = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            store
                .as_store_mut()
                .parked(|| capture_environment(&environment));
        }));
        assert!(
            failure.is_err(),
            "the removed environment must remain invalid"
        );
        store.as_store_mut().parked(|| {
            assert!(
                Store::with_current(|_| ()).is_some(),
                "capture must restore counted Store custody"
            );
        });
        let fresh = FunctionEnv::new(&mut store, 42);
        assert_eq!(*fresh.as_ref(&store), 42);
        drop(store);
    }

    struct ObservedLease {
        bytes: Box<[u8]>,
        budget: Arc<ResourceBudget>,
        observed: Arc<Mutex<Vec<u64>>>,
        pending_release: bool,
        drop_panic: Option<Arc<u32>>,
        release_panic: Option<Arc<u32>>,
    }
    // SAFETY: boxed storage is private, stable, and exclusively owned until Drop.
    unsafe impl HostMemoryLease for ObservedLease {
        fn address(&self) -> u32 {
            65536
        }
        fn length(&self) -> u32 {
            self.bytes.len() as u32
        }
        fn as_ptr(&self) -> *mut u8 {
            self.bytes.as_ptr().cast_mut()
        }
        fn release(self: Box<Self>) -> LocalFuture<'static, ()> {
            if let Some(failure) = &self.release_panic {
                std::panic::panic_any(failure.clone());
            }
            Box::pin(async move {
                if self.pending_release {
                    std::future::pending::<()>().await;
                }
                drop(self);
            })
        }
    }
    impl Drop for ObservedLease {
        fn drop(&mut self) {
            self.observed
                .lock()
                .unwrap()
                .push(self.budget.memory_charged());
            if let Some(failure) = &self.drop_panic {
                std::panic::panic_any(failure.clone());
            }
        }
    }
    fn observed_lease(
        budget: &Arc<ResourceBudget>,
        observed: &Arc<Mutex<Vec<u64>>>,
        bytes: usize,
        pending_release: bool,
    ) -> Box<dyn HostMemoryLease> {
        Box::new(ObservedLease {
            bytes: vec![0; bytes].into_boxed_slice(),
            budget: budget.clone(),
            observed: observed.clone(),
            pending_release,
            drop_panic: None,
            release_panic: None,
        })
    }

    #[tokio::test]
    async fn charged_backing_keeps_funding_until_queued_or_closed_release_finishes() {
        tokio::task::LocalSet::new()
            .run_until(async {
                for close in [false, true] {
                    let budget = ResourceBudget::with_memory_limit(32);
                    let observed = Arc::new(Mutex::new(Vec::new()));
                    let scope = LoaderCleanup::new(Arc::new(TokioHost::default()));
                    let lease = account_lease(
                        observed_lease(&budget, &observed, 4, false),
                        Some(LeaseCharge::new(budget.clone(), 8).unwrap()),
                    )
                    .unwrap();
                    assert_eq!(
                        budget.memory_charged(),
                        4,
                        "actual length reconciles before publication"
                    );
                    let backing = chunk(lease, scope.clone());
                    if close {
                        scope.run(async {}).await;
                    }
                    drop(backing);
                    if !close {
                        assert!(observed.lock().unwrap().is_empty());
                        assert_eq!(budget.memory_charged(), 4);
                        scope.run(async {}).await;
                    }
                    assert_eq!(*observed.lock().unwrap(), vec![4]);
                    assert_eq!(budget.memory_charged(), 0);
                }
            })
            .await;
    }

    #[test]
    fn canceled_or_unpolled_reservation_and_release_drop_before_their_charge() {
        use std::{
            future::Future,
            task::{Context, Waker},
        };
        struct ReservationDrop(Arc<ResourceBudget>, Arc<Mutex<Vec<u64>>>);
        impl Drop for ReservationDrop {
            fn drop(&mut self) {
                self.1.lock().unwrap().push(self.0.memory_charged());
            }
        }
        for poll in [false, true] {
            let budget = ResourceBudget::with_memory_limit(32);
            let observed = Arc::new(Mutex::new(Vec::new()));
            let owner = ReservationDrop(budget.clone(), observed.clone());
            let pending: LocalFuture<'_, wasixv2_syscalls::SysResult<Box<dyn HostMemoryLease>>> =
                Box::pin(async move {
                    let _owner = owner;
                    std::future::pending().await
                });
            let mut reservation = Box::pin(ReservationFuture(Charged {
                value: Some(pending),
                charge: Some(LeaseCharge::new(budget.clone(), 8).unwrap()),
            }));
            if poll {
                assert!(
                    reservation
                        .as_mut()
                        .poll(&mut Context::from_waker(Waker::noop()))
                        .is_pending()
                );
            }
            drop(reservation);
            assert_eq!(*observed.lock().unwrap(), vec![8]);
            assert_eq!(budget.memory_charged(), 0);
            observed.lock().unwrap().clear();
            let lease = account_lease(
                observed_lease(&budget, &observed, 8, true),
                Some(LeaseCharge::new(budget.clone(), 8).unwrap()),
            )
            .unwrap();
            let mut release = lease.release();
            if poll {
                assert!(
                    release
                        .as_mut()
                        .poll(&mut Context::from_waker(Waker::noop()))
                        .is_pending()
                );
            }
            drop(release);
            assert_eq!(*observed.lock().unwrap(), vec![8]);
            assert_eq!(budget.memory_charged(), 0);
        }
    }

    #[test]
    fn refused_actual_length_releases_lease_before_rolling_back_precharge() {
        let budget = ResourceBudget::with_memory_limit(8);
        let observed = Arc::new(Mutex::new(Vec::new()));
        let charge = LeaseCharge::new(budget.clone(), 4).unwrap();
        assert!(
            account_lease(observed_lease(&budget, &observed, 16, false), Some(charge)).is_none()
        );
        assert_eq!(*observed.lock().unwrap(), vec![4]);
        assert_eq!(budget.memory_charged(), 0);
    }

    #[test]
    fn first_lease_or_release_panic_survives_secondary_accountant_failures() {
        use std::sync::atomic::AtomicU64;
        struct Accountant {
            charged: AtomicU64,
            uncharge: Arc<u32>,
            last_drop: Arc<u32>,
            drops: Arc<AtomicUsize>,
        }
        impl crate::NapiMemoryAccountant for Accountant {
            fn memory_limit(&self) -> u64 {
                32
            }
            fn memory_charged(&self) -> u64 {
                self.charged.load(Ordering::SeqCst)
            }
            fn try_charge(&self, bytes: u64) -> bool {
                self.charged.fetch_add(bytes, Ordering::SeqCst);
                true
            }
            fn uncharge(&self, bytes: u64) {
                self.charged.fetch_sub(bytes, Ordering::SeqCst);
                std::panic::panic_any(self.uncharge.clone());
            }
        }
        impl Drop for Accountant {
            fn drop(&mut self) {
                self.drops.fetch_add(1, Ordering::SeqCst);
                std::panic::panic_any(self.last_drop.clone());
            }
        }
        for release in [false, true] {
            let first = Arc::new(101);
            let drops = Arc::new(AtomicUsize::new(0));
            let budget = ResourceBudget::with_accountant(Arc::new(Accountant {
                charged: AtomicU64::new(0),
                uncharge: Arc::new(202),
                last_drop: Arc::new(303),
                drops: drops.clone(),
            }));
            let observed = Arc::new(Mutex::new(Vec::new()));
            let underlying = ObservedLease {
                bytes: vec![0; 8].into_boxed_slice(),
                budget: budget.clone(),
                observed: observed.clone(),
                pending_release: false,
                drop_panic: (!release).then(|| first.clone()),
                release_panic: release.then(|| first.clone()),
            };
            let lease = account_lease(
                Box::new(underlying),
                Some(LeaseCharge::new(budget.clone(), 8).unwrap()),
            )
            .unwrap();
            drop(budget);
            let failure = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                if release {
                    drop(lease.release());
                } else {
                    drop(lease);
                }
            }))
            .unwrap_err();
            assert!(Arc::ptr_eq(
                failure.downcast_ref::<Arc<u32>>().unwrap(),
                &first
            ));
            assert_eq!(*observed.lock().unwrap(), vec![8]);
            assert_eq!(drops.load(Ordering::SeqCst), 1);
        }
    }
}

#[cfg(test)]
mod import_tests {
    use crate::NapiCtx;
    use crate::NapiRuntimeHooks;
    use wasmer::{AsStoreMut, Extern, Imports, MemoryType, Module, Pages, Store};

    #[test]
    fn hook_accepts_custom_loader_control() {
        struct Custom;
        impl wasixv2_syscalls::LoaderControl for Custom {}
        let hook: std::sync::Arc<dyn wasixv2_exec::InstantiationHook<Custom>> =
            std::sync::Arc::new(super::NapiHook::new(NapiCtx::default()));
        assert_eq!(hook.requires_owning_thread(), !cfg!(target_arch = "wasm32"));
        assert!(hook.provides_import("napi", "napi_wasm_init_env"));
        assert!(!hook.provides_import("custom", "missing"));
    }

    #[test]
    fn prepare_imports_reuses_wasix_memory() {
        let mut store = Store::default();
        let module = Module::new(
            &store,
            r#"(module
                (import "env" "memory" (memory 1 1))
                (import "napi" "napi_get_undefined"
                    (func (param i32 i32) (result i32)))
            )"#,
        )
        .expect("module compiles");
        let memory =
            wasmer::Memory::new(&mut store, MemoryType::new(Pages(1), Some(Pages(1)), false))
                .expect("WASIX memory can be created");
        memory
            .view(&store)
            .write(32, &[1, 2, 3, 4])
            .expect("marker can be written");
        let mut imports = Imports::new();
        imports.define("env", "memory", memory);

        let hooks = NapiCtx::default().runtime_hooks();
        let _state =
            NapiRuntimeHooks::add_imports(&hooks, &module, &mut store.as_store_mut(), &mut imports)
                .expect("N-API imports can be prepared");

        let Some(Extern::Memory(prepared_memory)) = imports.get_export("env", "memory") else {
            panic!("prepared imports must contain env.memory");
        };
        let mut marker = [0; 4];
        prepared_memory
            .view(&store)
            .read(32, &mut marker)
            .expect("marker can be read through prepared memory");
        assert_eq!(marker, [1, 2, 3, 4]);
        assert_eq!(
            imports
                .iter()
                .filter(|(namespace, name, _)| *namespace == "env" && *name == "memory")
                .count(),
            1
        );
    }
}
