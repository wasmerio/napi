//! Exact import snapshots and physical native-engine cleanup custody.
//!
//! TLS contains weak metadata only. The C++ stack owns each opaque holder and
//! restores its physical header and isolate before releasing that holder.

use crate::{NapiEnv, env::NativeRetirement};
use std::{
    any::Any,
    cell::RefCell,
    ffi::c_void,
    panic::{AssertUnwindSafe, catch_unwind, resume_unwind},
    rc::{Rc, Weak},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};
use wasmer::{
    RuntimeError,
    sys::{
        FunctionEnvFramePin, NativeEngineEntryIdentity, NativeEngineFrame, NativeEngineInvocation,
        NativeEngineStackBounds, NativeSynchronousCallback,
    },
};

pub(crate) enum CallbackFailure {
    Guest(RuntimeError),
    Panic(Box<dyn Any + Send>),
}

#[derive(Default)]
struct FailureState {
    first: Option<CallbackFailure>,
    // The original payload is presently travelling through a Rust-only unwind
    // or Result. Cleanup must not fill its slot with a secondary failure.
    replaying: bool,
}
#[derive(Clone, Default)]
pub(crate) struct FirstFailure(Rc<RefCell<FailureState>>);
impl FirstFailure {
    pub(crate) fn record(&self, failure: CallbackFailure) {
        let rejected = {
            let mut state = self.0.borrow_mut();
            if state.first.is_none() && !state.replaying {
                state.first = Some(failure);
                None
            } else {
                Some(failure)
            }
        };
        discard(rejected);
    }
    pub(crate) fn failed(&self) -> bool {
        let state = self.0.borrow();
        state.first.is_some() || state.replaying
    }
    pub(crate) fn replay(&self) -> Result<(), RuntimeError> {
        let panic = {
            let mut state = self.0.borrow_mut();
            match state.first.as_ref() {
                Some(CallbackFailure::Guest(error)) => return Err(error.clone()),
                Some(CallbackFailure::Panic(_)) => {}
                None => return Ok(()),
            }
            state.replaying = true;
            match state.first.take().unwrap() {
                CallbackFailure::Panic(panic) => panic,
                CallbackFailure::Guest(_) => unreachable!(),
            }
        };
        resume_unwind(panic)
    }
    pub(crate) fn body_finished(&self, failure: Option<CallbackFailure>) {
        // A replayed payload is now owned again, after every counted Store
        // wrapper and the consumed FunctionEnvMut have been restored/dropped.
        self.0.borrow_mut().replaying = false;
        if let Some(failure) = failure {
            self.record(failure);
        }
    }
    pub(crate) fn take(&self) -> Option<CallbackFailure> {
        self.0.borrow_mut().first.take()
    }
}
fn discard<T>(value: T) {
    let _ = catch_unwind(AssertUnwindSafe(|| drop(value)));
}

pub(crate) struct ImportRow {
    pub(crate) environment: Option<Rc<FunctionEnvFramePin<NapiEnv>>>,
}
struct ImportEntry {
    row: Weak<ImportRow>,
    failure: Weak<RefCell<FailureState>>,
    stopped: Option<std::sync::Weak<AtomicBool>>,
    identity: NativeEngineEntryIdentity,
}
thread_local! {
    static IMPORTS: RefCell<Vec<ImportEntry>> = const { RefCell::new(Vec::new()) };
}
struct Selected {
    row: Option<Rc<ImportRow>>,
    failure: FirstFailure,
    stopped: Option<Arc<AtomicBool>>,
}
fn select() -> Option<Selected> {
    IMPORTS.with(|entries| {
        entries.borrow().iter().rev().find_map(|entry| {
            if !entry.identity.is_current() {
                return None;
            }
            Some(Selected {
                row: entry.row.upgrade(),
                failure: FirstFailure(entry.failure.upgrade()?),
                stopped: entry.stopped.as_ref().and_then(std::sync::Weak::upgrade),
            })
        })
    })
}

