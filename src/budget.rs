//! Cross-VM resource accounting for the N-API (`imports`) provider.
//!
//! An edgejs guest running under the `imports` provider splits execution and
//! allocation across two VMs: the wasmer store (guest wasm linear memory) and
//! host-native V8. To keep an embedder-imposed memory budget honest across
//! *both*, every pool charges against one shared [`ResourceBudget`] per app.
//!
//! This module implements **Phase 1** of that design: the accountant itself
//! plus exact, deterministic charging of **guest wasm linear memory**. Later
//! phases charge the V8 heap, external memory, and host transients against the
//! same budget, and add CPU metering; the [`Pool`] enum and the budget API are
//! the insertion points for them.
//!
//! ## Native sys install path
//!
//! The guest's linear memory is *imported* (host-created). wasmer's public API
//! does not let an embedder inject a custom [`LinearMemory`] into a
//! [`wasmer::Memory`] directly (the backend `VMMemory` enum is private), so the
//! charge is installed one level down, via custom [`Tunables`]: a
//! [`BudgetedTunables`] wraps [`BaseTunables`] and returns every host memory
//! wrapped in a [`BudgetedMemory`]. Installing those tunables on the engine
//! (see `cli.rs`) makes `Memory::new` — and therefore the guest's imported
//! memory — budget-aware with no change to the memory-creation call site.
//!
//! This tunables path exists only for Wasmer's native `sys` backend. A
//! `wasm32` host-JavaScript build uses the host's WebAssembly memory and does
//! not compile or install [`BudgetedTunables`]. It still uses
//! [`ResourceBudget`] for provider-owned resources such as environments,
//! value handles, and declared external memory.

use std::ffi::c_void;
use std::sync::{
    Arc,
    atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
};
#[cfg(not(target_arch = "wasm32"))]
use std::time::Duration;

#[cfg(not(target_arch = "wasm32"))]
use wasmer::sys::vm::{
    ExpectedValue, LinearMemory, MemoryError, ThreadConditions, VMMemory, VMMemoryDefinition,
    VMSharedMemory, VMTable, VMTableDefinition, WaiterError,
};
#[cfg(not(target_arch = "wasm32"))]
use wasmer::sys::{BaseTunables, Tunables};
#[cfg(not(target_arch = "wasm32"))]
use wasmer::{MemoryStyle, MemoryType, Pages, TableStyle, TableType, WASM_PAGE_SIZE};

/// Sentinel meaning "no memory limit". A budget built with this total tracks
/// charges for observability but never denies one.
const UNLIMITED: u64 = u64::MAX;

const MIB: u64 = 1024 * 1024;

/// Initial `max_old_generation_size` charged per V8 isolate at env creation.
pub const DEFAULT_INITIAL_ISOLATE_HEAP: u64 = 64 * MIB;
/// Increment the near-heap-limit callback reserves per grow grant.
pub const DEFAULT_HEAP_GROW_STEP: u64 = 32 * MIB;
/// One-time V8 heap slack reserved up front (and charged to the budget) so a
/// denied growth callback can terminate and unwind without entering V8's fatal
/// out-of-memory path.
pub const DEFAULT_UNWIND_SLACK: u64 = 16 * MIB;
/// Heap headroom, beyond the budget, that a refused growth exposes to V8 once
/// per isolate, together with [`DEFAULT_UNWIND_SLACK`], while the isolate is
/// terminated.
///
/// V8 asks the near-heap-limit callback once before the last-resort collection
/// of a failed allocation and retries the allocation against the answer; a
/// retry that still does not fit is `FatalProcessOutOfMemory`, which aborts
/// the whole process. The headroom therefore covers the largest single
/// allocation JavaScript can request: V8 caps every heap object at 1 GiB
/// (`FixedArray` and `FixedDoubleArray` at 128 Mi entries, strings at
/// `String::kMaxLength` two-byte characters), plus large-object page headers
/// and the small allocations a builtin makes before it sees the termination.
/// Raising the limit commits no memory: what the isolate takes from the
/// headroom is what it allocates before termination lands, and it is freed
/// with the isolate.
pub const DEFAULT_HEAP_EMERGENCY_HEADROOM: u64 = 1088 * MIB;

/// Isolates that were granted emergency heap headroom, process-wide.
static HEAP_EMERGENCY_GRANTS: AtomicU64 = AtomicU64::new(0);
/// Near-heap-limit callbacks that found the emergency headroom already spent,
/// process-wide. Each means a stopping isolate asked for more heap than the
/// headroom before its termination landed; V8 aborts the process if its retry
/// then fails, so a non-zero value is an incident, not a metric.
static HEAP_EMERGENCY_EXHAUSTED: AtomicU64 = AtomicU64::new(0);

/// Process-wide counters of the emergency heap headroom.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct HeapEmergencyStats {
    /// Isolates that were stopped and granted emergency headroom.
    pub grants: u64,
    /// Callbacks that could not raise the limit further. V8 may have aborted
    /// the process after any of them.
    pub exhausted: u64,
}

/// Process-wide counters of the emergency heap headroom, for the embedder's
/// metrics and alerts.
pub fn heap_emergency_stats() -> HeapEmergencyStats {
    HeapEmergencyStats {
        grants: HEAP_EMERGENCY_GRANTS.load(Ordering::Acquire),
        exhausted: HEAP_EMERGENCY_EXHAUSTED.load(Ordering::Acquire),
    }
}
/// Fixed per-isolate overhead charged to cover young generation, code range,
/// and V8's own malloc'd metadata without sampling.
pub const DEFAULT_PER_ISOLATE_OVERHEAD: u64 = 8 * MIB;

/// Estimated host-side bytes held per N-API value handle crossed to the guest:
/// the `napi_ref` struct, the handle-map node, and a V8 global-handle slot. The
/// referenced JS value itself lives in the (separately charged) V8 heap. The
/// bridge charges each entry of its handle tables at this estimate (see
/// [`Pool::HostBookkeeping`]).
pub const EST_HOST_BYTES_PER_VALUE: u64 = 128;

/// Granularity of [`Pool::HostBookkeeping`] charges. The bridge asks for
/// another chunk when its live estimate outgrows what was granted, and returns
/// whole chunks once at least two are unused.
pub const HOST_BOOKKEEPING_CHUNK: u64 = 256 * 1024;

/// Host copies of guest memory below this size are not charged to
/// [`Pool::HostTransient`]: an import holds a bounded number of them, and they
/// are freed before it returns. Larger copies, and every name an import keeps
/// in an array (property descriptors), are charged.
pub const UNCHARGED_HOST_COPY_BYTES: usize = 16 * 1024;

#[cfg(not(target_arch = "wasm32"))]
fn pages_to_bytes(pages: Pages) -> u64 {
    u64::from(pages.0) * WASM_PAGE_SIZE as u64
}

/// Round `bytes` up to a whole number of wasm pages, saturating.
#[cfg(not(target_arch = "wasm32"))]
fn round_up_to_page(bytes: u64) -> u64 {
    let page = WASM_PAGE_SIZE as u64;
    bytes.div_ceil(page).saturating_mul(page)
}

/// A distinct byte pool metered against the budget.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum Pool {
    /// Guest wasm linear memory (wasmer `WasmMmap`).
    WasmLinear,
    /// V8 per-isolate heap *ceiling* (old + young + code range + per-isolate
    /// overhead + pre-reserved unwind slack), charged by reservation at env
    /// creation and raised in grow-steps by the near-heap-limit callback. Charged by ceiling, not
    /// live usage, so the guarantee never races V8's GC. A refused grow step
    /// stops the isolate and exposes [`DEFAULT_HEAP_EMERGENCY_HEADROOM`]
    /// outside the budget (see [`ResourceUsage::v8_heap_emergency`]).
    V8HeapReserved,
    /// V8 external memory the guest has explicitly declared via
    /// `napi_adjust_external_memory` (`NapiEnv::charge_declared_external`).
    /// ArrayBuffer/Buffer backing stores are NOT charged here: GuestHeap is
    /// the only allocation path for them and they're charged as
    /// [`Pool::WasmLinear`] instead — see [`crate::guest_heap::GuestHeap`].
    V8External,
    /// Host memory the native bridge keeps for the guest's handles: value
    /// slots, references, deferreds, callback registrations and the opaque
    /// handle tables. Estimated per entry ([`EST_HOST_BYTES_PER_VALUE`]) and
    /// granted in [`HOST_BOOKKEEPING_CHUNK`]s, so most handles charge nothing.
    HostBookkeeping,
    /// Host copies of guest memory an import makes for the duration of the
    /// call: strings, names, property descriptors, argument arrays and other
    /// guest-supplied byte ranges. Copies smaller than
    /// [`UNCHARGED_HOST_COPY_BYTES`] are not charged.
    HostTransient,
    /// Serialized messages between the context's environments (`postMessage`
    /// payloads), from creation until they are taken, dropped, or the
    /// context stops.
    SerializedMessage,
}

/// Embedder-owned aggregate accounting for byte reservations made by N-API.
///
/// N-API retains its own per-pool counters; the embedder sees only byte totals,
/// keeping pool policy and future N-API implementation details out of Edge.
///
/// A refused [`try_charge`](Self::try_charge) means the application is out of
/// memory: N-API fails the operation, and the embedder may stop the
/// application (for example through [`crate::NapiRuntimeControl::terminate_all`]).
/// A refused [`try_charge_soft`](Self::try_charge_soft) is reported to the
/// application as an ordinary JavaScript error, and it keeps running.
pub trait NapiMemoryAccountant: Send + Sync {
    fn memory_limit(&self) -> u64;
    fn memory_charged(&self) -> u64;
    fn try_charge(&self, bytes: u64) -> bool;
    /// Charges bytes whose refusal N-API reports to the application as a
    /// catchable JavaScript error (for example a `postMessage` payload that
    /// does not fit). It must refuse without side effects such as stopping
    /// the application. Embedders whose [`try_charge`](Self::try_charge)
    /// stops the application on refusal override it; the default is
    /// `try_charge`.
    fn try_charge_soft(&self, bytes: u64) -> bool {
        self.try_charge(bytes)
    }
    fn uncharge(&self, bytes: u64);
}

