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
//! ## Install path
//!
//! The guest's linear memory is *imported* (host-created). wasmer's public API
//! does not let an embedder inject a custom [`LinearMemory`] into a
//! [`wasmer::Memory`] directly (the backend `VMMemory` enum is private), so the
//! charge is installed one level down, via custom [`Tunables`]: a
//! [`BudgetedTunables`] wraps [`BaseTunables`] and returns every host memory
//! wrapped in a [`BudgetedMemory`]. Installing those tunables on the engine
//! (see `cli.rs`) makes `Memory::new` — and therefore the guest's imported
//! memory — budget-aware with no change to the memory-creation call site.

use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
};
use std::time::Duration;

use wasmer::sys::vm::{
    ExpectedValue, LinearMemory, MemoryError, ThreadConditions, VMMemory, VMMemoryDefinition,
    VMSharedMemory, VMTable, VMTableDefinition, WaiterError,
};
use wasmer::sys::{BaseTunables, Tunables};
use wasmer::{MemoryStyle, MemoryType, Pages, TableStyle, TableType, WASM_PAGE_SIZE};

/// Sentinel meaning "no memory limit". A budget built with this total tracks
/// charges for observability but never denies one.
const UNLIMITED: u64 = u64::MAX;

fn pages_to_bytes(pages: Pages) -> u64 {
    u64::from(pages.0) * WASM_PAGE_SIZE as u64
}

/// Round `bytes` up to a whole number of wasm pages, saturating.
fn round_up_to_page(bytes: u64) -> u64 {
    let page = WASM_PAGE_SIZE as u64;
    bytes.div_ceil(page).saturating_mul(page)
}

/// A distinct byte pool metered against the budget.
///
/// Phase 1 charges only [`Pool::WasmLinear`]; later phases add the V8 heap,
/// external memory, and host-transient pools.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Pool {
    /// Guest wasm linear memory (wasmer `WasmMmap`).
    WasmLinear,
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
    /// Sum of all currently-charged bytes across every pool.
    pub mem_charged: u64,
    /// Currently-charged guest wasm linear memory bytes.
    pub wasm_linear: u64,
}

/// One shared accountant per app, `Arc`-shared into every pool that allocates.
///
/// Charging is **reserve-based**: bytes are charged before (or exactly at) the
/// moment they become live, so `actual usage <= mem_charged <= mem_total`
/// always holds and enforcement never races a garbage collector. All state is
/// atomic, so worker threads (each its own store + isolate) share one budget.
#[derive(Debug)]
pub struct ResourceBudget {
    /// Total byte budget. `UNLIMITED` disables enforcement (tracking only).
    mem_total: u64,
    /// Sum of all currently-charged bytes across every pool.
    mem_charged: AtomicU64,
    /// Per-pool charge, for observability and reconciliation.
    wasm_linear: AtomicU64,
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

    fn with_total(mem_total: u64) -> Self {
        Self {
            mem_total,
            mem_charged: AtomicU64::new(0),
            wasm_linear: AtomicU64::new(0),
        }
    }

    /// Whether this budget enforces a limit.
    pub fn is_unlimited(&self) -> bool {
        self.mem_total == UNLIMITED
    }

    /// The total memory budget (`u64::MAX` if unlimited).
    pub fn memory_limit(&self) -> u64 {
        self.mem_total
    }

    /// Bytes currently charged across all pools.
    pub fn memory_charged(&self) -> u64 {
        self.mem_charged.load(Ordering::Acquire)
    }

    /// Bytes still available before the budget is exhausted.
    pub fn memory_remaining(&self) -> u64 {
        self.mem_total.saturating_sub(self.memory_charged())
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

        if self.mem_total == UNLIMITED {
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
    }

    fn pool_counter(&self, pool: Pool) -> &AtomicU64 {
        match pool {
            Pool::WasmLinear => &self.wasm_linear,
        }
    }

    /// Snapshot the current charges for metrics / billing.
    pub fn snapshot(&self) -> ResourceUsage {
        ResourceUsage {
            mem_total: self.mem_total,
            mem_charged: self.mem_charged.load(Ordering::Acquire),
            wasm_linear: self.wasm_linear.load(Ordering::Acquire),
        }
    }
}

/// The live charge for one physical wasm-memory allocation.
///
/// Shared (`Arc`) between all [`BudgetedMemory`] handles that refer to the same
/// underlying mmap — cloning a shared memory shares this, so the charge is
/// released exactly once, when the last handle drops.
#[derive(Debug)]
struct MemoryCharge {
    budget: Arc<ResourceBudget>,
    /// Bytes currently charged for this allocation.
    bytes: AtomicU64,
}

impl MemoryCharge {
    fn new(budget: Arc<ResourceBudget>, bytes: u64) -> Result<Arc<Self>, OverBudget> {
        budget.try_charge(Pool::WasmLinear, bytes)?;
        Ok(Arc::new(Self {
            budget,
            bytes: AtomicU64::new(bytes),
        }))
    }
}

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
pub struct BudgetedMemory {
    inner: VMMemory,
    charge: Arc<MemoryCharge>,
}

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

fn over_budget_to_memory_error(err: OverBudget) -> MemoryError {
    MemoryError::Generic(err.to_string())
}

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
pub struct BudgetedTunables<T: Tunables> {
    base: T,
    budget: Arc<ResourceBudget>,
}

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
pub fn budgeted_tunables(
    target: &wasmer::sys::Target,
    budget: Arc<ResourceBudget>,
) -> BudgetedTunables<BaseTunables> {
    BudgetedTunables::new(BaseTunables::for_target(target), budget)
}

#[cfg(test)]
mod tests {
    use super::*;
    use wasmer::sys::{Cranelift, EngineBuilder};
    use wasmer::{Memory, MemoryType, Pages, Store, WASM_PAGE_SIZE};

    const PAGE: u64 = WASM_PAGE_SIZE as u64;

    /// A store whose engine charges guest wasm linear memory against `budget`.
    fn budgeted_store(budget: Arc<ResourceBudget>) -> Store {
        let mut engine = EngineBuilder::new(Cranelift::default()).engine();
        let tunables = budgeted_tunables(engine.target(), budget);
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
}