/// An actual Rust import owns its row. Removal is by exact allocation identity;
/// a suspended unrelated root may have installed newer entries on this thread.
pub(crate) struct ImportScope {
    row: Option<Rc<ImportRow>>,
    identity: Weak<ImportRow>,
    pub(crate) failure: FirstFailure,
}
impl ImportScope {
    pub(crate) fn enter(
        environment: Option<FunctionEnvFramePin<NapiEnv>>,
        stopped: Option<Arc<AtomicBool>>,
    ) -> Self {
        Self::with_failure(environment, stopped, FirstFailure::default())
    }
    fn with_failure(
        environment: Option<FunctionEnvFramePin<NapiEnv>>,
        stopped: Option<Arc<AtomicBool>>,
        failure: FirstFailure,
    ) -> Self {
        let row = Rc::new(ImportRow {
            environment: environment.map(Rc::new),
        });
        let identity = Rc::downgrade(&row);
        IMPORTS.with(|entries| {
            entries.borrow_mut().push(ImportEntry {
                row: identity.clone(),
                failure: Rc::downgrade(&failure.0),
                stopped: stopped.as_ref().map(Arc::downgrade),
                identity: NativeEngineEntryIdentity::capture(),
            })
        });
        Self {
            row: Some(row),
            identity,
            failure,
        }
    }
    pub(crate) fn finish(&mut self) {
        // Retain the independent failure owner/weak entry through the LAST
        // exact EnvCell destructor, which can itself enter inert CPP cleanup.
        if let Err(panic) = catch_unwind(AssertUnwindSafe(|| drop(self.row.take()))) {
            self.failure.record(CallbackFailure::Panic(panic));
        }
        let removed = IMPORTS.with(|entries| {
            let mut entries = entries.borrow_mut();
            entries
                .iter()
                .position(|entry| entry.row.ptr_eq(&self.identity))
                .map(|index| entries.remove(index))
        });
        if let Err(panic) = catch_unwind(AssertUnwindSafe(|| drop(removed))) {
            self.failure.record(CallbackFailure::Panic(panic));
        }
    }
}
impl Drop for ImportScope {
    fn drop(&mut self) {
        self.finish();
    }
}

pub(crate) fn replay_current() -> Result<(), RuntimeError> {
    match select() {
        Some(selected) => selected.failure.replay(),
        None => Ok(()),
    }
}
pub(crate) fn record_current_error(error: RuntimeError) {
    let raw = unsafe { crate::snapi::snapi_bridge_native_current_holder() };
    if !raw.is_null() {
        unsafe { &*raw.cast::<FrameHolder>() }.fail(CallbackFailure::Guest(error));
    } else if let Some(selected) = select() {
        selected.failure.record(CallbackFailure::Guest(error));
    }
}

pub(crate) fn current_failed() -> bool {
    select().is_some_and(|selected| selected.failure.failed())
}

// The physical holder owns disposal timing. Only the exact live row that
// requested deferral owns failure attribution; ambient drain-time work does not.
struct DeferredRetirement {
    plan: NativeRetirement,
    destination: Option<Weak<RefCell<FailureState>>>,
}

pub(crate) struct FrameHolder {
    invocation: Option<NativeEngineInvocation>,
    guard: Option<NativeEngineFrame>,
    pub(crate) environment: Option<Rc<FunctionEnvFramePin<NapiEnv>>>,
    failure: FirstFailure,
    stopped: Option<Arc<AtomicBool>>,
    cleanup: bool,
    deferred: RefCell<Vec<DeferredRetirement>>,
}
impl FrameHolder {
    pub(crate) fn failed(&self) -> bool {
        self.failure.failed()
    }
    pub(crate) fn fail(&self, failure: CallbackFailure) {
        self.failure.record(failure);
        // This is the only V8 control operation permitted inside an allocator.
        // CPP uses its cached initialized/live/non-disposing physical target.
        unsafe {
            crate::snapi::snapi_bridge_native_fail_current();
        }
    }
    fn finish(&mut self) {
        // A metadata-only holder underneath an allocator never accepts plans;
        // CPP routes those directly to the nearest depth-zero ancestor holder.
        let deferred = std::mem::take(self.deferred.get_mut());
        for retirement in deferred {
            finish_deferred_retirement(retirement);
        }
        if !self.cleanup
            && let Some(error) = self
                .guard
                .as_ref()
                .and_then(NativeEngineFrame::retirement_error)
        {
            self.failure.record(CallbackFailure::Guest(error));
        }
        if let Err(panic) = catch_unwind(AssertUnwindSafe(|| drop(self.guard.take()))) {
            self.failure.record(CallbackFailure::Panic(panic));
        }
        if let Err(panic) = catch_unwind(AssertUnwindSafe(|| drop(self.environment.take()))) {
            self.failure.record(CallbackFailure::Panic(panic));
        }
    }
}
impl Drop for FrameHolder {
    fn drop(&mut self) {
        self.finish();
    }
}