/// Error returned by [`ResourceBudget::try_charge`] when a charge would push
/// total live bytes past the app's memory budget.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OverBudget {
    /// Pool the rejected charge targeted.
    pub pool: Pool,
    /// Bytes the caller tried to charge.
    pub requested: u64,
    /// Bytes already charged when the request was rejected.
    pub charged: u64,
    /// The app's total memory budget.
    pub total: u64,
}

impl std::fmt::Display for OverBudget {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "resource budget exceeded charging {} bytes to {:?}: {} of {} bytes already charged",
            self.requested, self.pool, self.charged, self.total
        )
    }
}

impl std::error::Error for OverBudget {}

/// A point-in-time view of a budget's charges, for metrics / billing.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ResourceUsage {
    /// The app's total memory budget (`u64::MAX` if unlimited).
    pub mem_total: u64,
    /// Aggregate bytes currently charged. With an embedder accountant this also
    /// includes non-N-API memory sharing the same application budget.
    pub mem_charged: u64,
    /// Currently-charged guest wasm linear memory bytes.
    pub wasm_linear: u64,
    /// Currently-reserved V8 per-isolate heap ceiling bytes.
    pub v8_heap_reserved: u64,
    /// Currently-charged V8 external memory (ArrayBuffer/Buffer) bytes.
    pub v8_external: u64,
    /// Host bookkeeping granted to the bridge ([`Pool::HostBookkeeping`]).
    pub host_bookkeeping: u64,
    /// Charged host copies of guest memory ([`Pool::HostTransient`]).
    pub host_transient: u64,
    /// Charged serialized messages ([`Pool::SerializedMessage`]).
    pub serialized_message: u64,
    /// V8 heap headroom currently exposed *outside* the budget to isolates
    /// whose growth was refused and that are being terminated (see
    /// [`DEFAULT_HEAP_EMERGENCY_HEADROOM`]). Not part of `mem_charged`.
    pub v8_heap_emergency: u64,
    /// V8 heap headroom currently lent, uncharged, to isolates for an
    /// allocation that may be pending, until the isolate's next interrupt
    /// check (see [`napi_host_near_heap_limit_grant`]). Not part of
    /// `mem_charged`.
    pub v8_heap_lease: u64,
    /// Number of live V8 isolates (envs) counted against `max_envs`.
    pub live_isolates: usize,
}

/// The (possibly clamped) heap constraints a guest requested for a new V8 env.
/// Fields are bytes; `0` means "unset" (V8 picks its default).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RequestedHeap {
    pub max_young: u32,
    pub max_old: u32,
    pub code_range: u32,
}

/// The outcome of reserving budget for a new V8 env: the constraints to forward
/// to V8 (clamped to fit the budget) and the ceiling charged for them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HeapReservation {
    pub max_young: u32,
    pub max_old: u32,
    pub code_range: u32,
    /// Bytes charged to [`Pool::V8HeapReserved`]; `0` under an unlimited budget.
    pub ceiling_bytes: u64,
    /// Whether budget clamping was applied (i.e. constraints must be forwarded
    /// to V8 even if the guest requested none).
    pub clamped: bool,
}

/// Why a new V8 env was refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EnvRejected {
    /// The app is already at its `max_envs` isolate cap.
    TooManyEnvs,
    /// The minimum viable heap ceiling does not fit the remaining budget.
    HeapDoesNotFit,
}

/// One shared accountant per app, `Arc`-shared into every pool that allocates.
///
/// Charging is **reserve-based**: bytes are charged before (or exactly at) the
/// moment they become live, so `actual usage <= mem_charged <= mem_total`
/// always holds and enforcement never races a garbage collector. All state is
/// atomic, so worker threads (each its own store + isolate) share one budget.
pub struct ResourceBudget {
    /// Total byte budget. `UNLIMITED` disables enforcement (tracking only).
    mem_total: u64,
    /// Sum of all currently-charged bytes across every pool.
    mem_charged: AtomicU64,
    accountant: Option<Arc<dyn NapiMemoryAccountant>>,
    /// Per-pool charge, for observability and reconciliation.
    wasm_linear: AtomicU64,
    v8_heap_reserved: AtomicU64,
    v8_external: AtomicU64,
    host_bookkeeping: AtomicU64,
    host_transient: AtomicU64,
    serialized_message: AtomicU64,
    /// Heap headroom a refused grow step exposes per isolate.
    heap_emergency_headroom: AtomicU64,
    /// Emergency headroom currently exposed, over all stopping isolates. Not
    /// a charge: the budget is exhausted when it is used.
    v8_heap_emergency: AtomicU64,
    /// Headroom currently lent to isolates for a pending allocation.
    v8_heap_lease: AtomicU64,
    /// Live V8 isolates (envs), counted against `max_envs`.
    live_isolates: AtomicUsize,
}

impl std::fmt::Debug for ResourceBudget {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ResourceBudget")
            .field("mem_total", &self.memory_limit())
            .field("mem_charged", &self.memory_charged())
            .field("wasm_linear", &self.wasm_linear.load(Ordering::Acquire))
            .field(
                "v8_heap_reserved",
                &self.v8_heap_reserved.load(Ordering::Acquire),
            )
            .field("v8_external", &self.v8_external.load(Ordering::Acquire))
            .field(
                "host_bookkeeping",
                &self.host_bookkeeping.load(Ordering::Acquire),
            )
            .field(
                "host_transient",
                &self.host_transient.load(Ordering::Acquire),
            )
            .field(
                "serialized_message",
                &self.serialized_message.load(Ordering::Acquire),
            )
            .field("live_isolates", &self.live_isolates.load(Ordering::Acquire))
            .finish()
    }
}

impl ResourceBudget {
    /// A budget that tracks charges but never denies one.
    pub fn unlimited() -> Arc<Self> {
        Arc::new(Self::with_total(UNLIMITED))
    }

    /// A budget that denies any charge pushing live bytes past `bytes`.
    pub fn with_memory_limit(bytes: u64) -> Arc<Self> {
        Arc::new(Self::with_total(bytes))
    }

    /// Use an embedder-owned total while retaining N-API per-pool policy.
    /// Whether an embedder owns wasm linear-memory accounting.
    ///
    /// The guest heap grows the guest's memory through the store, so an
    /// embedder that meters growth through its own tunables has already
    /// charged those bytes by the time we see them; charging again would
    /// count one allocation twice against the same total. With no accountant
    /// nothing else is counting, so the heap charges its own claims.
    pub(crate) fn wasm_is_externally_accounted(&self) -> bool {
        self.accountant.is_some()
    }

    pub fn with_accountant(accountant: Arc<dyn NapiMemoryAccountant>) -> Arc<Self> {
        Arc::new(Self {
            mem_total: accountant.memory_limit(),
            mem_charged: AtomicU64::new(0),
            accountant: Some(accountant),
            wasm_linear: AtomicU64::new(0),
            v8_heap_reserved: AtomicU64::new(0),
            v8_external: AtomicU64::new(0),
            host_bookkeeping: AtomicU64::new(0),
            host_transient: AtomicU64::new(0),
            serialized_message: AtomicU64::new(0),
            heap_emergency_headroom: AtomicU64::new(DEFAULT_HEAP_EMERGENCY_HEADROOM),
            v8_heap_emergency: AtomicU64::new(0),
            v8_heap_lease: AtomicU64::new(0),
            live_isolates: AtomicUsize::new(0),
        })
    }

    fn with_total(mem_total: u64) -> Self {
        Self {
            mem_total,
            mem_charged: AtomicU64::new(0),
            accountant: None,
            wasm_linear: AtomicU64::new(0),
            v8_heap_reserved: AtomicU64::new(0),
            v8_external: AtomicU64::new(0),
            host_bookkeeping: AtomicU64::new(0),
            host_transient: AtomicU64::new(0),
            serialized_message: AtomicU64::new(0),
            heap_emergency_headroom: AtomicU64::new(DEFAULT_HEAP_EMERGENCY_HEADROOM),
            v8_heap_emergency: AtomicU64::new(0),
            v8_heap_lease: AtomicU64::new(0),
            live_isolates: AtomicUsize::new(0),
        }
    }

    /// Whether this budget enforces a limit.
    pub fn is_unlimited(&self) -> bool {
        self.memory_limit() == UNLIMITED
    }

    /// The total memory budget (`u64::MAX` if unlimited).
    pub fn memory_limit(&self) -> u64 {
        self.accountant
            .as_ref()
            .map_or(self.mem_total, |x| x.memory_limit())
    }

    /// Bytes currently charged across all pools.
    pub fn memory_charged(&self) -> u64 {
        self.accountant.as_ref().map_or_else(
            || self.mem_charged.load(Ordering::Acquire),
            |x| x.memory_charged(),
        )
    }

    /// Bytes still available before the budget is exhausted.
    pub fn memory_remaining(&self) -> u64 {
        self.memory_limit().saturating_sub(self.memory_charged())
    }

    /// Atomically charge `bytes` against `pool`; `Err` if it would exceed the
    /// total. On success the caller owns the charge and must [`uncharge`] it
    /// (directly or via an owner whose `Drop` does).
    ///
    /// [`uncharge`]: ResourceBudget::uncharge
    pub fn try_charge(&self, pool: Pool, bytes: u64) -> Result<(), OverBudget> {
        self.charge(pool, bytes, false)
    }

    /// Like [`try_charge`](Self::try_charge), for bytes whose refusal the
    /// application sees as a catchable JavaScript error: an embedder
    /// accountant is asked through [`NapiMemoryAccountant::try_charge_soft`],
    /// so a refusal does not stop the application.
    pub fn try_charge_soft(&self, pool: Pool, bytes: u64) -> Result<(), OverBudget> {
        self.charge(pool, bytes, true)
    }

