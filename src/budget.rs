//! Cross-VM resource accounting for the N-API (`imports`) provider.
//!
//! An edgejs guest running under the `imports` provider splits execution and
//! allocation across two VMs: the wasmer store (guest wasm linear memory) and
//! host-native V8. To keep an embedder-imposed memory budget honest across
//! *both*, every pool charges against one shared [`ResourceBudget`] per app.
//!
//! The accountant reserves guest wasm linear memory, V8 heap ceilings,
//! declared external memory, and a bounded background lane against one shared
//! limit. These reservations enforce a limit; callers must not mistake them
//! for observed resident memory. The Edge task manager meters CPU separately.
//!
//! ## Native sys install path
//!
//! The guest's linear memory is *imported* (host-created). wasmer's public API
//! does not let an embedder inject a custom [`LinearMemory`] into a
//! [`wasmer::Memory`] directly (the backend `VMMemory` enum is private), so the
//! charge is installed one level down, via custom [`Tunables`]: a
//! [`BudgetedTunables`] wraps [`BaseTunables`] and returns every native memory
//! wrapped in a [`BudgetedMemory`]. Installing those tunables on the engine
//! (see `cli.rs`) makes imported and module-defined memories budget-aware
//! with no change to the memory-creation call site.
//!
//! This tunables path exists only for Wasmer's native `sys` backend. A
//! `wasm32` host-JavaScript build uses the host's WebAssembly memory and does
//! not compile or install [`BudgetedTunables`]. It still uses
//! [`ResourceBudget`] for provider-owned resources such as environments,
//! value handles, and declared external memory.

#[cfg(not(target_arch = "wasm32"))]
use parking_lot::Mutex;
use std::ffi::c_void;
use std::sync::{
    Arc,
    atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
};
#[cfg(not(target_arch = "wasm32"))]
use std::time::Duration;

#[cfg(all(not(target_arch = "wasm32"), not(napi_standalone_legacy_wait)))]
use wasmer::sys::vm::StoreId;
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
/// Heap headroom, beyond the budget, that a denied growth callback exposes to
/// V8 once per isolate together with [`DEFAULT_UNWIND_SLACK`].
///
/// V8 consults the near-heap-limit callback exactly once per last-resort
/// collection and then retries the failed allocation against the hard limit;
/// a retry that still does not fit is `FatalProcessOutOfMemory`, which aborts
/// the whole process after the embedder's OOM callback. The headroom must
/// therefore cover the largest single allocation JavaScript can request: V8
/// caps every heap object at 1 GiB (`FixedArray`/`FixedDoubleArray` at 128 Mi
/// entries, strings at `String::kMaxLength` two-byte characters), so this
/// holds one such object plus large-object page headers and the small
/// allocations a non-interruptible builtin makes before the termination
/// request is observed. Raising the limit commits no memory by itself; the
/// isolate is already being terminated, so the bytes a context actually takes
/// from this headroom are bounded by what it allocates before termination
/// lands and are released with the isolate.
pub const DEFAULT_HEAP_EMERGENCY_HEADROOM: u64 = 1088 * MIB;

/// Process-wide count of isolates that received emergency heap headroom.
static HEAP_EMERGENCY_GRANTS: AtomicU64 = AtomicU64::new(0);
/// Process-wide count of near-heap-limit callbacks that found the emergency
/// headroom already spent. Each one means the stopping isolate asked for more
/// heap than the headroom before termination landed; V8 aborts the process on
/// the next failed retry, so a non-zero value is an incident, not a metric.
static HEAP_EMERGENCY_EXHAUSTED: AtomicU64 = AtomicU64::new(0);

/// Process-wide counters for the emergency heap headroom path.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct HeapEmergencyStats {
    /// Isolates that were stopped and granted emergency headroom.
    pub grants: u64,
    /// Callbacks that could not raise the limit further (see
    /// [`HEAP_EMERGENCY_EXHAUSTED`]); V8 may have aborted the process after
    /// any of them, so observing this counter non-zero in a live process is
    /// luck.
    pub exhausted: u64,
}

/// Process-wide counters for the emergency heap headroom path, for the
/// embedder's metrics and alerts.
pub fn heap_emergency_stats() -> HeapEmergencyStats {
    HeapEmergencyStats {
        grants: HEAP_EMERGENCY_GRANTS.load(Ordering::Acquire),
        exhausted: HEAP_EMERGENCY_EXHAUSTED.load(Ordering::Acquire),
    }
}
/// Fixed per-isolate overhead charged to cover young generation, code range,
/// and V8's own malloc'd metadata without sampling.
pub const DEFAULT_PER_ISOLATE_OVERHEAD: u64 = 8 * MIB;

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
    /// Fixed reservation for one dedicated V8 background lane, including its
    /// native thread stack and bounded pending task queue.
    V8BackgroundLane,
    /// Short-lived host snapshots of guest bytes and argument arrays. These
    /// are charged before allocation and released with their owning buffer.
    HostTransient,
    /// Serialized worker messages retained between a sender and a receiver.
    SerializedMessage,
    /// Host-side bookkeeping the bridge keeps per guest handle (refs, value
    /// slots, finalizer records, deferreds, scope frames, callback
    /// registrations), granted in chunks by [`napi_host_bookkeeping_charge`].
    HostBookkeeping,
}