/// Returns the complete owned plan on refusal; None transfers it to the holder.
/// The physical outer header keeps this holder alive; no Store/T is accessed.
pub(crate) fn defer_retirement(plan: NativeRetirement) -> Option<NativeRetirement> {
    if unsafe { crate::snapi::snapi_v8_allocator_active() } == 0 {
        return Some(plan);
    }
    let raw = unsafe { crate::snapi::snapi_bridge_native_retirement_holder() };
    if raw.is_null() {
        return Some(plan);
    }
    // End selection (including its temporary exact environment pin) BEFORE
    // borrowing the Vec. Neither metadata lock spans a user destructor.
    let destination = select().map(|selected| Rc::downgrade(&selected.failure.0));
    let holder = unsafe { &*raw.cast::<FrameHolder>() };
    holder
        .deferred
        .borrow_mut()
        .push(DeferredRetirement { plan, destination });
    None
}

// All Rust ABI entries catch their complete owned operation, including SDK
// admission and disposal. No user payload is dropped under a TLS/RefCell borrow.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snapi_host_native_frame_enter(
    previous: *mut c_void,
    mode: i32,
    callback_enabled: *mut i32,
) -> *mut c_void {
    if !callback_enabled.is_null() {
        unsafe {
            *callback_enabled = 0;
        }
    }
    let selected = select();
    let previous = if previous.is_null() {
        None
    } else {
        Some(unsafe { &*previous.cast::<FrameHolder>() })
    };
    let failure = selected
        .as_ref()
        .map(|input| input.failure.clone())
        .or_else(|| previous.map(|previous| previous.failure.clone()))
        .unwrap_or_default();
    let outcome = catch_unwind(AssertUnwindSafe(|| -> Result<FrameHolder, RuntimeError> {
        let preceding = previous.and_then(|previous| previous.invocation.clone());
        let guard = match mode {
            0 => Some(unsafe { NativeEngineFrame::enter_managed(preceding.as_ref())? }),
            1 => Some(unsafe { NativeEngineFrame::enter_cleanup(preceding.as_ref())? }),
            2 => None,
            _ => return Err(RuntimeError::new("invalid native frame mode")),
        };
        let environment = if mode == 0 {
            selected
                .as_ref()
                .and_then(|input| input.row.as_ref())
                .and_then(|row| row.environment.clone())
        } else {
            None
        };
        Ok(FrameHolder {
            invocation: guard.as_ref().and_then(NativeEngineFrame::invocation),
            guard,
            environment,
            failure: failure.clone(),
            stopped: selected.as_ref().and_then(|input| input.stopped.clone()),
            cleanup: mode != 0,
            deferred: RefCell::new(Vec::new()),
        })
    }));
    let result = match outcome {
        Ok(Ok(holder)) => {
            if !callback_enabled.is_null() {
                unsafe {
                    *callback_enabled = i32::from(holder.environment.is_some());
                }
            }
            Box::into_raw(Box::new(holder)).cast()
        }
        Ok(Err(error)) => {
            failure.record(CallbackFailure::Guest(error));
            std::ptr::null_mut()
        }
        Err(panic) => {
            failure.record(CallbackFailure::Panic(panic));
            std::ptr::null_mut()
        }
    };
    discard(failure);
    result
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snapi_host_native_frame_exit(raw: *mut c_void) {
    if raw.is_null() {
        return;
    }
    let mut holder = unsafe { Box::from_raw(raw.cast::<FrameHolder>()) };
    let failure = holder.failure.clone();
    if let Err(panic) = catch_unwind(AssertUnwindSafe(|| holder.finish())) {
        failure.record(CallbackFailure::Panic(panic));
    }
    if let Err(panic) = catch_unwind(AssertUnwindSafe(|| drop(holder))) {
        failure.record(CallbackFailure::Panic(panic));
    }
    // The actual import/host envelope owns failure until the CPP call returns.
    // A callerless frame discards its captured failure; it never faults another root.
    discard(failure);
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snapi_host_native_frame_reject_entry(raw: *mut c_void) {
    unsafe {
        snapi_host_native_frame_exit(raw);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snapi_host_native_frame_host_stopped(raw: *mut c_void) -> i32 {
    if raw.is_null() {
        return 0;
    }
    let holder = unsafe { &*raw.cast::<FrameHolder>() };
    i32::from(
        holder
            .stopped
            .as_ref()
            .is_some_and(|stopped| stopped.load(Ordering::Acquire)),
    )
}

/// Host-only allocator/finalizer/nearheap work is synchronous even when the
/// physical coroutine otherwise permits ordinary managed guest suspension.
/// R is Copy: no generic result destructor can escape the protected scope.
pub(crate) fn synchronous<R: Copy>(
    body: impl FnOnce(&NativeSynchronousCallback) -> R,
) -> Option<R> {
    let unwinding = std::thread::panicking();
    let mut guard = None;
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        guard = Some(NativeSynchronousCallback::enter());
        let result = body(guard.as_ref().unwrap());
        if current_physical_failed() {
            unsafe {
                crate::snapi::snapi_bridge_native_fail_current();
            }
            return Ok(None);
        }
        if !unwinding
            && let Some(error) = guard
                .as_ref()
                .and_then(NativeSynchronousCallback::retirement_error)
        {
            return Err(CallbackFailure::Guest(error));
        }
        Ok(Some(result))
    }));
    let mut result = match outcome {
        Ok(Ok(result)) => result,
        Ok(Err(failure)) => {
            capture_failure(failure);
            None
        }
        Err(panic) => {
            capture_failure(CallbackFailure::Panic(panic));
            None
        }
    };
    // Recording/discarding a secondary payload happens while NoYield is still
    // active. Its destructor cannot park a new native invocation in an FFI.
    if let Err(panic) = catch_unwind(AssertUnwindSafe(|| drop(guard.take()))) {
        capture_failure(CallbackFailure::Panic(panic));
        result = None;
    }
    result
}

/// Own a failure slot for host disposal when no actual import already owns one.
/// No guest environment is exposed, and no unrelated current root is faulted.
pub(crate) fn host_cleanup(body: impl FnOnce()) {
    let unwinding = std::thread::panicking();
    let inherited = select().map(|selected| selected.failure);
    let failure = inherited.clone().unwrap_or_default();
    let mut scope = ImportScope::with_failure(None, None, failure.clone());
    if let Err(panic) = catch_unwind(AssertUnwindSafe(body)) {
        failure.record(CallbackFailure::Panic(panic));
    }
    scope.finish();
    if inherited.is_none() {
        match failure.take() {
            Some(CallbackFailure::Panic(panic)) if !unwinding => resume_unwind(panic),
            Some(CallbackFailure::Guest(error)) if !unwinding => {
                // A host-only cleanup frame cannot introduce a managed error;
                // preserve this invariant without converting it to a V8 call.
                resume_unwind(Box::new(error));
            }
            secondary => discard(secondary),
        }
    }
}

fn capture_failure(failure: CallbackFailure) {
    let raw = unsafe { crate::snapi::snapi_bridge_native_current_holder() };
    if raw.is_null() {
        discard(failure);
    } else {
        unsafe { &*raw.cast::<FrameHolder>() }.fail(failure);
    }
}

// Used for multiple owned cleanup phases inside one synchronous FFI body.
// Capture each phase before starting the next; never unwind through a second
// user destructor while the first captured panic is still active.
pub(crate) fn cleanup_phase(body: impl FnOnce()) {
    if let Err(panic) = catch_unwind(AssertUnwindSafe(body)) {
        capture_failure(CallbackFailure::Panic(panic));
    }
}

/// Admission checks use only this owned holder's first-failure metadata.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snapi_host_native_frame_failed(raw: *mut c_void) -> i32 {
    if raw.is_null() {
        return 1;
    }
    match catch_unwind(AssertUnwindSafe(|| {
        unsafe { &*raw.cast::<FrameHolder>() }.failed()
    })) {
        Ok(failed) => i32::from(failed),
        Err(panic) => {
            discard(panic);
            1
        }
    }
}