    fn charge(&self, pool: Pool, bytes: u64, soft: bool) -> Result<(), OverBudget> {
        if bytes == 0 {
            return Ok(());
        }

        if let Some(accountant) = &self.accountant {
            let granted = if soft {
                accountant.try_charge_soft(bytes)
            } else {
                accountant.try_charge(bytes)
            };
            if !granted {
                return Err(OverBudget {
                    pool,
                    requested: bytes,
                    charged: accountant.memory_charged(),
                    total: accountant.memory_limit(),
                });
            }
            self.mem_charged.fetch_add(bytes, Ordering::AcqRel);
        } else if self.mem_total == UNLIMITED {
            self.mem_charged.fetch_add(bytes, Ordering::AcqRel);
        } else {
            // CAS loop so a concurrent charge can never let the sum slip past
            // the total between the check and the commit.
            let mut current = self.mem_charged.load(Ordering::Acquire);
            loop {
                let next = current
                    .checked_add(bytes)
                    .filter(|next| *next <= self.mem_total);
                let Some(next) = next else {
                    return Err(OverBudget {
                        pool,
                        requested: bytes,
                        charged: current,
                        total: self.mem_total,
                    });
                };
                match self.mem_charged.compare_exchange_weak(
                    current,
                    next,
                    Ordering::AcqRel,
                    Ordering::Acquire,
                ) {
                    Ok(_) => break,
                    Err(observed) => current = observed,
                }
            }
        }

        self.pool_counter(pool).fetch_add(bytes, Ordering::AcqRel);
        Ok(())
    }

    /// Release a previously-charged `bytes` from `pool`.
    pub fn uncharge(&self, pool: Pool, bytes: u64) {
        if bytes == 0 {
            return;
        }
        self.pool_counter(pool).fetch_sub(bytes, Ordering::AcqRel);
        self.mem_charged.fetch_sub(bytes, Ordering::AcqRel);
        if let Some(accountant) = &self.accountant {
            accountant.uncharge(bytes);
        }
    }

    fn pool_counter(&self, pool: Pool) -> &AtomicU64 {
        match pool {
            Pool::WasmLinear => &self.wasm_linear,
            Pool::V8HeapReserved => &self.v8_heap_reserved,
            Pool::V8External => &self.v8_external,
            Pool::HostBookkeeping => &self.host_bookkeeping,
            Pool::HostTransient => &self.host_transient,
            Pool::SerializedMessage => &self.serialized_message,
        }
    }

    /// Snapshot the current charges for metrics / billing.
    pub fn snapshot(&self) -> ResourceUsage {
        ResourceUsage {
            mem_total: self.memory_limit(),
            mem_charged: self.memory_charged(),
            wasm_linear: self.wasm_linear.load(Ordering::Acquire),
            v8_heap_reserved: self.v8_heap_reserved.load(Ordering::Acquire),
            v8_external: self.v8_external.load(Ordering::Acquire),
            host_bookkeeping: self.host_bookkeeping.load(Ordering::Acquire),
            host_transient: self.host_transient.load(Ordering::Acquire),
            serialized_message: self.serialized_message.load(Ordering::Acquire),
            v8_heap_emergency: self.v8_heap_emergency.load(Ordering::Acquire),
            v8_heap_lease: self.v8_heap_lease.load(Ordering::Acquire),
            live_isolates: self.live_isolates.load(Ordering::Acquire),
        }
    }

    /// Number of live V8 isolates counted against `max_envs`.
    pub fn live_isolates(&self) -> usize {
        self.live_isolates.load(Ordering::Acquire)
    }

    /// Heap headroom exposed to an isolate whose growth this budget refused
    /// (see [`DEFAULT_HEAP_EMERGENCY_HEADROOM`]).
    pub fn heap_emergency_headroom(&self) -> u64 {
        self.heap_emergency_headroom.load(Ordering::Acquire)
    }

    /// Overrides the emergency headroom for refusals after this call.
    ///
    /// Less than [`DEFAULT_HEAP_EMERGENCY_HEADROOM`] lets a single allocation
    /// larger than the value abort the process again; it is for a host that
    /// runs few, large isolates and prefers a smaller transient overshoot.
    pub fn set_heap_emergency_headroom(&self, bytes: u64) {
        self.heap_emergency_headroom.store(bytes, Ordering::Release);
    }

    /// Records headroom exposed to a stopping isolate.
    fn expose_heap_emergency(&self, bytes: u64) {
        self.v8_heap_emergency.fetch_add(bytes, Ordering::AcqRel);
        HEAP_EMERGENCY_GRANTS.fetch_add(1, Ordering::AcqRel);
    }

    /// The isolate that was exposed `bytes` of emergency headroom is gone.
    pub(crate) fn release_heap_emergency(&self, bytes: u64) {
        if bytes != 0 {
            self.v8_heap_emergency.fetch_sub(bytes, Ordering::AcqRel);
        }
    }

    /// Reserve budget for a new V8 env: acquire an isolate slot against
    /// `max_envs` and charge the (clamped) heap ceiling. On success the caller
    /// owns both and must release them with [`release_env`] exactly once — on
    /// env teardown, or immediately if env creation then fails.
    ///
    /// [`release_env`]: ResourceBudget::release_env
    pub fn try_reserve_env(
        &self,
        req: RequestedHeap,
        max_envs: Option<usize>,
    ) -> Result<HeapReservation, EnvRejected> {
        if !self.try_acquire_isolate(max_envs) {
            return Err(EnvRejected::TooManyEnvs);
        }
        let Some(reservation) = self.plan_heap_reservation(req) else {
            self.release_isolate();
            return Err(EnvRejected::HeapDoesNotFit);
        };
        if self
            .try_charge(Pool::V8HeapReserved, reservation.ceiling_bytes)
            .is_err()
        {
            self.release_isolate();
            return Err(EnvRejected::HeapDoesNotFit);
        }
        Ok(reservation)
    }

    /// Release an env reservation: uncharge its heap ceiling and free its
    /// isolate slot. Pair with exactly one successful [`try_reserve_env`].
    ///
    /// [`try_reserve_env`]: ResourceBudget::try_reserve_env
    pub fn release_env(&self, ceiling_bytes: u64) {
        self.uncharge(Pool::V8HeapReserved, ceiling_bytes);
        self.release_isolate();
    }

    /// Atomically claim an isolate slot if under `max_envs` (always succeeds
    /// when `max_envs` is `None`).
    fn try_acquire_isolate(&self, max_envs: Option<usize>) -> bool {
        let Some(max) = max_envs else {
            self.live_isolates.fetch_add(1, Ordering::AcqRel);
            return true;
        };
        let mut current = self.live_isolates.load(Ordering::Acquire);
        loop {
            if current >= max {
                return false;
            }
            match self.live_isolates.compare_exchange_weak(
                current,
                current + 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return true,
                Err(observed) => current = observed,
            }
        }
    }

    fn release_isolate(&self) {
        self.live_isolates.fetch_sub(1, Ordering::AcqRel);
    }

    /// Clamp requested heap constraints to fit the remaining budget, applying
    /// the default old-generation ceiling and per-isolate overhead. Returns
    /// `None` if not even the minimum viable ceiling fits. Under an unlimited
    /// budget the request passes through unchanged and uncharged.
    fn plan_heap_reservation(&self, req: RequestedHeap) -> Option<HeapReservation> {
        if self.is_unlimited() {
            return Some(HeapReservation {
                max_young: req.max_young,
                max_old: req.max_old,
                code_range: req.code_range,
                ceiling_bytes: 0,
                clamped: false,
            });
        }

        // Explicit young/code plus a fixed overhead are reserved first; the
        // overhead also covers V8's own metadata and any unset young/code.
        let young = u64::from(req.max_young);
        let code = u64::from(req.code_range);
        let fixed = DEFAULT_PER_ISOLATE_OVERHEAD
            .checked_add(DEFAULT_UNWIND_SLACK)?
            .checked_add(young)?
            .checked_add(code)?;

        let remaining = self.memory_remaining();
        if remaining <= fixed {
            return None;
        }

        let old_available = remaining - fixed;
        let requested_old = if req.max_old > 0 {
            u64::from(req.max_old)
        } else {
            DEFAULT_INITIAL_ISOLATE_HEAP
        };
        let old = requested_old.min(old_available);
        if old == 0 {
            return None;
        }

        Some(HeapReservation {
            max_young: req.max_young,
            max_old: u32::try_from(old).unwrap_or(u32::MAX),
            code_range: req.code_range,
            ceiling_bytes: fixed + old,
            clamped: true,
        })
    }
}

/// Per-V8-env heap-growth tracker shared with the host-owned near-heap-limit
/// callback.
///
/// Boxed at env creation and handed to the bridge as an opaque pointer; the
/// owning [`crate::env::NapiEnv`] reclaims it at env teardown, after the
/// isolate is gone, to release the bytes granted beyond the initial ceiling,
/// any lease and any emergency headroom.
pub(crate) struct EnvHeapCharge {
    pub(crate) budget: Arc<ResourceBudget>,
    pub(crate) env: usize,
    pub(crate) host_stopped: Arc<AtomicBool>,
    /// Heap exposed outside the budget by the first refused grow step (the
    /// pre-charged unwind slack plus the emergency headroom); `0` until then.
    /// Set once, so a stopping isolate cannot expand its limit repeatedly.
    pub(crate) emergency_exposed: AtomicU64,
    /// Bytes granted beyond the initial ceiling (grow steps and reconciles).
    pub(crate) granted: AtomicU64,
    /// The old-generation limit the budget paid for: the ceiling forwarded at
    /// creation plus `granted`.
    pub(crate) charged_limit: AtomicU64,
    /// The isolate's old-generation limit as last answered or set: what V8's
    /// post-collection check compares the committed old generation with.
    /// Above `charged_limit` by `lease` while a lease is out.
    pub(crate) old_limit: AtomicU64,
    /// Headroom lent to V8, uncharged, for an allocation that may be pending
    /// (see [`napi_host_near_heap_limit_grant`]); `0` when none is out.
    pub(crate) lease: AtomicU64,
}

impl EnvHeapCharge {
    pub(crate) fn new(
        budget: Arc<ResourceBudget>,
        env: usize,
        host_stopped: Arc<AtomicBool>,
        old_limit: u64,
    ) -> Self {
        Self {
            budget,
            env,
            host_stopped,
            emergency_exposed: AtomicU64::new(0),
            granted: AtomicU64::new(0),
            charged_limit: AtomicU64::new(old_limit),
            old_limit: AtomicU64::new(old_limit),
            lease: AtomicU64::new(0),
        }
    }

    /// Stops the isolate's application for good: the budget refused its
    /// heap. Sticky, so the env's release runs no guest code either.
    fn deny(&self) {
        self.host_stopped.store(true, Ordering::Release);
        if self.env != 0 {
            unsafe {
                crate::snapi::snapi_bridge_unofficial_stop_execution(
                    self.env as crate::snapi::SnapiEnv,
                );
            }
        }
    }