/// Embedder-owned aggregate accounting for byte reservations made by N-API.
///
/// N-API retains its own per-pool counters; the embedder sees only byte totals,
/// keeping pool policy and future N-API implementation details out of Edge.
pub trait NapiMemoryAccountant: Send + Sync {
    fn memory_limit(&self) -> u64;
    fn memory_charged(&self) -> u64;
    fn try_charge(&self, bytes: u64) -> bool;
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
    pub v8_background_lane: u64,
    /// Live bytes in host snapshots of guest data.
    pub host_transient: u64,
    pub serialized_message: u64,
    /// Bytes granted to the bridge for per-handle host bookkeeping.
    pub host_bookkeeping: u64,
    /// V8 heap headroom currently exposed *outside* the budget to isolates
    /// whose growth was refused and that are being terminated (see
    /// [`DEFAULT_HEAP_EMERGENCY_HEADROOM`]). Non-zero means at least one
    /// isolate of this budget is stopping after exhausting its heap.
    pub v8_heap_emergency: u64,
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
/// Charging is **reserve-based** for the pools listed above. The charged total
/// is an admission limit for those pools, not an RSS measurement: some host
/// copies and V8 native allocations are outside these reservations.
/// All state is atomic so worker threads share one application budget.
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
    v8_background_lane: AtomicU64,
    host_transient: AtomicU64,
    serialized_message: AtomicU64,
    host_bookkeeping: AtomicU64,
    /// Heap headroom exposed outside the budget per stopping isolate
    /// ([`DEFAULT_HEAP_EMERGENCY_HEADROOM`] unless the embedder overrides it).
    heap_emergency_headroom: AtomicU64,
    /// Emergency headroom currently exposed, summed over stopping isolates.
    /// Not part of `mem_charged`: it is by definition over the budget.
    v8_heap_emergency: AtomicU64,
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
                "v8_background_lane",
                &self.v8_background_lane.load(Ordering::Acquire),
            )
            .field(
                "host_transient",
                &self.host_transient.load(Ordering::Acquire),
            )
            .field(
                "serialized_message",
                &self.serialized_message.load(Ordering::Acquire),
            )
            .field(
                "host_bookkeeping",
                &self.host_bookkeeping.load(Ordering::Acquire),
            )
            .field(
                "v8_heap_emergency",
                &self.v8_heap_emergency.load(Ordering::Acquire),
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
            v8_background_lane: AtomicU64::new(0),
            host_transient: AtomicU64::new(0),
            serialized_message: AtomicU64::new(0),
            host_bookkeeping: AtomicU64::new(0),
            heap_emergency_headroom: AtomicU64::new(DEFAULT_HEAP_EMERGENCY_HEADROOM),
            v8_heap_emergency: AtomicU64::new(0),
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
            v8_background_lane: AtomicU64::new(0),
            host_transient: AtomicU64::new(0),
            serialized_message: AtomicU64::new(0),
            host_bookkeeping: AtomicU64::new(0),
            heap_emergency_headroom: AtomicU64::new(DEFAULT_HEAP_EMERGENCY_HEADROOM),
            v8_heap_emergency: AtomicU64::new(0),
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
        if bytes == 0 {
            return Ok(());
        }

        if let Some(accountant) = &self.accountant {
            if !accountant.try_charge(bytes) {
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
            Pool::V8BackgroundLane => &self.v8_background_lane,
            Pool::HostTransient => &self.host_transient,
            Pool::SerializedMessage => &self.serialized_message,
            Pool::HostBookkeeping => &self.host_bookkeeping,
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
            v8_background_lane: self.v8_background_lane.load(Ordering::Acquire),
            host_transient: self.host_transient.load(Ordering::Acquire),
            serialized_message: self.serialized_message.load(Ordering::Acquire),
            host_bookkeeping: self.host_bookkeeping.load(Ordering::Acquire),
            v8_heap_emergency: self.v8_heap_emergency.load(Ordering::Acquire),
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

    /// Override the emergency headroom for isolates created after this call.
    ///
    /// Anything below [`DEFAULT_HEAP_EMERGENCY_HEADROOM`] reintroduces process
    /// aborts for single allocations larger than the configured value; the
    /// knob exists so a host that runs few, large isolates can trade that risk
    /// against a smaller transient overshoot.
    pub fn set_heap_emergency_headroom(&self, bytes: u64) {
        self.heap_emergency_headroom.store(bytes, Ordering::Release);
    }

    /// Record emergency headroom exposed to a stopping isolate. It is not a
    /// charge: the budget is exhausted when this happens.
    fn expose_heap_emergency(&self, bytes: u64) {
        self.v8_heap_emergency.fetch_add(bytes, Ordering::AcqRel);
        HEAP_EMERGENCY_GRANTS.fetch_add(1, Ordering::AcqRel);
    }

    /// The isolate that used emergency headroom is gone.
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

/// Per-V8-env budget tracker shared with the host-owned near-heap-limit
/// callback and the bridge's bookkeeping hooks.
///
/// Boxed at env creation and handed to the bridge as an opaque pointer; the
/// owning [`crate::env::NapiEnv`] reclaims it at env teardown to release the
/// bytes granted beyond the initial ceiling.
pub(crate) struct EnvHeapCharge {
    pub(crate) budget: Arc<ResourceBudget>,
    pub(crate) env: usize,
    pub(crate) host_stopped: Arc<AtomicBool>,
    /// Heap bytes exposed outside the budget by the first refused grow step
    /// (the pre-charged unwind slack plus the emergency headroom); `0` until
    /// then. Set once, so a stopping isolate cannot expand its limit
    /// repeatedly.
    pub(crate) emergency_exposed: AtomicU64,
    /// Bytes granted beyond the initial ceiling by grow-step grants.
    pub(crate) granted: AtomicU64,
    /// Bytes granted to [`Pool::HostBookkeeping`] and not yet returned.
    pub(crate) bookkeeping_granted: AtomicU64,
}

impl EnvHeapCharge {
    /// The budget refused: stop this env the way a host kill does. The sticky
    /// flag keeps later imports out and termination unwinds any running JS.
    fn deny(&self) {
        self.host_stopped.store(true, Ordering::Release);
        if self.env != 0 {
            // SAFETY: the tracker is only reachable while its env is live.
            unsafe {
                crate::snapi::snapi_bridge_unofficial_terminate_execution(
                    self.env as crate::snapi::SnapiEnv,
                );
            }
        }
    }
}

/// Host-owned bookkeeping grant for a budgeted V8 isolate: the bridge asks for
/// `bytes` more of per-handle host bookkeeping. Returns nonzero when granted;
/// a denial stops the env exactly like an exhausted heap-growth grant.
///
/// # Safety
/// As for [`napi_host_near_heap_limit_grant`].
#[unsafe(no_mangle)]
pub extern "C" fn napi_host_bookkeeping_charge(data: *const c_void, bytes: u64) -> i32 {
    if data.is_null() {
        return 1;
    }
    // SAFETY: see the function's safety contract.
    let tracker = unsafe { &*(data as *const EnvHeapCharge) };
    match tracker.budget.try_charge(Pool::HostBookkeeping, bytes) {
        Ok(()) => {
            tracker
                .bookkeeping_granted
                .fetch_add(bytes, Ordering::AcqRel);
            1
        }
        Err(_) => {
            tracker.deny();
            0
        }
    }
}

/// Returns bookkeeping the bridge no longer needs, clamped to what it was
/// granted. The bridge serializes calls per env, so a plain load/store cannot
/// race.
///
/// # Safety
/// As for [`napi_host_near_heap_limit_grant`].
#[unsafe(no_mangle)]
pub extern "C" fn napi_host_bookkeeping_uncharge(data: *const c_void, bytes: u64) {
    if data.is_null() {
        return;
    }
    // SAFETY: see the function's safety contract.
    let tracker = unsafe { &*(data as *const EnvHeapCharge) };
    let granted = tracker.bookkeeping_granted.load(Ordering::Acquire);
    let release = bytes.min(granted);
    tracker
        .bookkeeping_granted
        .store(granted - release, Ordering::Release);
    tracker.budget.uncharge(Pool::HostBookkeeping, release);
}

/// Host-owned near-heap-limit callback for a budgeted V8 isolate.
///
/// When V8 approaches a heap ceiling it invokes this on the isolate's JS
/// thread. We charge the bytes the committed old generation already exceeds
/// the limit by (`committed_old_generation - current_limit`, normally zero)
/// plus one [`DEFAULT_HEAP_GROW_STEP`] against the budget and, if granted,
/// raise the limit by that amount. If the budget refuses, request isolate
/// termination and expose, once, the pre-reserved unwind slack plus the
/// budget's emergency headroom ([`DEFAULT_HEAP_EMERGENCY_HEADROOM`], or the
/// overshoot if that is larger).
///
/// Both rules exist because V8 gives this callback exactly one answer per
/// occasion and aborts the process if the answer is too small:
///
/// * After every collection, `Heap::CollectGarbage` checks that the committed
///   old generation fits the limit, invokes the callback once if it does not,
///   and calls `FatalProcessOutOfMemory("Reached heap limit")` if it still
///   does not. The old generation can exceed the limit without any prior
///   callback because `NewLargeObjectSpace::AllocateRaw` admits the first
///   large young object regardless of the limit (a `new Array(3e7)` is one
///   120 MB object) and the next full collection promotes it. Hence the
///   overshoot term.
/// * A failed allocation ends in `AllocateRawWithRetryOrFailSlowPath`, which
///   invokes the callback once before the last-resort collection and aborts
///   with `CALL_AND_RETRY_LAST` if the retry still does not fit. A single
///   allocation can be 1 GiB, so a grow step or the slack alone is not enough
///   for a refusal. Hence the emergency headroom.
///
/// Termination lands at the next interrupt check; allocations until then come
/// out of the exposed headroom. A second refusal leaves the limit unchanged,
/// so a stopping isolate cannot expand its heap repeatedly. The budget is
/// atomic, so this is safe to call concurrently with charges on other threads.
///
/// # Safety
/// `data` must be null or a pointer to an [`EnvHeapCharge`] that outlives the
/// call. The tracker is freed only at env teardown — after the callback is
/// removed from the isolate and while no JS runs — so a non-null pointer is
/// valid for every real invocation.
#[unsafe(no_mangle)]
pub extern "C" fn napi_host_near_heap_limit_grant(
    data: *const c_void,
    current_limit: usize,
    _initial_limit: usize,
    committed_old_generation: usize,
) -> usize {
    if data.is_null() {
        return current_limit;
    }
    // SAFETY: see the function's safety contract.
    let tracker = unsafe { &*(data as *const EnvHeapCharge) };
    // Bytes by which the old generation already exceeds the limit. V8 admits
    // the first large object of the young generation without consulting the
    // limit and promotes it on the next full collection, so this can be as
    // large as one heap object (1 GiB). Right after this callback V8 compares
    // the committed old generation against the returned limit and aborts the
    // process if it is still exceeded, so every answer below covers it.
    let overshoot = committed_old_generation.saturating_sub(current_limit) as u64;
    let grant = overshoot.saturating_add(DEFAULT_HEAP_GROW_STEP);
    match tracker.budget.try_charge(Pool::V8HeapReserved, grant) {
        Ok(()) => {
            tracker.granted.fetch_add(grant, Ordering::AcqRel);
            current_limit.saturating_add(usize::try_from(grant).unwrap_or(usize::MAX))
        }
        Err(_) => {
            tracker.deny();
            let exposed = DEFAULT_UNWIND_SLACK
                .saturating_add(tracker.budget.heap_emergency_headroom().max(overshoot));
            if tracker
                .emergency_exposed
                .compare_exchange(0, exposed, Ordering::AcqRel, Ordering::Acquire)
                .is_ok()
            {
                tracker.budget.expose_heap_emergency(exposed);
                current_limit.saturating_add(usize::try_from(exposed).unwrap_or(usize::MAX))
            } else {
                // The stopping isolate kept allocating past the headroom
                // without reaching an interrupt check. Nothing more can be
                // granted safely; V8 aborts the process if its retry fails.
                HEAP_EMERGENCY_EXHAUSTED.fetch_add(1, Ordering::AcqRel);
                current_limit
            }
        }
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
    /// Shared clones must serialize the full size/reserve/mutate/reconcile
    /// sequence, not only the backend's growth operation.
    operation: Mutex<()>,
}

#[cfg(not(target_arch = "wasm32"))]
impl MemoryCharge {
    fn new(budget: Arc<ResourceBudget>, bytes: u64) -> Result<Arc<Self>, OverBudget> {
        budget.try_charge(Pool::WasmLinear, bytes)?;
        Ok(Arc::new(Self {
            budget,
            bytes: AtomicU64::new(bytes),
            operation: Mutex::new(()),
        }))
    }

    fn release(&self, bytes: u64) {
        let previous = self.bytes.fetch_sub(bytes, Ordering::AcqRel);
        debug_assert!(previous >= bytes, "linear-memory charge underflow");
        self.budget.uncharge(Pool::WasmLinear, bytes);
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
/// is charged first: `grow` (and `grow_at_least`) reserve the new high-water
/// budget and fail with [`MemoryError::CouldNotGrow`] — which the guest sees as
/// `memory.grow` returning `-1`, i.e. an ordinary allocation failure — when the
/// budget is exhausted. The initial (minimum) size is charged before backend
/// construction by [`BudgetedTunables`]. A reset retains the backing mapping,
/// so its high-water charge remains until the final handle drops.
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

    fn from_precharged(inner: VMMemory, charge: Arc<MemoryCharge>) -> Result<Self, MemoryError> {
        let actual = pages_to_bytes(inner.size());
        let reserved = charge.bytes.load(Ordering::Acquire);
        if actual > reserved {
            return Err(MemoryError::Generic(format!(
                "linear memory initialized with {actual} bytes after reserving {reserved} bytes"
            )));
        }
        charge.release(reserved - actual);
        Ok(Self { inner, charge })
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
        let charge = Arc::clone(&self.charge);
        wasmer::sys::vm::on_host_stack(|| {
            let _operation = charge.operation.lock();
            let before = pages_to_bytes(self.inner.size());
            let high_water = charge.bytes.load(Ordering::Acquire);
            let target = before.saturating_add(pages_to_bytes(delta));
            let reserve = target.saturating_sub(high_water);
            charge
                .budget
                .try_charge(Pool::WasmLinear, reserve)
                .map_err(|_| MemoryError::CouldNotGrow {
                    current: self.inner.size(),
                    attempted_delta: delta,
                })?;
            match self.inner.grow(delta) {
                Ok(previous) => {
                    let after = pages_to_bytes(self.inner.size());
                    let actual = after.saturating_sub(high_water);
                    debug_assert!(actual <= reserve);
                    charge.bytes.fetch_add(actual, Ordering::AcqRel);
                    charge.budget.uncharge(Pool::WasmLinear, reserve - actual);
                    Ok(previous)
                }
                Err(err) => {
                    charge.budget.uncharge(Pool::WasmLinear, reserve);
                    Err(err)
                }
            }
        })
    }

    fn grow_at_least(&mut self, min_size: u64) -> Result<(), MemoryError> {
        let charge = Arc::clone(&self.charge);
        wasmer::sys::vm::on_host_stack(|| {
            let _operation = charge.operation.lock();
            let before = pages_to_bytes(self.inner.size());
            if min_size <= before {
                return self.inner.grow_at_least(min_size);
            }
            let high_water = charge.bytes.load(Ordering::Acquire);
            let reserve = round_up_to_page(min_size).saturating_sub(high_water);
            charge
                .budget
                .try_charge(Pool::WasmLinear, reserve)
                .map_err(|_| MemoryError::CouldNotGrow {
                    current: self.inner.size(),
                    attempted_delta: Pages::from_bytes_rounded_up(min_size.saturating_sub(before))
                        .unwrap_or(Pages(u32::MAX)),
                })?;
            match self.inner.grow_at_least(min_size) {
                Ok(()) => {
                    let after = pages_to_bytes(self.inner.size());
                    let actual = after.saturating_sub(high_water);
                    debug_assert!(actual <= reserve);
                    charge.bytes.fetch_add(actual, Ordering::AcqRel);
                    charge.budget.uncharge(Pool::WasmLinear, reserve - actual);
                    Ok(())
                }
                Err(err) => {
                    charge.budget.uncharge(Pool::WasmLinear, reserve);
                    Err(err)
                }
            }
        })
    }

    fn reset(&mut self) -> Result<(), MemoryError> {
        let charge = Arc::clone(&self.charge);
        wasmer::sys::vm::on_host_stack(|| {
            let _operation = charge.operation.lock();
            // Wasmer resets the logical size but retains accessible backing.
            // The high-water charge stays until the final handle drops.
            self.inner.reset()
        })
    }

    fn vmmemory(&self) -> std::ptr::NonNull<VMMemoryDefinition> {
        self.inner.vmmemory()
    }

    fn try_clone(&self) -> Result<Box<dyn LinearMemory + Send + Sync + 'static>, MemoryError> {
        // A clone shares the same underlying allocation (shared memory), so it
        // shares the same charge: the bytes are counted once and released when
        // the last handle drops.
        // `VMMemory::try_clone` (inherent) already yields a `VMMemory`.
        wasmer::sys::vm::on_host_stack(|| {
            let _operation = self.charge.operation.lock();
            let inner = self.inner.try_clone()?;
            Ok(Box::new(BudgetedMemory {
                inner,
                charge: Arc::clone(&self.charge),
            })
                as Box<dyn LinearMemory + Send + Sync + 'static>)
        })
    }

    fn copy(&self) -> Result<Box<dyn LinearMemory + Send + Sync + 'static>, MemoryError> {
        wasmer::sys::vm::on_host_stack(|| {
            let _operation = self.charge.operation.lock();
            // Copy retains the source mapping's accessible backing even after
            // reset has lowered its logical size. Reserve that high-water
            // allocation before the backend allocates the copy.
            let bytes = self.charge.bytes.load(Ordering::Acquire);
            let charge = MemoryCharge::new(Arc::clone(&self.charge.budget), bytes)
                .map_err(over_budget_to_memory_error)?;
            let forked = self.inner.copy()?;
            if pages_to_bytes(forked.size()) > bytes {
                return Err(MemoryError::Generic(
                    "copied linear memory exceeded its quota reservation".into(),
                ));
            }
            Ok(Box::new(BudgetedMemory {
                inner: VMMemory::from(forked),
                charge,
            })
                as Box<dyn LinearMemory + Send + Sync + 'static>)
        })
    }

    fn as_shared(&self) -> Result<VMSharedMemory, MemoryError> {
        // The pinned public Wasmer used by the standalone CLI still detaches
        // shared memories through this raw VMSharedMemory API. Its WASIX
        // pthreads need that detach path for libuv's async workers. A raw
        // handle drops our charge wrapper, so only permit it when accounting
        // is explicitly unlimited. Managed Edge uses a newer Wasmer API that
        // preserves the wrapper and never takes this compatibility path.
        #[cfg(napi_standalone_legacy_wait)]
        if self.charge.budget.accountant.is_none() && self.charge.budget.mem_total == UNLIMITED {
            return self.inner.as_shared();
        }

        Err(MemoryError::UnsupportedOperation {
            message: "budgeted memory requires wrapper-preserving shared detachment".into(),
        })
    }

    unsafe fn do_wait(
        &mut self,
        dst: u32,
        expected: ExpectedValue,
        timeout: Option<Duration>,
    ) -> Result<u32, WaiterError> {
        // SAFETY: forwarded verbatim to the inner memory, whose contract we
        // inherit; `dst` validity/alignment is the caller's responsibility.
        wasmer::sys::vm::on_host_stack(|| unsafe { self.inner.do_wait(dst, expected, timeout) })
    }

    #[cfg(not(napi_standalone_legacy_wait))]
    unsafe fn do_wait_interruptible(
        &mut self,
        dst: u32,
        expected: ExpectedValue,
        timeout: Option<Duration>,
        store_id: StoreId,
    ) -> Result<u32, WaiterError> {
        // Preserve the store identity so force-stop can wake an infinite
        // atomic.wait registered by the underlying shared memory.
        wasmer::sys::vm::on_host_stack(|| unsafe {
            self.inner
                .do_wait_interruptible(dst, expected, timeout, store_id)
        })
    }

    fn do_notify(&mut self, dst: u32, count: u32) -> u32 {
        wasmer::sys::vm::on_host_stack(|| self.inner.do_notify(dst, count))
    }

    fn thread_conditions(&self) -> Option<&ThreadConditions> {
        self.inner.thread_conditions()
    }
}

/// [`Tunables`] that wrap every native memory in a [`BudgetedMemory`] and clamp a
/// requested memory's maximum to what the budget could ever grant.
///
/// All other logic delegates to the wrapped base tunables. Both imported and
/// module-defined memories reserve their initial pages before the backend maps
/// them. The wrapped backend must initialize with at most the requested
/// minimum; a larger actual size is rejected and the memory is dropped.
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
        let reserved = pages_to_bytes(adjusted.minimum);
        let charge = MemoryCharge::new(Arc::clone(&self.budget), reserved)
            .map_err(over_budget_to_memory_error)?;
        let inner = self.base.create_host_memory(&adjusted, style)?;
        let budgeted = BudgetedMemory::from_precharged(inner, charge)?;
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
        let reserved = pages_to_bytes(adjusted.minimum);
        let charge = MemoryCharge::new(Arc::clone(&self.budget), reserved)
            .map_err(over_budget_to_memory_error)?;
        // SAFETY: contract forwarded to base; `vm_definition_location` validity
        // is the caller's responsibility.
        let inner = unsafe {
            self.base
                .create_vm_memory(&adjusted, style, vm_definition_location)
        }?;
        let budgeted = BudgetedMemory::from_precharged(inner, charge)?;
        Ok(VMMemory::from(
            Box::new(budgeted) as Box<dyn LinearMemory + Send + Sync + 'static>
        ))
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
    #[cfg(napi_standalone_legacy_wait)]
    use wasmer::MemoryLocation;
    use wasmer::sys::{Cranelift, EngineBuilder};
    use wasmer::{Imports, Instance, Memory, MemoryType, Module, Pages, Store, WASM_PAGE_SIZE};

    const PAGE: u64 = WASM_PAGE_SIZE as u64;

    #[cfg(not(napi_standalone_legacy_wait))]
    #[derive(Debug)]
    struct InterruptWaitProbe {
        inner: VMMemory,
        seen: Arc<AtomicBool>,
        expected_store: StoreId,
    }

    #[cfg(not(napi_standalone_legacy_wait))]
    impl LinearMemory for InterruptWaitProbe {
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
            self.inner.grow(delta)
        }

        fn vmmemory(&self) -> std::ptr::NonNull<VMMemoryDefinition> {
            self.inner.vmmemory()
        }

        fn try_clone(&self) -> Result<Box<dyn LinearMemory + Send + Sync>, MemoryError> {
            Ok(Box::new(Self {
                inner: self.inner.try_clone()?,
                seen: Arc::clone(&self.seen),
                expected_store: self.expected_store,
            }))
        }

        fn copy(&self) -> Result<Box<dyn LinearMemory + Send + Sync>, MemoryError> {
            Ok(Box::new(Self {
                inner: VMMemory::from(self.inner.copy()?),
                seen: Arc::clone(&self.seen),
                expected_store: self.expected_store,
            }))
        }

        unsafe fn do_wait(
            &mut self,
            _dst: u32,
            _expected: ExpectedValue,
            _timeout: Option<Duration>,
        ) -> Result<u32, WaiterError> {
            panic!("interruptible wait must not be downgraded to a plain wait")
        }

        unsafe fn do_wait_interruptible(
            &mut self,
            _dst: u32,
            _expected: ExpectedValue,
            _timeout: Option<Duration>,
            store_id: StoreId,
        ) -> Result<u32, WaiterError> {
            assert_eq!(store_id, self.expected_store);
            self.seen.store(true, Ordering::Release);
            Ok(7)
        }
    }

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
        loop {
            match memory.grow(&mut store, Pages(1)) {
                Ok(_) => pages += 1,
                Err(_) => break,
            }
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
    fn module_defined_memories_use_the_same_quota() {
        let budget = ResourceBudget::with_memory_limit(2 * PAGE);
        let mut store = budgeted_store(Arc::clone(&budget));
        let wasm = wat::parse_str(r#"(module (memory (export "memory") 1 2))"#).unwrap();
        let module = Module::new(&store, wasm).unwrap();
        let first = Instance::new(&mut store, &module, &Imports::new()).unwrap();
        let second = Instance::new(&mut store, &module, &Imports::new()).unwrap();
        assert_eq!(budget.snapshot().wasm_linear, 2 * PAGE);
        let memory = first.exports.get_memory("memory").unwrap();
        assert!(memory.grow(&mut store, Pages(1)).is_err());
        assert!(Instance::new(&mut store, &module, &Imports::new()).is_err());
        assert_eq!(budget.snapshot().wasm_linear, 2 * PAGE);
        drop((first, second, store));
        assert_eq!(budget.snapshot().wasm_linear, 0);
    }

    // Public standalone Wasmer detaches copies through a raw shared-memory
    // handle. Finite-budget copies fail closed there; this quota-preserving
    // copy test applies to the newer wrapper-preserving managed Wasmer API.
    #[cfg(not(napi_standalone_legacy_wait))]
    #[test]
    fn reset_keeps_backing_charged_and_copy_reserves_it() {
        let budget = ResourceBudget::with_memory_limit(2 * PAGE);
        let mut store = budgeted_store(Arc::clone(&budget));
        let memory = Memory::new(&mut store, MemoryType::new(1, Some(2), true)).unwrap();
        memory.reset(&mut store).unwrap();
        assert_eq!(memory.size(&store), Pages(0));
        assert_eq!(budget.snapshot().wasm_linear, PAGE);

        let copied = memory.copy(&store).unwrap();
        assert_eq!(budget.snapshot().wasm_linear, 2 * PAGE);
        assert!(memory.copy(&store).is_err());
        assert_eq!(budget.snapshot().wasm_linear, 2 * PAGE);
        drop(copied);
        assert_eq!(budget.snapshot().wasm_linear, PAGE);

        memory.grow(&mut store, Pages(1)).unwrap();
        assert_eq!(budget.snapshot().wasm_linear, PAGE);
        memory.grow_at_least(&mut store, 2 * PAGE).unwrap();
        assert_eq!(budget.snapshot().wasm_linear, 2 * PAGE);
        drop(store);
        assert_eq!(budget.snapshot().wasm_linear, 0);
    }

    #[test]
    fn direct_raw_shared_detachment_is_rejected() {
        let budget = ResourceBudget::with_memory_limit(PAGE);
        let ty = MemoryType::new(1, Some(1), true);
        let base = BaseTunables::new();
        let style = base.memory_style(&ty);
        let inner = base.create_host_memory(&ty, &style).unwrap();
        let wrapped = BudgetedMemory::new(inner, Arc::clone(&budget)).unwrap();
        assert!(LinearMemory::as_shared(&wrapped).is_err());
        assert_eq!(budget.snapshot().wasm_linear, PAGE);
        drop(wrapped);
        assert_eq!(budget.snapshot().wasm_linear, 0);

        #[cfg(napi_standalone_legacy_wait)]
        {
            let mut store = budgeted_store(Arc::clone(&budget));
            let memory = Memory::new(&mut store, ty).unwrap();
            assert!(
                memory.as_shared(&store).is_none(),
                "legacy Wasmer must reject raw detachment under a finite quota"
            );
        }
    }

    #[cfg(napi_standalone_legacy_wait)]
    #[test]
    fn standalone_unlimited_budget_keeps_wasix_shared_memory_detachable() {
        let budget = ResourceBudget::unlimited();
        let mut store = budgeted_store(Arc::clone(&budget));
        let memory = Memory::new(&mut store, MemoryType::new(1, Some(2), true)).unwrap();
        let shared = memory
            .as_shared(&store)
            .expect("legacy Wasmer must be able to detach WASIX pthread memory");
        assert_eq!(
            shared
                .wait(MemoryLocation::new_32(0), Some(Duration::ZERO))
                .unwrap(),
            2,
        );
        let mut worker_store = Store::new(store.engine().clone());
        let attached = shared.attach(&mut worker_store);
        assert_eq!(attached.size(&worker_store), Pages(1));
    }

    #[cfg(not(napi_standalone_legacy_wait))]
    #[test]
    fn interruptible_wait_preserves_the_store_identity() {
        let budget = ResourceBudget::with_memory_limit(PAGE);
        let ty = MemoryType::new(1, Some(1), true);
        let base = BaseTunables::new();
        let style = base.memory_style(&ty);
        let inner = base.create_host_memory(&ty, &style).unwrap();
        let seen = Arc::new(AtomicBool::new(false));
        let store_id = StoreId::default();
        let probe = InterruptWaitProbe {
            inner,
            seen: Arc::clone(&seen),
            expected_store: store_id,
        };
        let mut memory = BudgetedMemory::new(
            VMMemory::from(Box::new(probe) as Box<dyn LinearMemory + Send + Sync>),
            budget,
        )
        .unwrap();
        let result = unsafe {
            LinearMemory::do_wait_interruptible(&mut memory, 0, ExpectedValue::None, None, store_id)
        }
        .unwrap();
        assert_eq!(result, 7);
        assert!(seen.load(Ordering::Acquire));
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

    #[test]
    fn near_heap_limit_callback_grants_until_budget_exhausted() {
        let step = DEFAULT_HEAP_GROW_STEP as usize;
        // The unwind slack is pre-reserved as part of the env ceiling, with room
        // for exactly two additional grow-step grants.
        let budget =
            ResourceBudget::with_memory_limit(DEFAULT_UNWIND_SLACK + 2 * DEFAULT_HEAP_GROW_STEP);
        budget
            .try_charge(Pool::V8HeapReserved, DEFAULT_UNWIND_SLACK)
            .unwrap();
        let host_stopped = Arc::new(AtomicBool::new(false));
        let ptr = Box::into_raw(Box::new(EnvHeapCharge {
            budget: Arc::clone(&budget),
            env: 0,
            host_stopped: Arc::clone(&host_stopped),
            emergency_exposed: AtomicU64::new(0),
            granted: AtomicU64::new(0),
            bookkeeping_granted: AtomicU64::new(0),
        }));
        let data = ptr as *const c_void;
        let base = 100 * 1024 * 1024usize;
        let emergency = (DEFAULT_UNWIND_SLACK + DEFAULT_HEAP_EMERGENCY_HEADROOM) as usize;

        // Each of the first two grants raises the limit by a step and charges it.
        assert_eq!(
            napi_host_near_heap_limit_grant(data, base, base, 0),
            base + step
        );
        assert_eq!(
            napi_host_near_heap_limit_grant(data, base + step, base, 0),
            base + 2 * step
        );
        assert_eq!(
            budget.snapshot().v8_heap_reserved,
            DEFAULT_UNWIND_SLACK + 2 * DEFAULT_HEAP_GROW_STEP
        );

        // Budget exhaustion requests termination and exposes the already-reserved
        // unwind slack plus the emergency headroom exactly once; the headroom
        // is recorded outside the budget, not charged to it.
        assert_eq!(
            napi_host_near_heap_limit_grant(data, base + 2 * step, base, 0),
            base + 2 * step + emergency
        );
        assert_eq!(
            napi_host_near_heap_limit_grant(data, base + 2 * step + emergency, base, 0),
            base + 2 * step + emergency,
            "a second refusal must not expand the limit again"
        );
        let usage = budget.snapshot();
        assert_eq!(
            usage.v8_heap_reserved,
            DEFAULT_UNWIND_SLACK + 2 * DEFAULT_HEAP_GROW_STEP
        );
        assert_eq!(usage.v8_heap_emergency, emergency as u64);
        assert_eq!(usage.mem_charged, usage.v8_heap_reserved);
        assert!(heap_emergency_stats().grants >= 1);
        assert!(heap_emergency_stats().exhausted >= 1);

        assert!(host_stopped.load(Ordering::Acquire));

        // The tracker recorded exactly what was granted, and releasing it (as
        // env teardown does) returns the pool to zero.
        let tracker = unsafe { Box::from_raw(ptr) };
        let granted = tracker.granted.load(Ordering::Acquire);
        assert_eq!(granted, 2 * DEFAULT_HEAP_GROW_STEP);
        budget.uncharge(Pool::V8HeapReserved, granted + DEFAULT_UNWIND_SLACK);
        budget.release_heap_emergency(tracker.emergency_exposed.load(Ordering::Acquire));
        let usage = budget.snapshot();
        assert_eq!(usage.v8_heap_reserved, 0);
        assert_eq!(usage.v8_heap_emergency, 0);
    }

    #[test]
    fn emergency_headroom_covers_the_largest_v8_heap_object() {
        // V8 caps FixedArray/FixedDoubleArray at 128 Mi entries and strings at
        // String::kMaxLength two-byte characters: both are 1 GiB objects. A
        // single refused allocation of that size has to fit into what one
        // callback exposes, or V8 aborts the process on its retry.
        let largest_object = 1024 * MIB;
        assert!(DEFAULT_HEAP_EMERGENCY_HEADROOM > largest_object);
        let budget = ResourceBudget::with_memory_limit(DEFAULT_UNWIND_SLACK);
        budget
            .try_charge(Pool::V8HeapReserved, DEFAULT_UNWIND_SLACK)
            .unwrap();
        let ptr = Box::into_raw(Box::new(EnvHeapCharge {
            budget: Arc::clone(&budget),
            env: 0,
            host_stopped: Arc::new(AtomicBool::new(false)),
            emergency_exposed: AtomicU64::new(0),
            granted: AtomicU64::new(0),
            bookkeeping_granted: AtomicU64::new(0),
        }));
        let base = 64 * MIB as usize;
        let raised = napi_host_near_heap_limit_grant(ptr as *const c_void, base, base, 0);
        assert!(raised >= base + largest_object as usize);
        let tracker = unsafe { Box::from_raw(ptr) };
        budget.release_heap_emergency(tracker.emergency_exposed.load(Ordering::Acquire));
        assert_eq!(budget.snapshot().v8_heap_emergency, 0);
    }

    #[test]
    fn emergency_headroom_is_configurable_per_budget() {
        let budget = ResourceBudget::with_memory_limit(DEFAULT_UNWIND_SLACK);
        budget.set_heap_emergency_headroom(3 * MIB);
        budget
            .try_charge(Pool::V8HeapReserved, DEFAULT_UNWIND_SLACK)
            .unwrap();
        let ptr = Box::into_raw(Box::new(EnvHeapCharge {
            budget: Arc::clone(&budget),
            env: 0,
            host_stopped: Arc::new(AtomicBool::new(false)),
            emergency_exposed: AtomicU64::new(0),
            granted: AtomicU64::new(0),
            bookkeeping_granted: AtomicU64::new(0),
        }));
        let base = 64 * MIB as usize;
        assert_eq!(
            napi_host_near_heap_limit_grant(ptr as *const c_void, base, base, 0),
            base + (DEFAULT_UNWIND_SLACK + 3 * MIB) as usize
        );
        assert_eq!(
            budget.snapshot().v8_heap_emergency,
            DEFAULT_UNWIND_SLACK + 3 * MIB
        );
        drop(unsafe { Box::from_raw(ptr) });
    }

    #[test]
    fn near_heap_limit_callback_ignores_null_data() {
        assert_eq!(
            napi_host_near_heap_limit_grant(std::ptr::null(), 42, 7, 0),
            42,
            "a null tracker leaves the limit unchanged"
        );
    }

    #[test]
    fn bookkeeping_grants_until_budget_exhausted_then_stops_the_env() {
        let budget = ResourceBudget::with_memory_limit(3 * MIB);
        let host_stopped = Arc::new(AtomicBool::new(false));
        let ptr = Box::into_raw(Box::new(EnvHeapCharge {
            budget: Arc::clone(&budget),
            env: 0,
            host_stopped: Arc::clone(&host_stopped),
            emergency_exposed: AtomicU64::new(0),
            granted: AtomicU64::new(0),
            bookkeeping_granted: AtomicU64::new(0),
        }));
        let data = ptr as *const c_void;

        assert_eq!(napi_host_bookkeeping_charge(data, 2 * MIB), 1);
        assert_eq!(budget.snapshot().host_bookkeeping, 2 * MIB);
        assert!(!host_stopped.load(Ordering::Acquire));

        // Over budget: denied, nothing charged, and the env is stopped.
        assert_eq!(napi_host_bookkeeping_charge(data, 2 * MIB), 0);
        assert_eq!(budget.snapshot().host_bookkeeping, 2 * MIB);
        assert!(host_stopped.load(Ordering::Acquire));

        // Returns are clamped to what was granted, so the pool cannot underflow.
        napi_host_bookkeeping_uncharge(data, MIB);
        napi_host_bookkeeping_uncharge(data, 10 * MIB);
        assert_eq!(budget.snapshot().host_bookkeeping, 0);

        // Teardown releases whatever the bridge still held.
        assert_eq!(napi_host_bookkeeping_charge(data, MIB), 1);
        let tracker = unsafe { Box::from_raw(ptr) };
        budget.uncharge(
            Pool::HostBookkeeping,
            tracker.bookkeeping_granted.load(Ordering::Acquire),
        );
        assert_eq!(budget.snapshot().host_bookkeeping, 0);

        // A null tracker (unbudgeted env) always grants.
        assert_eq!(napi_host_bookkeeping_charge(std::ptr::null(), MIB), 1);
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

    #[cfg(all(napi_standalone_legacy_wait, not(target_arch = "wasm32")))]
    #[test]
    fn external_accountant_cannot_use_legacy_raw_detachment() {
        let accountant = TestAccountant::new(UNLIMITED);
        let external: Arc<dyn NapiMemoryAccountant> = accountant.clone();
        let budget = ResourceBudget::with_accountant(external);
        let ty = MemoryType::new(1, Some(1), true);
        let base = BaseTunables::new();
        let inner = base
            .create_host_memory(&ty, &base.memory_style(&ty))
            .unwrap();
        let wrapped = BudgetedMemory::new(inner, budget).unwrap();

        assert!(LinearMemory::as_shared(&wrapped).is_err());
        drop(wrapped);
        assert_eq!(accountant.memory_charged(), 0);
    }

    #[test]
    fn grant_covers_an_old_generation_already_over_the_limit() {
        // V8 admits the first large young object regardless of the limit and
        // promotes it on the next full collection; the callback then has one
        // answer to cover the committed old generation or the process aborts.
        let budget = ResourceBudget::with_memory_limit(DEFAULT_UNWIND_SLACK + 512 * MIB);
        budget
            .try_charge(Pool::V8HeapReserved, DEFAULT_UNWIND_SLACK)
            .unwrap();
        let ptr = Box::into_raw(Box::new(EnvHeapCharge {
            budget: Arc::clone(&budget),
            env: 0,
            host_stopped: Arc::new(AtomicBool::new(false)),
            emergency_exposed: AtomicU64::new(0),
            granted: AtomicU64::new(0),
            bookkeeping_granted: AtomicU64::new(0),
        }));
        let data = ptr as *const c_void;
        let limit = 64 * MIB as usize;
        let committed = limit + 120 * MIB as usize;
        let raised = napi_host_near_heap_limit_grant(data, limit, limit, committed);
        assert!(raised >= committed, "{raised} does not cover {committed}");
        assert_eq!(raised, committed + DEFAULT_HEAP_GROW_STEP as usize);
        assert_eq!(
            budget.snapshot().v8_heap_reserved,
            DEFAULT_UNWIND_SLACK + 120 * MIB + DEFAULT_HEAP_GROW_STEP
        );
        assert_eq!(budget.snapshot().v8_heap_emergency, 0);
        drop(unsafe { Box::from_raw(ptr) });
    }

    #[test]
    fn refusal_covers_an_overshoot_larger_than_the_headroom() {
        let budget = ResourceBudget::with_memory_limit(DEFAULT_UNWIND_SLACK);
        budget.set_heap_emergency_headroom(MIB);
        budget
            .try_charge(Pool::V8HeapReserved, DEFAULT_UNWIND_SLACK)
            .unwrap();
        let host_stopped = Arc::new(AtomicBool::new(false));
        let ptr = Box::into_raw(Box::new(EnvHeapCharge {
            budget: Arc::clone(&budget),
            env: 0,
            host_stopped: Arc::clone(&host_stopped),
            emergency_exposed: AtomicU64::new(0),
            granted: AtomicU64::new(0),
            bookkeeping_granted: AtomicU64::new(0),
        }));
        let data = ptr as *const c_void;
        let limit = 64 * MIB as usize;
        let committed = limit + 300 * MIB as usize;
        let raised = napi_host_near_heap_limit_grant(data, limit, limit, committed);
        assert!(host_stopped.load(Ordering::Acquire));
        assert_eq!(raised, committed + DEFAULT_UNWIND_SLACK as usize);
        assert_eq!(
            budget.snapshot().v8_heap_emergency,
            300 * MIB + DEFAULT_UNWIND_SLACK
        );
        drop(unsafe { Box::from_raw(ptr) });
    }
}