// A nested owned cleanup can record a failure without throwing through C++.
// Re-signal precisely this physical frame before its caller receives success.
pub(crate) fn current_physical_failed() -> bool {
    let raw = unsafe { crate::snapi::snapi_bridge_native_current_holder() };
    !raw.is_null() && unsafe { &*raw.cast::<FrameHolder>() }.failed()
}

/// Mark a vetoed operation in its own metadata header before input cleanup.
/// This hook neither enters the SDK nor destroys the caller-owned holder.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snapi_host_native_frame_reject_operation(raw: *mut c_void) {
    if raw.is_null() {
        return;
    }
    let holder = unsafe { &*raw.cast::<FrameHolder>() };
    if let Err(panic) = catch_unwind(AssertUnwindSafe(|| {
        holder
            .failure
            .record(CallbackFailure::Guest(RuntimeError::new(
                "V8 entry is forbidden from a native allocator callback",
            )));
    })) {
        discard(panic);
    }
}

fn finish_deferred_retirement(retirement: DeferredRetirement) {
    let destination = retirement
        .destination
        .and_then(|weak| weak.upgrade())
        .map(FirstFailure);
    let callerless = destination.is_none();
    let failure = destination.unwrap_or_default();
    // Force this captured destination through every provider/tracker/charge
    // cleanup phase. Never fall back to a different ambient row at drain time.
    let mut scope = ImportScope::with_failure(None, None, failure.clone());
    if let Err(panic) = catch_unwind(AssertUnwindSafe(|| drop(retirement.plan))) {
        failure.record(CallbackFailure::Panic(panic));
    }
    scope.finish();
    if callerless {
        // The initiating logical work has no live result envelope. Preserve
        // resource cleanup, catch payload Drop, and leave another root alone.
        discard(failure.take());
    }
}