    /// Sets the outstanding lease, keeping the budget's lease counter exact.
    fn set_lease(&self, bytes: u64) {
        let previous = self.lease.swap(bytes, Ordering::AcqRel);
        if bytes > previous {
            self.budget
                .v8_heap_lease
                .fetch_add(bytes - previous, Ordering::AcqRel);
        } else if previous > bytes {
            self.budget
                .v8_heap_lease
                .fetch_sub(previous - bytes, Ordering::AcqRel);
        }
    }

    /// Exposes, once, the unwind slack and the emergency headroom (or `need`
    /// past the current limit, if larger) to a stopping isolate. The new
    /// limit, or `None` if they were exposed before.
    fn expose_emergency(&self, current_limit: usize, need: u64) -> Option<usize> {
        let exposed =
            DEFAULT_UNWIND_SLACK.saturating_add(self.budget.heap_emergency_headroom().max(need));
        self.emergency_exposed
            .compare_exchange(0, exposed, Ordering::AcqRel, Ordering::Acquire)
            .ok()?;
        self.budget.expose_heap_emergency(exposed);
        Some(current_limit.saturating_add(usize::try_from(exposed).unwrap_or(usize::MAX)))
    }

    /// Returns what the tracker still holds outside the grants: the lease
    /// and the emergency exposure. At env teardown.
    pub(crate) fn release_exposure(&self) {
        self.set_lease(0);
        self.budget
            .release_heap_emergency(self.emergency_exposed.load(Ordering::Acquire));
    }
}

/// Host-owned near-heap-limit callback for a budgeted V8 isolate.
///
/// V8 calls it on the isolate's thread when the heap nears its limit, and
/// takes one answer per occasion: if the answer is too small V8 aborts the
/// whole process. Three cases matter (`Heap::CollectAllAvailableGarbage`,
/// `Heap::CheckHeapLimitReached`, `HeapAllocator::RetryCustomAllocateOrFail`
/// in V8 15.9):
///
/// * After a collection the old generation is over the limit
///   (`committed_old_generation > current_limit`): V8 aborts unless the
///   answer covers it. The old generation can get there with no earlier
///   callback, because `NewLargeObjectSpace::AllocateRaw` admits the first
///   large young object regardless of the limit (`new Array(3e7)` is one
///   240 MB object) and the next full collection promotes it. The callback
///   charges the overshoot past what the budget paid for, plus one
///   [`DEFAULT_HEAP_GROW_STEP`].
/// * Before the last-resort collection of a failed allocation, or after
///   ineffective mark-compacts, the old generation is within the limit: an
///   allocation of unknown size (up to one maximal object, 1 GiB) is pending,
///   and V8 retries it against the answer once, with no further callback, and
///   aborts if it does not fit. A paid step alone may not cover it, so the
///   answer also *lends* headroom for one maximal object, uncharged
///   ([`ResourceUsage::v8_heap_lease`]). `reconcile_out` then asks the bridge
///   for an interrupt, which runs after the allocation: it charges what the
///   heap really uses and lowers V8's limit back to what the budget paid for
///   ([`napi_host_heap_reconcile`]). The next return from JavaScript does the
///   same if the interrupt did not run.
/// * If the budget refuses, the callback stops the application (sticky) and
///   exposes, once, the pre-charged unwind slack plus the budget's emergency
///   headroom ([`DEFAULT_HEAP_EMERGENCY_HEADROOM`], or the overshoot if
///   larger), so the failed allocation completes while termination lands.
///   After that it can only nudge the limit by a page, which keeps V8's
///   ineffective-mark-compact check from aborting.
///
/// The budget is atomic, so this is safe concurrently with charges on other
/// threads.
///
/// # Safety
/// `data` must be null or a pointer to an [`EnvHeapCharge`] that outlives the
/// call, and `reconcile_out` null or writable. The tracker is freed only
/// after the env's isolate is disposed, so a non-null pointer is valid for
/// every real invocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn napi_host_near_heap_limit_grant(
    data: *const c_void,
    current_limit: usize,
    _initial_limit: usize,
    committed_old_generation: usize,
    young_large_objects: usize,
    reconcile_out: *mut i32,
) -> usize {
    if data.is_null() {
        return current_limit;
    }
    // SAFETY: see the function's safety contract.
    let tracker = unsafe { &*(data as *const EnvHeapCharge) };
    let committed = committed_old_generation as u64;
    let charged = tracker.charged_limit.load(Ordering::Acquire);
    let overshoot = committed.saturating_sub(charged);
    let grant = overshoot.saturating_add(DEFAULT_HEAP_GROW_STEP);
    let answer = match tracker.budget.try_charge(Pool::V8HeapReserved, grant) {
        Ok(()) => {
            tracker.granted.fetch_add(grant, Ordering::AcqRel);
            let paid = charged.saturating_add(grant);
            tracker.charged_limit.store(paid, Ordering::Release);
            // V8 applies only a limit above the current one, and aborts if a
            // forced call gets none.
            let mut answer = paid.max((current_limit.saturating_add(HEAP_PAGE)) as u64);
            // An allocation may be pending (see above): lend it room, unless
            // the isolate is already stopping on its emergency headroom (whose
            // exposure is counted there). Every paid grant lends: the
            // committed figure can exceed the limit while V8's capacity, which
            // its retry is checked against, does not.
            let lease = if tracker.emergency_exposed.load(Ordering::Acquire) == 0 {
                let pending = committed
                    .saturating_add(young_large_objects as u64)
                    .saturating_add(tracker.budget.heap_emergency_headroom());
                answer = answer.max(pending);
                answer - paid
            } else {
                0
            };
            tracker.set_lease(lease);
            if lease > 0 && !reconcile_out.is_null() {
                // SAFETY: see the function's safety contract.
                unsafe { reconcile_out.write(1) };
            }
            usize::try_from(answer).unwrap_or(usize::MAX)
        }
        Err(_) => {
            tracker.deny();
            tracker
                .expose_emergency(current_limit, overshoot)
                .unwrap_or_else(|| {
                    // The stopping isolate went past the headroom before
                    // reaching an interrupt check. Nothing more can be
                    // granted safely.
                    HEAP_EMERGENCY_EXHAUSTED.fetch_add(1, Ordering::AcqRel);
                    current_limit.saturating_add(HEAP_PAGE)
                })
        }
    };
    tracker.old_limit.store(answer as u64, Ordering::Release);
    answer
}

/// What V8 needs to move a limit by at least one page.
const HEAP_PAGE: usize = 256 * 1024;

/// Ends a lease [`napi_host_near_heap_limit_grant`] lent V8. On the isolate's
/// thread, outside a collection, after the pending allocation: charges what
/// the heap uses past what the budget paid for, and what V8 will keep when its
/// limit is restored (never below 1.25 times the live bytes), then writes the
/// limit V8 must be lowered to. Returns 1 when V8's limit must be lowered, 0
/// otherwise (no lease, or the budget refused: the application is then
/// stopped, and the lent headroom becomes its emergency headroom).
///
/// # Safety
/// As [`napi_host_near_heap_limit_grant`]; `limit_out` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn napi_host_heap_reconcile(
    data: *const c_void,
    committed_old_generation: usize,
    young_large_objects: usize,
    used: usize,
    limit_out: *mut usize,
) -> i32 {
    if data.is_null() || limit_out.is_null() {
        return 0;
    }
    // SAFETY: see the function's safety contract.
    let tracker = unsafe { &*(data as *const EnvHeapCharge) };
    let lease = tracker.lease.load(Ordering::Acquire);
    if lease == 0 {
        return 0;
    }
    tracker.set_lease(0);
    if tracker.emergency_exposed.load(Ordering::Acquire) != 0 {
        return 0;
    }
    let charged = tracker.charged_limit.load(Ordering::Acquire);
    let in_use = (committed_old_generation as u64).saturating_add(young_large_objects as u64);
    let floor = (used as u64).saturating_add(used as u64 / 4);
    let target = in_use.max(floor).max(charged);
    let growth = target - charged;
    if tracker
        .budget
        .try_charge(Pool::V8HeapReserved, growth)
        .is_err()
    {
        // The lent headroom stays: the isolate is stopping on it.
        tracker.deny();
        let current = tracker.old_limit.load(Ordering::Acquire);
        let exposed = current.saturating_sub(charged);
        if tracker
            .emergency_exposed
            .compare_exchange(0, exposed, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            tracker.budget.expose_heap_emergency(exposed);
        }
        return 0;
    }
    tracker.granted.fetch_add(growth, Ordering::AcqRel);
    tracker.charged_limit.store(target, Ordering::Release);
    tracker.old_limit.store(target, Ordering::Release);
    // SAFETY: see the function's safety contract.
    unsafe { limit_out.write(usize::try_from(target).unwrap_or(usize::MAX)) };
    1
}

/// Whether a lease is out (the bridge then reconciles it at the next return
/// from JavaScript).
///
/// # Safety
/// As [`napi_host_near_heap_limit_grant`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn napi_host_heap_lease_pending(data: *const c_void) -> i32 {
    if data.is_null() {
        return 0;
    }
    // SAFETY: see the function's safety contract.
    let tracker = unsafe { &*(data as *const EnvHeapCharge) };
    i32::from(tracker.lease.load(Ordering::Acquire) != 0)
}

/// The isolate's old-generation limit as last answered or set.
///
/// # Safety
/// As [`napi_host_near_heap_limit_grant`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn napi_host_heap_old_limit(data: *const c_void) -> u64 {
    if data.is_null() {
        return 0;
    }
    // SAFETY: see the function's safety contract.
    let tracker = unsafe { &*(data as *const EnvHeapCharge) };
    tracker.old_limit.load(Ordering::Acquire)
}

/// Charges `bytes` of host bookkeeping for the native bridge (see
/// [`Pool::HostBookkeeping`]). Nonzero on success.
///
/// # Safety
/// `budget` must be a reference to a [`ResourceBudget`] made by
/// [`ResourceBudget::bridge_reference`] and not yet released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn napi_host_bookkeeping_charge(budget: *const c_void, bytes: u64) -> i32 {
    if budget.is_null() {
        return 0;
    }
    // SAFETY: see the function's safety contract.
    let budget = unsafe { &*(budget as *const ResourceBudget) };
    i32::from(budget.try_charge(Pool::HostBookkeeping, bytes).is_ok())
}

/// Returns `bytes` of host bookkeeping the bridge charged.
///
/// # Safety
/// As [`napi_host_bookkeeping_charge`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn napi_host_bookkeeping_uncharge(budget: *const c_void, bytes: u64) {
    if budget.is_null() {
        return;
    }
    // SAFETY: see the function's safety contract.
    let budget = unsafe { &*(budget as *const ResourceBudget) };
    budget.uncharge(Pool::HostBookkeeping, bytes);
}

/// Drops the bridge's reference to a budget.
///
/// # Safety
/// `budget` must come from [`ResourceBudget::bridge_reference`], and is
/// unusable afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn napi_host_budget_release(budget: *const c_void) {
    if !budget.is_null() {
        // SAFETY: see the function's safety contract.
        drop(unsafe { Arc::from_raw(budget as *const ResourceBudget) });
    }
}

/// The budget pool a provider charge names (`NapiHostMemory::Pool` in
/// v8/src/internal/napi_v8_agent.h).
fn provider_pool(pool: u32) -> Option<Pool> {
    match pool {
        0 => Some(Pool::HostBookkeeping),
        1 => Some(Pool::HostTransient),
        2 => Some(Pool::SerializedMessage),
        _ => None,
    }
}

/// Charges `bytes` of host memory the provider is about to allocate to
/// `pool`; a soft charge reaches the accountant's `try_charge_soft`. Nonzero
/// on success.
///
/// # Safety
/// As [`napi_host_bookkeeping_charge`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn napi_host_memory_charge(
    budget: *const c_void,
    pool: u32,
    bytes: u64,
    soft: i32,
) -> i32 {
    let Some(pool) = provider_pool(pool) else {
        return 0;
    };
    if budget.is_null() {
        return 0;
    }
    // SAFETY: see the function's safety contract.
    let budget = unsafe { &*(budget as *const ResourceBudget) };
    let charged = if soft != 0 {
        budget.try_charge_soft(pool, bytes)
    } else {
        budget.try_charge(pool, bytes)
    };
    i32::from(charged.is_ok())
}

/// Returns `bytes` the provider charged to `pool`.
///
/// # Safety
/// As [`napi_host_bookkeeping_charge`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn napi_host_memory_uncharge(budget: *const c_void, pool: u32, bytes: u64) {
    let Some(pool) = provider_pool(pool) else {
        return;
    };
    if budget.is_null() {
        return;
    }
    // SAFETY: see the function's safety contract.
    let budget = unsafe { &*(budget as *const ResourceBudget) };
    budget.uncharge(pool, bytes);
}

/// Takes one more reference to a budget the bridge holds, which
/// [`napi_host_budget_release`] returns.
///
/// # Safety
/// `budget` must be a live reference from [`ResourceBudget::bridge_reference`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn napi_host_budget_retain(budget: *const c_void) {
    if !budget.is_null() {
        // SAFETY: see the function's safety contract.
        unsafe { Arc::increment_strong_count(budget as *const ResourceBudget) };
    }
}

impl ResourceBudget {
    /// A reference to `budget` for the native bridge, which returns it with
    /// [`napi_host_budget_release`] (or the caller with
    /// [`ResourceBudget::drop_bridge_reference`] if the bridge refused it).
    pub(crate) fn bridge_reference(budget: &Arc<Self>) -> *const c_void {
        Arc::into_raw(Arc::clone(budget)) as *const c_void
    }

    /// Drops a reference from [`ResourceBudget::bridge_reference`] that the
    /// bridge did not take.
    pub(crate) fn drop_bridge_reference(reference: *const c_void) {
        // SAFETY: the bridge did not take it, so it is still the caller's.
        unsafe { napi_host_budget_release(reference) };
    }
}

/// The live charge for one physical wasm-memory allocation.
///
/// Shared (`Arc`) between all [`BudgetedMemory`] handles that refer to the same
/// underlying mmap — cloning a shared memory shares this, so the charge is
/// released exactly once, when the last handle drops.
#[derive(Debug)]
#[cfg(not(target_arch = "wasm32"))]
struct MemoryCharge {
    budget: Arc<ResourceBudget>,
    /// Bytes currently charged for this allocation.
    bytes: AtomicU64,
}

#[cfg(not(target_arch = "wasm32"))]
impl MemoryCharge {
    fn new(budget: Arc<ResourceBudget>, bytes: u64) -> Result<Arc<Self>, OverBudget> {
        budget.try_charge(Pool::WasmLinear, bytes)?;
        Ok(Arc::new(Self {
            budget,
            bytes: AtomicU64::new(bytes),
        }))
    }
}

#[cfg(not(target_arch = "wasm32"))]
impl Drop for MemoryCharge {
    fn drop(&mut self) {
        let bytes = self.bytes.swap(0, Ordering::AcqRel);
        self.budget.uncharge(Pool::WasmLinear, bytes);
    }
}

/// A [`LinearMemory`] that charges its bytes against a [`ResourceBudget`].
///
/// Delegates every operation to an inner backend `VMMemory`, except that growth
/// is charged first: `grow` (and `grow_at_least`) reserve the delta against the
/// budget and fail with [`MemoryError::CouldNotGrow`] — which the guest sees as
/// `memory.grow` returning `-1`, i.e. an ordinary allocation failure — when the
/// budget is exhausted. The initial (minimum) size is charged at construction.
#[derive(Debug)]
#[cfg(not(target_arch = "wasm32"))]
pub struct BudgetedMemory {
    inner: VMMemory,
    charge: Arc<MemoryCharge>,
}

#[cfg(not(target_arch = "wasm32"))]
impl BudgetedMemory {
    /// Wrap an already-allocated backend memory, charging its current size.
    ///
    /// The base tunables allocate the minimum pages before we can charge, so a
    /// minimum that alone exceeds the budget is allocated then rejected here;
    /// the transient over-allocation is one minimum-sized memory and is freed
    /// as `inner` drops on the error path.
    pub fn new(inner: VMMemory, budget: Arc<ResourceBudget>) -> Result<Self, MemoryError> {
        let bytes = pages_to_bytes(inner.size());
        let charge = MemoryCharge::new(budget, bytes).map_err(over_budget_to_memory_error)?;
        Ok(Self { inner, charge })
    }

    fn budget(&self) -> &Arc<ResourceBudget> {
        &self.charge.budget
    }
}

#[cfg(not(target_arch = "wasm32"))]
fn over_budget_to_memory_error(err: OverBudget) -> MemoryError {
    MemoryError::Generic(err.to_string())
}

#[cfg(not(target_arch = "wasm32"))]
impl LinearMemory for BudgetedMemory {
    fn ty(&self) -> MemoryType {
        self.inner.ty()
    }

    fn size(&self) -> Pages {
        self.inner.size()
    }

    fn style(&self) -> MemoryStyle {
        self.inner.style()
    }

    fn grow(&mut self, delta: Pages) -> Result<Pages, MemoryError> {
        let delta_bytes = pages_to_bytes(delta);
        // Reserve first: a denied charge must look exactly like hitting the
        // memory's maximum, so the guest's `memory.grow` returns -1.
        self.budget()
            .try_charge(Pool::WasmLinear, delta_bytes)
            .map_err(|_| MemoryError::CouldNotGrow {
                current: self.inner.size(),
                attempted_delta: delta,
            })?;

        match self.inner.grow(delta) {
            Ok(previous) => {
                self.charge.bytes.fetch_add(delta_bytes, Ordering::AcqRel);
                Ok(previous)
            }
            Err(err) => {
                // The real grow failed (e.g. hit its own maximum); give the
                // reservation back so it does not leak against the budget.
                self.budget().uncharge(Pool::WasmLinear, delta_bytes);
                Err(err)
            }
        }
    }

    fn grow_at_least(&mut self, min_size: u64) -> Result<(), MemoryError> {
        let before = pages_to_bytes(self.inner.size());
        if min_size <= before {
            // Already big enough; the inner call is a no-op that cannot grow.
            return self.inner.grow_at_least(min_size);
        }

        // Reserve an upper bound (page-rounded target minus current) up front,
        // then reconcile to the size actually reached.
        let reserve = round_up_to_page(min_size).saturating_sub(before);
        self.budget()
            .try_charge(Pool::WasmLinear, reserve)
            .map_err(|_| MemoryError::CouldNotGrow {
                current: self.inner.size(),
                attempted_delta: Pages::from_bytes_rounded_up(min_size.saturating_sub(before))
                    .unwrap_or(Pages(u32::MAX)),
            })?;

        match self.inner.grow_at_least(min_size) {
            Ok(()) => {
                let actual = pages_to_bytes(self.inner.size()).saturating_sub(before);
                if reserve > actual {
                    self.budget().uncharge(Pool::WasmLinear, reserve - actual);
                }
                self.charge.bytes.fetch_add(actual, Ordering::AcqRel);
                Ok(())
            }
            Err(err) => {
                self.budget().uncharge(Pool::WasmLinear, reserve);
                Err(err)
            }
        }
    }

    fn reset(&mut self) -> Result<(), MemoryError> {
        self.inner.reset()?;
        // reset only ever shrinks; release the freed bytes.
        let after = pages_to_bytes(self.inner.size());
        let previous = self.charge.bytes.swap(after, Ordering::AcqRel);
        if previous > after {
            self.budget().uncharge(Pool::WasmLinear, previous - after);
        }
        Ok(())
    }

    fn vmmemory(&self) -> std::ptr::NonNull<VMMemoryDefinition> {
        self.inner.vmmemory()
    }