/// Numeric physical-stack metadata only; it adds no engine or Store authority.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snapi_host_native_stack_bounds(low: *mut usize, base: *mut usize) -> i32 {
    if low.is_null() || base.is_null() {
        return 0;
    }
    unsafe {
        *low = 0;
        *base = 0;
    }
    match catch_unwind(AssertUnwindSafe(NativeEngineStackBounds::current)) {
        Ok(Some(bounds)) => {
            unsafe {
                *low = bounds.usable_low;
                *base = bounds.base;
            }
            1
        }
        Ok(None) => 0,
        Err(panic) => {
            discard(panic);
            0
        }
    }
}

/// A declined stack setup has an owned failure channel before V8 admission.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snapi_host_native_frame_reject_stack(raw: *mut c_void) {
    if raw.is_null() {
        return;
    }
    let holder = unsafe { &*raw.cast::<FrameHolder>() };
    if let Err(panic) = catch_unwind(AssertUnwindSafe(|| {
        holder
            .failure
            .record(CallbackFailure::Guest(RuntimeError::new(
                "native engine stack bounds are unavailable or exhausted",
            )));
    })) {
        discard(panic);
    }
}

// Numeric metadata only: caller CPP headers retain/block each selected ancestor.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snapi_host_native_paused_stack_top(
    low: usize,
    base: usize,
    top: *mut usize,
) -> i32 {
    if top.is_null() {
        return 0;
    }
    unsafe { top.write(0) };
    if (low == 0) != (base == 0) || (low != 0 && low >= base) {
        return 0;
    }
    let bounds = (low != 0).then_some(NativeEngineStackBounds {
        usable_low: low,
        base,
    });
    match catch_unwind(AssertUnwindSafe(|| {
        NativeEngineStackBounds::paused_top(bounds)
    })) {
        Ok(Some(value)) => {
            unsafe { top.write(value) };
            1
        }
        Ok(None) => 0,
        Err(_) => std::process::abort(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn import_row_removal_preserves_a_later_exact_entry() {
        assert!(select().is_none());
        let mut first = ImportScope::enter(None, None);
        let mut second = ImportScope::enter(None, None);
        let current = select().unwrap().failure;
        let is_second = Rc::ptr_eq(&current.0, &second.failure.0);
        first.finish();
        let after_first = select().unwrap().failure;
        let still_second = Rc::ptr_eq(&after_first.0, &second.failure.0);
        second.finish();
        assert!(select().is_none());
        assert!(is_second && still_second);
    }

    #[test]
    fn replayed_panic_keeps_first_failure_through_secondary_cleanup() {
        struct Cleanup<'a>(&'a FirstFailure);
        impl Drop for Cleanup<'_> {
            fn drop(&mut self) {
                self.0
                    .record(CallbackFailure::Panic(Box::new("secondary cleanup")));
            }
        }
        let original = Arc::new(());
        let failure = FirstFailure::default();
        failure.record(CallbackFailure::Panic(Box::new(original.clone())));
        let outcome = catch_unwind(AssertUnwindSafe(|| {
            let _cleanup = Cleanup(&failure);
            failure.replay().unwrap();
        }));
        failure.body_finished(outcome.err().map(CallbackFailure::Panic));
        let first = failure.take().unwrap();
        let CallbackFailure::Panic(panic) = first else {
            panic!("panic was replaced");
        };
        assert!(Arc::ptr_eq(
            panic.downcast_ref::<Arc<()>>().unwrap(),
            &original
        ));
    }
}