    fn try_clone(&self) -> Result<Box<dyn LinearMemory + Send + Sync + 'static>, MemoryError> {
        // A clone shares the same underlying allocation (shared memory), so it
        // shares the same charge: the bytes are counted once and released when
        // the last handle drops.
        // `VMMemory::try_clone` (inherent) already yields a `VMMemory`.
        let inner = self.inner.try_clone()?;
        Ok(Box::new(BudgetedMemory {
            inner,
            charge: Arc::clone(&self.charge),
        }))
    }

    fn copy(&self) -> Result<Box<dyn LinearMemory + Send + Sync + 'static>, MemoryError> {
        // A copy is a genuinely new allocation, so it needs its own charge.
        let forked = self.inner.copy()?;
        let bytes = pages_to_bytes(forked.size());
        let charge = MemoryCharge::new(Arc::clone(self.budget()), bytes)
            .map_err(over_budget_to_memory_error)?;
        Ok(Box::new(BudgetedMemory {
            inner: VMMemory::from(forked),
            charge,
        }))
    }

    fn as_shared(&self) -> Result<VMSharedMemory, MemoryError> {
        self.inner.as_shared()
    }

    unsafe fn do_wait(
        &mut self,
        dst: u32,
        expected: ExpectedValue,
        timeout: Option<Duration>,
    ) -> Result<u32, WaiterError> {
        // SAFETY: forwarded verbatim to the inner memory, whose contract we
        // inherit; `dst` validity/alignment is the caller's responsibility.
        unsafe { self.inner.do_wait(dst, expected, timeout) }
    }

    fn do_notify(&mut self, dst: u32, count: u32) -> u32 {
        self.inner.do_notify(dst, count)
    }

    fn thread_conditions(&self) -> Option<&ThreadConditions> {
        self.inner.thread_conditions()
    }
}

/// [`Tunables`] that wrap every host memory in a [`BudgetedMemory`] and clamp a
/// requested memory's maximum to what the budget could ever grant.
///
/// All other logic delegates to the wrapped base tunables — mirroring the
/// `tunables_limit_memory` example. Module-*defined* memories (created via
/// `create_vm_memory` into a fixed VM slot) are left to the base tunables in
/// Phase 1; the edgejs guest *imports* its memory, so it flows through
/// `create_host_memory` and is charged. The max-pages clamp still bounds the
/// defined case cheaply.
#[cfg(not(target_arch = "wasm32"))]
pub struct BudgetedTunables<T: Tunables> {
    base: T,
    budget: Arc<ResourceBudget>,
}

#[cfg(not(target_arch = "wasm32"))]
impl<T: Tunables> BudgetedTunables<T> {
    /// Wrap `base`, charging every host memory against `budget`.
    pub fn new(base: T, budget: Arc<ResourceBudget>) -> Self {
        Self { base, budget }
    }

    /// The whole-budget page ceiling, or `None` when unlimited.
    fn budget_max_pages(&self) -> Option<Pages> {
        if self.budget.is_unlimited() {
            return None;
        }
        let pages = (self.budget.memory_limit() / WASM_PAGE_SIZE as u64).min(u64::from(u32::MAX));
        Some(Pages(pages as u32))
    }

    /// Clamp a requested memory's maximum to the budget ceiling (cheap layer-1
    /// cap; exact accounting still happens in [`BudgetedMemory`]).
    fn adjust_memory(&self, requested: &MemoryType) -> MemoryType {
        let mut adjusted = *requested;
        if let Some(cap) = self.budget_max_pages() {
            adjusted.maximum = Some(match adjusted.maximum {
                Some(max) => max.min(cap),
                None => cap,
            });
        }
        adjusted
    }

    fn validate_memory(&self, ty: &MemoryType) -> Result<(), MemoryError> {
        if let Some(cap) = self.budget_max_pages()
            && ty.minimum > cap
        {
            return Err(MemoryError::MinimumMemoryTooLarge {
                min_requested: ty.minimum,
                max_allowed: cap,
            });
        }
        Ok(())
    }
}

#[cfg(not(target_arch = "wasm32"))]
impl<T: Tunables> Tunables for BudgetedTunables<T> {
    fn memory_style(&self, memory: &MemoryType) -> MemoryStyle {
        self.base.memory_style(&self.adjust_memory(memory))
    }

    fn table_style(&self, table: &TableType) -> TableStyle {
        self.base.table_style(table)
    }

    fn create_host_memory(
        &self,
        ty: &MemoryType,
        style: &MemoryStyle,
    ) -> Result<VMMemory, MemoryError> {
        let adjusted = self.adjust_memory(ty);
        self.validate_memory(&adjusted)?;
        let inner = self.base.create_host_memory(&adjusted, style)?;
        let budgeted = BudgetedMemory::new(inner, Arc::clone(&self.budget))?;
        Ok(VMMemory::from(
            Box::new(budgeted) as Box<dyn LinearMemory + Send + Sync + 'static>
        ))
    }

    unsafe fn create_vm_memory(
        &self,
        ty: &MemoryType,
        style: &MemoryStyle,
        vm_definition_location: std::ptr::NonNull<VMMemoryDefinition>,
    ) -> Result<VMMemory, MemoryError> {
        let adjusted = self.adjust_memory(ty);
        self.validate_memory(&adjusted)?;
        // SAFETY: contract forwarded to base; `vm_definition_location` validity
        // is the caller's responsibility.
        unsafe {
            self.base
                .create_vm_memory(&adjusted, style, vm_definition_location)
        }
    }

    fn create_host_table(&self, ty: &TableType, style: &TableStyle) -> Result<VMTable, String> {
        self.base.create_host_table(ty, style)
    }

    unsafe fn create_vm_table(
        &self,
        ty: &TableType,
        style: &TableStyle,
        vm_definition_location: std::ptr::NonNull<VMTableDefinition>,
    ) -> Result<VMTable, String> {
        // SAFETY: contract forwarded to base.
        unsafe { self.base.create_vm_table(ty, style, vm_definition_location) }
    }
}

/// Build [`BudgetedTunables`] over the platform default base tunables.
#[cfg(not(target_arch = "wasm32"))]
pub fn budgeted_tunables(budget: Arc<ResourceBudget>) -> BudgetedTunables<BaseTunables> {
    BudgetedTunables::new(BaseTunables::new(), budget)
}

#[cfg(all(test, not(target_arch = "wasm32")))]
mod tests {
    use super::*;
    use wasmer::sys::{Cranelift, EngineBuilder};
    use wasmer::{Memory, MemoryType, Pages, Store, WASM_PAGE_SIZE};

    const PAGE: u64 = WASM_PAGE_SIZE as u64;

    /// A store whose engine charges guest wasm linear memory against `budget`.
    fn budgeted_store(budget: Arc<ResourceBudget>) -> Store {
        let mut engine = EngineBuilder::new(Cranelift::default()).engine();
        let tunables = budgeted_tunables(budget);
        engine.set_tunables(tunables);
        Store::new(engine)
    }

    #[test]
    fn try_charge_tracks_and_rejects() {
        let budget = ResourceBudget::with_memory_limit(100);
        budget.try_charge(Pool::WasmLinear, 60).expect("fits");
        assert_eq!(budget.memory_charged(), 60);
        assert_eq!(budget.memory_remaining(), 40);

        // A charge past the total is rejected atomically and changes nothing.
        let err = budget
            .try_charge(Pool::WasmLinear, 50)
            .expect_err("exceeds budget");
        assert_eq!(err.requested, 50);
        assert_eq!(err.charged, 60);
        assert_eq!(budget.memory_charged(), 60);

        budget.try_charge(Pool::WasmLinear, 40).expect("exact fit");
        assert_eq!(budget.memory_remaining(), 0);

        budget.uncharge(Pool::WasmLinear, 100);
        assert_eq!(budget.memory_charged(), 0);
        assert_eq!(budget.snapshot().wasm_linear, 0);
    }

    #[test]
    fn unlimited_budget_never_denies() {
        let budget = ResourceBudget::unlimited();
        assert!(budget.is_unlimited());
        budget
            .try_charge(Pool::WasmLinear, u64::from(u32::MAX))
            .expect("unlimited budget accepts any charge");
        assert_eq!(budget.snapshot().wasm_linear, u64::from(u32::MAX));
    }

    #[test]
    fn static_clamp_bounds_memory_maximum() {
        // Budget of 10 pages; a memory that requests no maximum is clamped.
        let budget = ResourceBudget::with_memory_limit(10 * PAGE);
        let mut store = budgeted_store(budget);
        let memory =
            Memory::new(&mut store, MemoryType::new(1, None, false)).expect("1-page memory fits");
        assert_eq!(memory.ty(&store).maximum, Some(Pages(10)));
    }

    #[test]
    fn wasm_memory_grow_bomb_is_capped() {
        // Budget of 10 pages. A guest that grows forever must be stopped at the
        // budget, with the failure surfacing as an ordinary grow failure.
        let budget = ResourceBudget::with_memory_limit(10 * PAGE);
        let mut store = budgeted_store(Arc::clone(&budget));

        let memory =
            Memory::new(&mut store, MemoryType::new(1, None, false)).expect("initial page fits");
        assert_eq!(budget.memory_charged(), PAGE, "minimum charged up front");

        let mut pages = 1u32;
        while memory.grow(&mut store, Pages(1)).is_ok() {
            pages += 1;
            assert!(pages <= 100, "grow was never capped");
        }

        assert_eq!(pages, 10, "capped at the 10-page budget");
        assert_eq!(budget.memory_charged(), 10 * PAGE);
        // Still denied after the cap, and no charge leaked from the attempt.
        assert!(memory.grow(&mut store, Pages(1)).is_err());
        assert_eq!(budget.memory_charged(), 10 * PAGE);
    }

    #[test]
    fn charges_released_when_memory_drops() {
        let budget = ResourceBudget::with_memory_limit(100 * PAGE);
        {
            let mut store = budgeted_store(Arc::clone(&budget));
            let memory =
                Memory::new(&mut store, MemoryType::new(3, None, false)).expect("3 pages fit");
            memory
                .grow(&mut store, Pages(5))
                .expect("grow within budget");
            assert_eq!(budget.memory_charged(), 8 * PAGE, "3 min + 5 grown");
        }
        assert_eq!(
            budget.memory_charged(),
            0,
            "dropping the store releases the memory's charge"
        );
    }

    #[test]
    fn separate_memories_share_one_budget() {
        // Two memories in one store draw from the same budget; the aggregate,
        // not either one alone, is what the cap bounds.
        let budget = ResourceBudget::with_memory_limit(6 * PAGE);
        let mut store = budgeted_store(Arc::clone(&budget));

        let _a = Memory::new(&mut store, MemoryType::new(4, None, false)).expect("first fits");
        assert_eq!(budget.memory_charged(), 4 * PAGE);

        // The second memory's minimum (4 pages) no longer fits in the 2 pages
        // left, so creation fails rather than overrunning the shared budget.
        let b = Memory::new(&mut store, MemoryType::new(4, None, false));
        assert!(b.is_err(), "second memory exceeds the shared budget");
        assert_eq!(
            budget.memory_charged(),
            4 * PAGE,
            "failed create charged nothing"
        );

        // A memory that does fit is accepted.
        let _c = Memory::new(&mut store, MemoryType::new(2, None, false)).expect("2 pages fit");
        assert_eq!(budget.memory_charged(), 6 * PAGE);
    }

    #[test]
    fn env_reservation_charges_ceiling_and_releases() {
        let budget = ResourceBudget::with_memory_limit(100 * MIB);
        let res = budget
            .try_reserve_env(RequestedHeap::default(), None)
            .expect("env fits");
        assert!(res.clamped);
        // Default old-gen (64 MiB) fits, plus 8 MiB overhead and 16 MiB unwind slack.
        assert_eq!(u64::from(res.max_old), DEFAULT_INITIAL_ISOLATE_HEAP);
        assert_eq!(res.ceiling_bytes, 88 * MIB);
        assert_eq!(budget.snapshot().v8_heap_reserved, 88 * MIB);
        assert_eq!(budget.live_isolates(), 1);

        budget.release_env(res.ceiling_bytes);
        assert_eq!(budget.snapshot().v8_heap_reserved, 0);
        assert_eq!(budget.live_isolates(), 0);
    }

    #[test]
    fn env_reservation_clamps_old_gen_to_fit() {
        // Only 40 MiB: overhead (8) and unwind slack (16) leave 16 MiB for
        // old-gen, below the 64 MiB default, so it is clamped to fit.
        let budget = ResourceBudget::with_memory_limit(40 * MIB);
        let res = budget
            .try_reserve_env(RequestedHeap::default(), None)
            .expect("clamped env fits");
        assert_eq!(u64::from(res.max_old), 16 * MIB);
        assert_eq!(res.ceiling_bytes, 40 * MIB);
    }

    #[test]
    fn env_reservation_counts_explicit_young_and_code() {
        let budget = ResourceBudget::with_memory_limit(100 * MIB);
        let req = RequestedHeap {
            max_young: (4 * MIB) as u32,
            max_old: (16 * MIB) as u32,
            code_range: (2 * MIB) as u32,
        };
        let res = budget.try_reserve_env(req, None).expect("fits");
        // ceiling = overhead(8) + unwind(16) + young(4) + code(2) + old(16).
        assert_eq!(res.max_young, (4 * MIB) as u32);
        assert_eq!(res.max_old, (16 * MIB) as u32);
        assert_eq!(res.code_range, (2 * MIB) as u32);
        assert_eq!(res.ceiling_bytes, 46 * MIB);
    }

    #[test]
    fn env_reservation_refused_when_heap_cannot_fit() {
        // Below overhead plus unwind slack, so no viable heap exists.
        let budget = ResourceBudget::with_memory_limit(4 * MIB);
        let err = budget
            .try_reserve_env(RequestedHeap::default(), None)
            .expect_err("too small for any env");
        assert_eq!(err, EnvRejected::HeapDoesNotFit);
        // The isolate slot claimed up front was rolled back.
        assert_eq!(budget.live_isolates(), 0);
    }

    #[test]
    fn max_envs_caps_live_isolates() {
        let budget = ResourceBudget::with_memory_limit(1000 * MIB);
        let r1 = budget
            .try_reserve_env(RequestedHeap::default(), Some(2))
            .expect("first env");
        let _r2 = budget
            .try_reserve_env(RequestedHeap::default(), Some(2))
            .expect("second env");
        assert_eq!(budget.live_isolates(), 2);

        let err = budget
            .try_reserve_env(RequestedHeap::default(), Some(2))
            .expect_err("third env exceeds max_envs");
        assert_eq!(err, EnvRejected::TooManyEnvs);
        // A refused env neither counted nor charged.
        assert_eq!(budget.live_isolates(), 2);

        budget.release_env(r1.ceiling_bytes);
        assert_eq!(budget.live_isolates(), 1);
        // A slot freed, so another env fits again.
        let _r3 = budget
            .try_reserve_env(RequestedHeap::default(), Some(2))
            .expect("env fits after release");
    }

    #[test]
    fn unlimited_budget_passes_env_request_through() {
        let budget = ResourceBudget::unlimited();
        let req = RequestedHeap {
            max_young: 1,
            max_old: 2,
            code_range: 3,
        };
        let res = budget.try_reserve_env(req, None).expect("always fits");
        assert!(!res.clamped);
        assert_eq!(res.ceiling_bytes, 0);
        assert_eq!(
            (res.max_young, res.max_old, res.code_range),
            (1, 2, 3),
            "constraints forwarded unchanged"
        );
        assert_eq!(budget.snapshot().v8_heap_reserved, 0);
        assert_eq!(budget.live_isolates(), 1, "still counted for observability");
    }

    fn grant(data: *const c_void, current: usize, committed: usize) -> (usize, bool) {
        let mut reconcile = 0;
        let answer = unsafe {
            napi_host_near_heap_limit_grant(data, current, current, committed, 0, &mut reconcile)
        };
        (answer, reconcile != 0)
    }

    #[test]
    fn near_heap_limit_callback_grants_until_budget_exhausted() {
        let step = DEFAULT_HEAP_GROW_STEP as usize;
        // The unwind slack is pre-reserved as part of the env ceiling, with room
        // for exactly two grow steps, each with a one-byte overshoot.
        let budget = ResourceBudget::with_memory_limit(
            DEFAULT_UNWIND_SLACK + 2 * DEFAULT_HEAP_GROW_STEP + 2,
        );
        budget
            .try_charge(Pool::V8HeapReserved, DEFAULT_UNWIND_SLACK)
            .unwrap();
        // No headroom: the grants below lend nothing (see the lease tests).
        budget.set_heap_emergency_headroom(0);
        let host_stopped = Arc::new(AtomicBool::new(false));
        let base = 100 * 1024 * 1024usize;
        let ptr = Box::into_raw(Box::new(EnvHeapCharge::new(
            Arc::clone(&budget),
            0,
            Arc::clone(&host_stopped),
            base as u64,
        )));
        let data = ptr as *const c_void;
        // With no headroom configured, the exposure is the slack plus the
        // overshoot.
        let emergency = (DEFAULT_UNWIND_SLACK + 1) as usize;
        let grants_before = heap_emergency_stats().grants;

        // Forced calls (the old generation already past the limit) raise the
        // limit by the overshoot plus a step.
        assert_eq!(grant(data, base, base + 1), (base + step + 1, false));
        assert_eq!(
            grant(data, base + step + 1, base + step + 2),
            (base + 2 * step + 2, false)
        );
        assert_eq!(
            budget.snapshot().v8_heap_reserved,
            DEFAULT_UNWIND_SLACK + 2 * DEFAULT_HEAP_GROW_STEP + 2
        );
        assert_eq!(budget.snapshot().v8_heap_lease, 0);

        // Budget exhaustion requests termination and exposes the unwind slack
        // plus the emergency headroom (here: the overshoot) exactly once; it
        // is reported outside the budget, not charged to it.
        let limit = base + 2 * step + 2;
        assert_eq!(grant(data, limit, limit + 1), (limit + emergency, false));
        // Later refusals only nudge the limit by a page.
        assert_eq!(
            grant(data, limit + emergency, limit + emergency + 1),
            (limit + emergency + HEAP_PAGE, false)
        );
        let usage = budget.snapshot();
        assert_eq!(usage.v8_heap_emergency, emergency as u64);
        assert!(heap_emergency_stats().grants > grants_before);
        assert!(host_stopped.load(Ordering::Acquire));

        // Teardown returns the grants and the exposure.
        let tracker = unsafe { Box::from_raw(ptr) };
        let granted = tracker.granted.load(Ordering::Acquire);
        budget.uncharge(Pool::V8HeapReserved, granted + DEFAULT_UNWIND_SLACK);
        tracker.release_exposure();
        let usage = budget.snapshot();
        assert_eq!(usage.v8_heap_reserved, 0);
        assert_eq!(usage.v8_heap_emergency, 0);
    }

    #[test]
    fn a_grant_before_a_pending_allocation_lends_headroom_until_reconciled() {
        let budget = ResourceBudget::with_memory_limit(1024 * MIB);
        let limit = 64 * MIB as usize;
        let ptr = Box::into_raw(Box::new(EnvHeapCharge::new(
            Arc::clone(&budget),
            0,
            Arc::new(AtomicBool::new(false)),
            limit as u64,
        )));
        let data = ptr as *const c_void;
        // Within the limit: an allocation (up to one maximal object) may be
        // pending. One step is paid; a maximal object's room is lent.
        let committed = limit - MIB as usize;
        let (answer, reconcile) = grant(data, limit, committed);
        assert!(reconcile);
        assert_eq!(
            answer as u64,
            committed as u64 + budget.heap_emergency_headroom()
        );
        let paid = limit as u64 + DEFAULT_HEAP_GROW_STEP;
        assert_eq!(budget.snapshot().v8_heap_reserved, DEFAULT_HEAP_GROW_STEP);
        assert_eq!(budget.snapshot().v8_heap_lease, answer as u64 - paid);
        assert_eq!(unsafe { napi_host_heap_lease_pending(data) }, 1);

        // The allocation used 200 MiB of it: the reconcile charges that and
        // lowers V8's limit to it.
        let in_use = paid as usize + 200 * MIB as usize;
        let mut lowered = 0usize;
        assert_eq!(
            unsafe { napi_host_heap_reconcile(data, in_use, 0, in_use / 2, &mut lowered) },
            1
        );
        assert_eq!(lowered, in_use);
        assert_eq!(budget.snapshot().v8_heap_lease, 0);
        assert_eq!(
            budget.snapshot().v8_heap_reserved,
            in_use as u64 - limit as u64
        );
        assert_eq!(unsafe { napi_host_heap_old_limit(data) }, in_use as u64);
        assert_eq!(unsafe { napi_host_heap_lease_pending(data) }, 0);
        // Nothing to reconcile twice.
        assert_eq!(
            unsafe { napi_host_heap_reconcile(data, in_use, 0, 0, &mut lowered) },
            0
        );

        // The live bytes V8 keeps on restore (1.25x) are charged as well.
        let (_, reconcile) = grant(data, in_use, in_use - MIB as usize);
        assert!(reconcile);
        let used = in_use;
        assert_eq!(
            unsafe { napi_host_heap_reconcile(data, in_use, 0, used, &mut lowered) },
            1
        );
        assert_eq!(lowered as u64, used as u64 + used as u64 / 4);
        let tracker = unsafe { Box::from_raw(ptr) };
        budget.uncharge(
            Pool::V8HeapReserved,
            tracker.granted.load(Ordering::Acquire),
        );
        tracker.release_exposure();
        assert_eq!(budget.memory_charged(), 0);
    }

    #[test]
    fn a_refused_reconcile_stops_the_isolate_on_its_lent_headroom() {
        let limit = 64 * MIB as usize;
        let budget = ResourceBudget::with_memory_limit(DEFAULT_HEAP_GROW_STEP);
        let stopped = Arc::new(AtomicBool::new(false));
        let ptr = Box::into_raw(Box::new(EnvHeapCharge::new(
            Arc::clone(&budget),
            0,
            Arc::clone(&stopped),
            limit as u64,
        )));
        let data = ptr as *const c_void;
        let (answer, reconcile) = grant(data, limit, limit / 2);
        assert!(reconcile);
        let mut lowered = 0usize;
        // The pending allocation used more than the budget has left.
        let in_use = limit + 2 * DEFAULT_HEAP_GROW_STEP as usize;
        assert_eq!(
            unsafe { napi_host_heap_reconcile(data, in_use, 0, 0, &mut lowered) },
            0
        );
        assert!(stopped.load(Ordering::Acquire));
        let usage = budget.snapshot();
        assert_eq!(usage.v8_heap_lease, 0);
        assert_eq!(
            usage.v8_heap_emergency,
            answer as u64 - limit as u64 - DEFAULT_HEAP_GROW_STEP
        );
        let tracker = unsafe { Box::from_raw(ptr) };
        budget.uncharge(
            Pool::V8HeapReserved,
            tracker.granted.load(Ordering::Acquire),
        );
        tracker.release_exposure();
        assert_eq!(budget.snapshot().v8_heap_emergency, 0);
    }

    #[test]
    fn a_paid_grant_always_raises_the_limit() {
        // A stopping isolate (its emergency headroom exposed) whose budget
        // frees up again: a paid grant must still raise V8's limit, which
        // already sits above the paid figure, or V8 aborts.
        let budget = ResourceBudget::with_memory_limit(1024 * MIB);
        let limit = 64 * MIB as usize;
        let ptr = Box::into_raw(Box::new(EnvHeapCharge::new(
            Arc::clone(&budget),
            0,
            Arc::new(AtomicBool::new(false)),
            limit as u64,
        )));
        let tracker = unsafe { &*ptr };
        tracker.emergency_exposed.store(1, Ordering::Release);
        let current = limit + 1024 * MIB as usize;
        let (answer, reconcile) = grant(ptr as *const c_void, current, current - 4 * MIB as usize);
        assert!(answer > current, "{answer} <= {current}");
        assert!(!reconcile, "a stopping isolate is lent nothing");
        assert_eq!(budget.snapshot().v8_heap_lease, 0);
        let tracker = unsafe { Box::from_raw(ptr) };
        budget.uncharge(
            Pool::V8HeapReserved,
            tracker.granted.load(Ordering::Acquire),
        );
        tracker.emergency_exposed.store(0, Ordering::Release);
        tracker.release_exposure();
    }

    #[test]
    fn near_heap_limit_callback_ignores_null_data() {
        assert_eq!(
            unsafe {
                napi_host_near_heap_limit_grant(std::ptr::null(), 42, 7, 0, 0, std::ptr::null_mut())
            },
            42,
            "a null tracker leaves the limit unchanged"
        );
    }

    #[test]
    fn the_bridge_charges_bookkeeping_through_its_budget_reference() {
        let budget = ResourceBudget::with_memory_limit(HOST_BOOKKEEPING_CHUNK * 2);
        let reference = ResourceBudget::bridge_reference(&budget);
        assert_eq!(Arc::strong_count(&budget), 2);
        unsafe {
            assert_eq!(
                napi_host_bookkeeping_charge(reference, HOST_BOOKKEEPING_CHUNK),
                1
            );
            assert_eq!(
                napi_host_bookkeeping_charge(reference, HOST_BOOKKEEPING_CHUNK),
                1
            );
            // The budget is spent: the next chunk is refused.
            assert_eq!(napi_host_bookkeeping_charge(reference, 1), 0);
            assert_eq!(
                budget.snapshot().host_bookkeeping,
                2 * HOST_BOOKKEEPING_CHUNK
            );
            napi_host_bookkeeping_uncharge(reference, 2 * HOST_BOOKKEEPING_CHUNK);
            assert_eq!(napi_host_bookkeeping_charge(std::ptr::null(), 1), 0);
            napi_host_budget_release(reference);
        }
        assert_eq!(budget.memory_charged(), 0);
        assert_eq!(Arc::strong_count(&budget), 1);
    }

    #[test]
    fn soft_charges_reach_the_accountants_soft_path() {
        #[derive(Default)]
        struct Accountant {
            charged: AtomicU64,
            hard: AtomicUsize,
            soft: AtomicUsize,
        }
        impl NapiMemoryAccountant for Accountant {
            fn memory_limit(&self) -> u64 {
                100
            }
            fn memory_charged(&self) -> u64 {
                self.charged.load(Ordering::Acquire)
            }
            fn try_charge(&self, bytes: u64) -> bool {
                self.hard.fetch_add(1, Ordering::AcqRel);
                self.charged.fetch_add(bytes, Ordering::AcqRel);
                true
            }
            fn try_charge_soft(&self, bytes: u64) -> bool {
                self.soft.fetch_add(1, Ordering::AcqRel);
                if self.memory_charged() + bytes > self.memory_limit() {
                    return false;
                }
                self.charged.fetch_add(bytes, Ordering::AcqRel);
                true
            }
            fn uncharge(&self, bytes: u64) {
                self.charged.fetch_sub(bytes, Ordering::AcqRel);
            }
        }
        let accountant = Arc::new(Accountant::default());
        let budget = ResourceBudget::with_accountant(accountant.clone());
        budget.try_charge_soft(Pool::SerializedMessage, 80).unwrap();
        let refused = budget
            .try_charge_soft(Pool::SerializedMessage, 40)
            .unwrap_err();
        assert_eq!(refused.pool, Pool::SerializedMessage);
        assert_eq!(accountant.soft.load(Ordering::Acquire), 2);
        assert_eq!(accountant.hard.load(Ordering::Acquire), 0);
        assert_eq!(budget.snapshot().serialized_message, 80);
        budget.uncharge(Pool::SerializedMessage, 80);
        assert_eq!(accountant.memory_charged(), 0);

        // Without an override the soft path is the hard one.
        struct HardOnly(AtomicU64);
        impl NapiMemoryAccountant for HardOnly {
            fn memory_limit(&self) -> u64 {
                10
            }
            fn memory_charged(&self) -> u64 {
                self.0.load(Ordering::Acquire)
            }
            fn try_charge(&self, bytes: u64) -> bool {
                bytes <= 10 && self.0.fetch_add(bytes, Ordering::AcqRel) == 0
            }
            fn uncharge(&self, bytes: u64) {
                self.0.fetch_sub(bytes, Ordering::AcqRel);
            }
        }
        let budget = ResourceBudget::with_accountant(Arc::new(HardOnly(AtomicU64::new(0))));
        assert!(budget.try_charge_soft(Pool::HostTransient, 11).is_err());
        budget.try_charge_soft(Pool::HostTransient, 10).unwrap();
        budget.uncharge(Pool::HostTransient, 10);
    }

    #[test]
    fn wasm_and_external_share_one_budget() {
        // A mixed wasm + declared-external workload is bounded by the
        // *combined* cap, not either pool alone.
        let budget = ResourceBudget::with_memory_limit(10 * MIB);
        budget
            .try_charge(Pool::WasmLinear, 7 * MIB)
            .expect("wasm fits");

        // Only 3 MiB remains, so a 4 MiB external declaration is denied...
        assert!(budget.try_charge(Pool::V8External, 4 * MIB).is_err());
        // ...but 3 MiB exactly fits, exhausting the shared budget.
        budget
            .try_charge(Pool::V8External, 3 * MIB)
            .expect("remaining room fits exactly");
        assert_eq!(budget.memory_charged(), 10 * MIB);

        budget.uncharge(Pool::V8External, 3 * MIB);
        assert_eq!(budget.snapshot().v8_external, 0);
    }
}

#[cfg(test)]
mod external_accountant_tests {
    use super::*;

    struct TestAccountant {
        limit: u64,
        charged: AtomicU64,
    }

    impl TestAccountant {
        fn new(limit: u64) -> Arc<Self> {
            Arc::new(Self {
                limit,
                charged: AtomicU64::new(0),
            })
        }
    }

    impl NapiMemoryAccountant for TestAccountant {
        fn memory_limit(&self) -> u64 {
            self.limit
        }

        fn memory_charged(&self) -> u64 {
            self.charged.load(Ordering::Acquire)
        }

        fn try_charge(&self, bytes: u64) -> bool {
            let mut current = self.charged.load(Ordering::Acquire);
            loop {
                let Some(next) = current
                    .checked_add(bytes)
                    .filter(|next| *next <= self.limit)
                else {
                    return false;
                };
                match self.charged.compare_exchange_weak(
                    current,
                    next,
                    Ordering::AcqRel,
                    Ordering::Acquire,
                ) {
                    Ok(_) => return true,
                    Err(observed) => current = observed,
                }
            }
        }

        fn uncharge(&self, bytes: u64) {
            self.charged.fetch_sub(bytes, Ordering::AcqRel);
        }
    }

    #[test]
    fn external_accountant_includes_non_napi_charges() {
        let accountant = TestAccountant::new(100);
        assert!(accountant.try_charge(40));

        let external: Arc<dyn NapiMemoryAccountant> = accountant.clone();
        let budget = ResourceBudget::with_accountant(external);
        budget.try_charge(Pool::V8HeapReserved, 60).unwrap();

        let usage = budget.snapshot();
        assert_eq!(usage.mem_charged, 100);
        assert_eq!(usage.v8_heap_reserved, 60);
        assert!(budget.try_charge(Pool::V8External, 1).is_err());

        budget.uncharge(Pool::V8HeapReserved, 60);
        assert_eq!(accountant.memory_charged(), 40);
        assert_eq!(budget.snapshot().v8_heap_reserved, 0);
    }
}
