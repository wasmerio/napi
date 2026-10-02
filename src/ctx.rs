use anyhow::{Context, Result, bail};
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
use std::sync::atomic::AtomicU32;
use std::{
    collections::HashSet,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
};
use wasmer::{Extern, ExternType, FunctionEnv, Imports, Instance, Module, StoreMut, Table, Value};

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
use crate::lane::ManagedV8LaneActivator;
use crate::{
    NAPI_EXTENSION_WASMER_MODULE_NAME, NAPI_EXTENSION_WASMER_MODULE_PREFIX, NAPI_MODULE_NAME,
    NapiEnv, NapiVersion, NapiWasmerExtensionVersion,
    budget::{NapiMemoryAccountant, ResourceBudget},
    guest::napi::{
        frozen_napi_type_matches, is_known_napi_import, register_env_imports, register_napi_imports,
    },
    message::PendingMessages,
};

#[derive(Debug, Clone, Default)]
pub struct NapiLimits {
    pub max_sessions: Option<usize>,
    pub max_envs: Option<usize>,
    /// Unified cross-VM memory budget (bytes): guest wasm linear memory plus,
    /// in later phases, V8 heap ceilings, external memory, and host transients.
    /// `None` means unlimited.
    pub total_memory_bytes: Option<u64>,
    pub max_total_external_memory: Option<u64>,
    pub max_total_heap_bytes: Option<u64>,
}

impl NapiLimits {
    /// The effective total memory budget, folding in the deprecated
    /// `max_total_heap_bytes` alias when the unified field is unset.
    fn memory_budget_bytes(&self) -> Option<u64> {
        self.total_memory_bytes.or(self.max_total_heap_bytes)
    }
}

/// Whether JavaScript in guest environments can use V8's `WebAssembly`.
///
/// Guest environments allocate V8 array buffers in the guest's linear memory,
/// where they count against the context's memory budget. V8 does not allocate
/// WebAssembly memories and compiled wasm code that way: they come from V8's
/// page allocator, outside the guest heap. The default therefore removes
/// `WebAssembly` from guest environments; [`WasmPolicy::EnabledMetered`]
/// exposes it with those allocations bounded and charged.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum WasmPolicy {
    /// `WebAssembly` is absent from the root context and from every `vm`
    /// context of a guest environment, and V8 refuses wasm code generation in
    /// them. This is the default.
    ///
    /// The restriction applies to environments backed by a guest heap, which
    /// is every environment a guest creates through this crate. (The
    /// provider's native C API leaves environments without a guest heap
    /// unrestricted, since their array buffers are not budgeted either.)
    #[default]
    Restricted,
    /// Expose V8's `WebAssembly` in guest environments **without wasm
    /// limits**. Unsupported for untrusted code.
    ///
    /// With this policy, guest JavaScript can make V8:
    ///
    /// * reserve address space for any number of wasm memories (by default
    ///   4 GiB for each 32-bit memory without a declared maximum) and commit
    ///   memory as they grow,
    /// * compile and commit executable wasm code, including on V8
    ///   background tasks (asynchronous compilation and tier-up),
    /// * use compiler working memory that grows with the module size.
    ///
    /// Nothing caps these and compiled code is not charged to
    /// [`NapiLimits::total_memory_bytes`], to a [`NapiMemoryAccountant`] or to
    /// any other limit of the context. (In a managed context, committed wasm
    /// memory pages are charged like other page-backed buffers.) Only the
    /// process-wide V8 flags bound it. Enabling V8's wasm compilers also adds
    /// them to the attack surface reachable from guest code.
    ///
    /// `vm` contexts follow their own `codeGeneration.wasm` option, and
    /// contexts not created by the provider (for example `ShadowRealm`
    /// realms) still refuse wasm code generation.
    ///
    /// Intended for trusted workloads and for evaluation.
    EnabledUnmetered,
    /// Expose V8's `WebAssembly` in guest environments with wasm memory and
    /// code bounded and charged to the context's budget.
    ///
    /// Requires a managed context ([`NapiCtxBuilder::build_managed_imports`]):
    /// accounting is attributed through the context's V8 lane. It also
    /// requires the process-wide engine limits ([`configure_wasm_engine`])
    /// before the first environment of the process is created. Without
    /// either, creating an environment fails.
    ///
    /// * Wasm memories: committed pages are charged softly to
    ///   [`Pool::V8WasmMemory`](crate::Pool::V8WasmMemory): a refused commit
    ///   makes `memory.grow` return `-1` and `new WebAssembly.Memory` throw a
    ///   `RangeError`; the application keeps running. Reserved address space
    ///   is not charged but bounded per memory
    ///   ([`WasmEngineLimits::max_memory_pages`]) and per context
    ///   ([`WasmLimits::max_memories`], [`WasmLimits::max_reserved_bytes`]);
    ///   a reservation over a cap is a `RangeError`.
    /// * Wasm code: committed code is charged to
    ///   [`Pool::V8WasmCode`](crate::Pool::V8WasmCode) after V8 committed it
    ///   (V8 cannot fail a code commit). If that charge is refused, or the
    ///   context's committed code exceeds [`WasmLimits::code_budget_bytes`],
    ///   the context is stopped like one whose heap growth was refused and
    ///   the embedder is told through
    ///   [`NapiMemoryAccountant::limit_exceeded`]. New compilations are
    ///   refused with a `CompileError` while the context or the process
    ///   ([`WasmEngineLimits::process_code_budget_bytes`]) is at its code
    ///   budget; a context whose commit takes the process past twice that
    ///   budget is stopped. A context can overshoot its code budget by the
    ///   function being compiled when it is crossed: up to about 38 MiB of
    ///   baseline code for a function at V8's size limit. Module size,
    ///   function count and table size are capped by the engine limits.
    ///
    /// Not charged: compiler working memory (bounded by the module size cap),
    /// V8's wasm metadata, and code pointer table entries (bounded by the
    /// function cap). Import wrapper code is shared by all contexts of the
    /// process; each wrapper page is charged to the context that first
    /// committed it while that context lives. `vm` contexts and realms behave
    /// as with [`WasmPolicy::EnabledUnmetered`].
    EnabledMetered(WasmLimits),
}

impl WasmPolicy {
    /// Value passed to the native bridge; must match `NapiWebAssemblyPolicy`
    /// in the provider's `restricted_context.h`.
    pub(crate) const fn bridge_code(self) -> u32 {
        match self {
            Self::Restricted => 0,
            Self::EnabledUnmetered => 1,
            Self::EnabledMetered(_) => 2,
        }
    }
}

/// Per-context limits of [`WasmPolicy::EnabledMetered`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub struct WasmLimits {
    /// Live wasm memories the context may have at once (default 64). Each
    /// memory is a few kernel memory mappings, so this bounds the context's
    /// share of the process's mappings.
    pub max_memories: u32,
    /// Address space all live wasm memories of the context may reserve
    /// (`None`, the default: 8 times the context's memory limit, unbounded
    /// for an unlimited budget). V8 retries a refused reservation with a
    /// smaller maximum, so memories still get created while it fits.
    pub max_reserved_bytes: Option<u64>,
    /// Committed wasm code past which the context is stopped (default
    /// 64 MiB).
    pub code_budget_bytes: u64,
}

impl Default for WasmLimits {
    fn default() -> Self {
        Self {
            max_memories: 64,
            max_reserved_bytes: None,
            code_budget_bytes: 64 * 1024 * 1024,
        }
    }
}

impl WasmLimits {
    /// The reserved-address-space cap for a context with `memory_limit`.
    pub(crate) fn reserved_bytes_cap(&self, memory_limit: u64) -> u64 {
        self.max_reserved_bytes
            .unwrap_or_else(|| memory_limit.saturating_mul(8))
    }
}

/// Process-wide V8 WebAssembly limits, see [`configure_wasm_engine`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub struct WasmEngineLimits {
    /// Maximum size of one wasm memory in 64 KiB pages, for 32- and 64-bit
    /// memories (default 16384, 1 GiB; at most 65536). V8 reserves address
    /// space for a memory's maximum up front, so this bounds every
    /// reservation, and `memory.grow` past it returns `-1`.
    pub max_memory_pages: u32,
    /// Maximum wasm module size in bytes (default 16 MiB, between 16 bytes
    /// and 1 GiB). Larger modules are refused with a `RangeError` before
    /// compilation. This also bounds compiler working memory.
    pub max_module_bytes: u64,
    /// Maximum functions per module (default 100,000, at most 1,000,000).
    pub max_functions: u32,
    /// Compile with the baseline compiler only (default `true`): no
    /// optimizing tier-up jobs on the context's background lane, whose
    /// compile time and memory are not bounded per function.
    pub liftoff_only: bool,
    /// Committed wasm code in the whole process past which new compilations
    /// in metered contexts are refused with a `CompileError` (default
    /// 1 GiB, between 1 MiB and 1.5 GiB). Lazy compilation of already
    /// admitted modules cannot be refused; a metered context whose code
    /// commit takes the process past twice this budget is stopped (see
    /// [`NapiLimitExceeded::WasmProcessCode`](crate::NapiLimitExceeded)).
    /// Both keep V8's own process-wide code limit (4095 MiB), whose breach
    /// aborts the process, out of reach.
    pub process_code_budget_bytes: u64,
    /// Maximum entries per wasm table (default 1,000,000, at most
    /// 10,000,000). Tables live on the V8 heap, where a single allocation
    /// larger than the heap's headroom is fatal for the process.
    pub max_table_size: u32,
}

impl Default for WasmEngineLimits {
    fn default() -> Self {
        Self {
            max_memory_pages: 16384,
            max_module_bytes: 16 * 1024 * 1024,
            max_functions: 100_000,
            liftoff_only: true,
            process_code_budget_bytes: 1024 * 1024 * 1024,
            max_table_size: 1_000_000,
        }
    }
}

/// Applies process-wide V8 WebAssembly limits; required for
/// [`WasmPolicy::EnabledMetered`].
///
/// V8 flags are process-wide and frozen when V8 initializes, which happens
/// when the first environment of the process is created, so call this
/// before that (for example at embedder startup). The limits apply to every
/// context of the process, whatever its policy, and they do not create the
/// V8 platform. Calling it again with the same limits succeeds; different
/// limits fail once V8 runs. Out-of-range values are rejected.
///
/// Also disables V8's process-wide cache of compiled wasm modules, which
/// would share code between contexts, and never enables V8's wasm trap
/// handler (bounds are checked explicitly) or streaming compilation.
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub fn configure_wasm_engine(limits: &WasmEngineLimits) -> Result<()> {
    let config = crate::snapi::SnapiWasmEngineConfig {
        size: std::mem::size_of::<crate::snapi::SnapiWasmEngineConfig>() as u32,
        max_memory_pages: limits.max_memory_pages,
        max_module_bytes: limits.max_module_bytes,
        max_functions: limits.max_functions,
        liftoff_only: u32::from(limits.liftoff_only),
        process_code_budget_bytes: limits.process_code_budget_bytes,
        max_table_size: limits.max_table_size,
        reserved: 0,
    };
    match unsafe { crate::snapi::snapi_v8_configure_wasm_engine(&config) } {
        0 => Ok(()),
        1 => bail!("invalid WebAssembly engine limits: {limits:?}"),
        _ => bail!("the V8 runtime already started with different WebAssembly engine limits"),
    }
}

#[derive(Default)]
pub struct NapiCtxBuilder {
    limits: NapiLimits,
    accountant: Option<Arc<dyn NapiMemoryAccountant>>,
    webassembly: WasmPolicy,
}

#[derive(Clone, Debug)]
pub struct NapiCtx {
    inner: Arc<NapiProviderBindings>,
}

#[derive(Clone)]
pub struct NapiSession {
    inner: Arc<NapiSessionInner>,
}

/// Opaque per-instantiation state returned by
/// [`NapiRuntimeHooks::additional_imports`] or [`NapiRuntimeHooks::add_imports`].
///
/// Pass it back, unmodified, to [`NapiRuntimeHooks::configure_instance`] for
/// the instance created with those imports.
pub struct NapiInstantiationState {
    session: Option<NapiSession>,
}

/// Runtime hooks that provide N-API imports for WASIX guests.
// The import phase creates a lightweight per-instantiation binding for the
// WASM host functions. V8 and the dedicated background lane start only when a
// guest invokes an env-creation import. Each binding stays tied to its store;
// the shared state below owns only instance-wide accounting and stop control.
#[derive(Clone, Debug)]
pub struct NapiRuntimeHooks {
    inner: Arc<NapiProviderBindings>,
}

/// Opaque control surface for stopping every V8 isolate owned by a context.
#[derive(Clone, Debug)]
pub struct NapiRuntimeControl {
    envs: Arc<Mutex<HashSet<usize>>>,
    host_stopped: Arc<AtomicBool>,
    pending_messages: Arc<PendingMessages>,
}

impl NapiRuntimeControl {
    #[cfg_attr(all(target_arch = "wasm32", feature = "js"), allow(dead_code))]
    pub(crate) fn new(
        envs: Arc<Mutex<HashSet<usize>>>,
        host_stopped: Arc<AtomicBool>,
        pending_messages: Arc<PendingMessages>,
    ) -> Self {
        Self {
            envs,
            host_stopped,
            pending_messages,
        }
    }

    /// Sets the sticky stop flag without touching the isolates: imports and
    /// isolates created later are refused. [`Self::terminate_all`] completes
    /// the stop.
    #[cfg_attr(all(target_arch = "wasm32", feature = "js"), allow(dead_code))]
    pub(crate) fn mark_stopped(&self) {
        self.host_stopped.store(true, Ordering::Release);
    }

    /// Permanently stop every currently-live V8 isolate owned by this context.
    ///
    /// Embedders call this both when the app exceeded its memory budget and
    /// when the instance was killed for an unrelated reason; N-API itself
    /// draws no distinction, since either way the app's JS must never run
    /// again. The stop is sticky: guest code cannot undo it with
    /// `napi_cancel_terminate_execution`, and isolates created afterwards
    /// stay terminated ([`NapiEnv::commit_isolate`] re-checks the flag under
    /// the same registry lock this holds).
    ///
    /// [`NapiEnv::commit_isolate`]: crate::env::NapiEnv::commit_isolate
    pub fn terminate_all(&self) {
        self.host_stopped.store(true, Ordering::Release);
        let envs = self.envs.lock().expect("poisoned N-API env registry");
        for env in envs.iter().copied() {
            unsafe {
                crate::snapi::snapi_bridge_unofficial_terminate_execution(
                    env as crate::snapi::SnapiEnv,
                );
            }
        }
        drop(envs);
        self.pending_messages.close_and_clear();
    }
}

/// Lightweight import bindings. Managed embedders construct these while
/// linking a module; no NapiCtx, V8 isolate, or background queue exists yet.
struct NapiProviderBindings {
    limits: NapiLimits,
    webassembly: WasmPolicy,
    active_sessions: Arc<AtomicUsize>,
    /// Guest-visible native env IDs span all worker sessions of this instance.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    next_native_env_id: Arc<AtomicU32>,
    /// One shared accountant per app, `Arc`-shared into the engine's budgeted
    /// tunables and the V8 heap, external-memory, and lane reservations.
    budget: Arc<ResourceBudget>,
    pending_messages: Arc<PendingMessages>,
    envs: Arc<Mutex<HashSet<usize>>>,
    host_stopped: Arc<AtomicBool>,
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    managed_lane_activator: Option<ManagedV8LaneActivator>,
}

impl std::fmt::Debug for NapiProviderBindings {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("NapiProviderBindings")
            .finish_non_exhaustive()
    }
}

struct NapiSessionInner {
    ctx: Arc<NapiProviderBindings>,
    lease: Arc<SessionLease>,
    imported_memory_type: Option<wasmer::MemoryType>,
    imported_table_type: Option<wasmer::TableType>,
    func_env: Mutex<Option<FunctionEnv<NapiEnv>>>,
}

impl std::fmt::Debug for NapiSession {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("NapiSession").finish_non_exhaustive()
    }
}

/// One admission slot shared by the temporary import session and every
/// FunctionEnv it installs. The slot stays occupied for the store lifetime,
/// including after `configure_instance` consumes its setup state.
pub(crate) struct SessionLease {
    active_sessions: Arc<AtomicUsize>,
}

impl Drop for SessionLease {
    fn drop(&mut self) {
        self.active_sessions.fetch_sub(1, Ordering::AcqRel);
    }
}

impl NapiCtxBuilder {
    pub fn max_sessions(mut self, max_sessions: usize) -> Self {
        self.limits.max_sessions = Some(max_sessions);
        self
    }

    pub fn max_envs(mut self, max_envs: usize) -> Self {
        self.limits.max_envs = Some(max_envs);
        self
    }

    /// The unified cross-VM memory budget in bytes (see
    /// [`NapiLimits::total_memory_bytes`]).
    pub fn total_memory_bytes(mut self, bytes: u64) -> Self {
        self.limits.total_memory_bytes = Some(bytes);
        self
    }

    pub fn max_total_external_memory(mut self, bytes: u64) -> Self {
        self.limits.max_total_external_memory = Some(bytes);
        self
    }

    pub fn max_total_heap_bytes(mut self, bytes: u64) -> Self {
        self.limits.max_total_heap_bytes = Some(bytes);
        self
    }

    /// Delegate total-memory admission to the embedder.
    pub fn memory_accountant(mut self, accountant: Arc<dyn NapiMemoryAccountant>) -> Self {
        self.accountant = Some(accountant);
        self
    }

    /// Whether guest JavaScript can use V8's `WebAssembly` (default:
    /// [`WasmPolicy::Restricted`]). Prefer [`WasmPolicy::EnabledMetered`]; with
    /// [`WasmPolicy::EnabledUnmetered`] wasm memory and code are not bounded.
    ///
    /// Applies to every environment a guest creates after this context is
    /// built, including those of WASIX worker threads.
    pub fn webassembly(mut self, policy: WasmPolicy) -> Self {
        self.webassembly = policy;
        self
    }

    /// Build lightweight import bindings for a managed embedder. The embedder
    /// owns lazy instance activation and must return its managed V8 task queue
    /// when a guest first creates an environment. Importing functions does not
    /// invoke the callback or allocate a NapiCtx, V8 isolate, or queue.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub fn build_managed_imports(self, activator: ManagedV8LaneActivator) -> NapiRuntimeHooks {
        NapiRuntimeHooks {
            inner: self.build_inner(Some(activator)),
        }
    }

    pub fn build(self) -> NapiCtx {
        NapiCtx {
            inner: self.build_inner(None),
        }
    }

    fn build_inner(
        self,
        #[cfg(not(all(target_arch = "wasm32", feature = "js")))] managed_lane_activator: Option<
            ManagedV8LaneActivator,
        >,
        #[cfg(all(target_arch = "wasm32", feature = "js"))] _background: Option<()>,
    ) -> Arc<NapiProviderBindings> {
        let budget = match self.accountant {
            Some(accountant) => ResourceBudget::with_accountant(accountant),
            None => match self.limits.memory_budget_bytes() {
                Some(bytes) => ResourceBudget::with_memory_limit(bytes),
                None => ResourceBudget::unlimited(),
            },
        };
        let envs = Arc::new(Mutex::new(HashSet::new()));
        let host_stopped = Arc::new(AtomicBool::new(false));
        Arc::new(NapiProviderBindings {
            limits: self.limits,
            webassembly: self.webassembly,
            active_sessions: Arc::new(AtomicUsize::new(0)),
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            next_native_env_id: Arc::new(AtomicU32::new(1)),
            budget,
            pending_messages: PendingMessages::new(),
            envs,
            host_stopped,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            managed_lane_activator,
        })
    }
}

impl Default for NapiCtx {
    fn default() -> Self {
        Self::builder().build()
    }
}

impl NapiCtx {
    pub fn builder() -> NapiCtxBuilder {
        NapiCtxBuilder::default()
    }

    pub fn limits(&self) -> &NapiLimits {
        &self.inner.limits
    }

    /// The WebAssembly policy for this context's guest environments.
    pub fn webassembly_policy(&self) -> WasmPolicy {
        self.inner.webassembly
    }

    pub fn active_sessions(&self) -> usize {
        self.inner.active_sessions.load(Ordering::Acquire)
    }

    /// The app's shared resource accountant. Install its budgeted tunables on
    /// the engine backing this app's guest store so guest wasm linear memory is
    /// charged against it (see [`crate::budget::budgeted_tunables`]).
    pub fn budget(&self) -> Arc<ResourceBudget> {
        Arc::clone(&self.inner.budget)
    }

    pub fn prepare_module(&self, module: &Module) -> Result<NapiSession> {
        self.new_session(module)
    }

    pub fn module_needs_napi(
        module: &Module,
    ) -> (Option<NapiVersion>, Option<NapiWasmerExtensionVersion>) {
        let mut napi_version = None;
        let mut napi_extension_version = None;

        for import in module.imports() {
            if import.module() == NAPI_MODULE_NAME {
                napi_version = Some(match napi_version {
                    Some(NapiVersion::Unknown) => NapiVersion::Unknown,
                    _ if matches!(import.ty(), ExternType::Function(actual)
                        if frozen_napi_type_matches(import.name(), actual) == Some(false)) =>
                    {
                        NapiVersion::Unknown
                    }
                    _ if is_known_napi_import(import.name()) => NapiVersion::V10,
                    _ => NapiVersion::Unknown,
                });
                continue;
            }

            let Some(detected_extension_version) =
                napi_wasmer_extension_version_from_namespace(import.module())
            else {
                continue;
            };

            napi_extension_version = Some(match napi_extension_version {
                None => detected_extension_version,
                Some(existing) if existing == detected_extension_version => existing,
                Some(_) => NapiWasmerExtensionVersion::Unknown,
            });
        }

        (napi_version, napi_extension_version)
    }

    pub fn runtime_hooks(&self) -> NapiRuntimeHooks {
        NapiRuntimeHooks {
            inner: Arc::clone(&self.inner),
        }
    }

    pub fn runtime_control(&self) -> NapiRuntimeControl {
        NapiRuntimeControl::from_inner(&self.inner)
    }

    pub fn new_session(&self, module: &Module) -> Result<NapiSession> {
        new_session(&self.inner, module)
    }
}

fn new_session(ctx: &Arc<NapiProviderBindings>, module: &Module) -> Result<NapiSession> {
    let max_sessions = ctx.limits.max_sessions.unwrap_or(usize::MAX);
    if ctx
        .active_sessions
        .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
            (current < max_sessions).then(|| current + 1)
        })
        .is_err()
    {
        bail!("refusing to create more than {max_sessions} active N-API sessions");
    }
    let lease = Arc::new(SessionLease {
        active_sessions: Arc::clone(&ctx.active_sessions),
    });

    let imported_memory_type = module.imports().find_map(|import| {
        if import.module() == "env"
            && import.name() == "memory"
            && let ExternType::Memory(ty) = import.ty()
        {
            return Some(*ty);
        }
        None
    });

    let imported_table_type = module.imports().find_map(|import| {
        if import.module() == "env"
            && import.name() == "__indirect_function_table"
            && let ExternType::Table(ty) = import.ty()
        {
            return Some(*ty);
        }
        None
    });

    Ok(NapiSession {
        inner: Arc::new(NapiSessionInner {
            ctx: Arc::clone(ctx),
            lease,
            imported_memory_type,
            imported_table_type,
            func_env: Mutex::new(None),
        }),
    })
}

impl NapiRuntimeControl {
    fn from_inner(inner: &Arc<NapiProviderBindings>) -> Self {
        Self {
            envs: Arc::clone(&inner.envs),
            host_stopped: Arc::clone(&inner.host_stopped),
            pending_messages: Arc::clone(&inner.pending_messages),
        }
    }
}

impl NapiRuntimeHooks {
    /// Shared resource budget for the guest store and all V8 environments.
    pub fn budget(&self) -> Arc<ResourceBudget> {
        Arc::clone(&self.inner.budget)
    }

    pub fn runtime_control(&self) -> NapiRuntimeControl {
        NapiRuntimeControl::from_inner(&self.inner)
    }

    /// Creates N-API imports when `module` requests them.
    pub fn additional_imports(
        &self,
        module: &Module,
        store: &mut StoreMut<'_>,
    ) -> Result<(Imports, NapiInstantiationState)> {
        let mut imports = Imports::new();
        let state = self.add_imports(module, store, &mut imports)?;
        Ok((imports, state))
    }

    /// Merges N-API imports into an existing import object when needed.
    ///
    /// Embedders that create shared imports such as `env.memory` before their
    /// extension hooks run should use this entry point. N-API reuses those
    /// objects so the host functions and guest instance always observe the
    /// same memory and table.
    pub fn add_imports(
        &self,
        module: &Module,
        store: &mut StoreMut<'_>,
        imports: &mut Imports,
    ) -> Result<NapiInstantiationState> {
        let (napi_version, napi_extension_version) = NapiCtx::module_needs_napi(module);
        if napi_version.is_none() && napi_extension_version.is_none() {
            return Ok(NapiInstantiationState { session: None });
        }

        if let Some(version) = napi_version
            && !NapiVersion::V10.is_compatible_with(version)
        {
            bail!("unsupported N-API import version: {version:?}");
        }

        if let Some(version) = napi_extension_version
            && !NapiWasmerExtensionVersion::V0.is_compatible_with(version)
        {
            bail!("unsupported Wasmer N-API extension version: {version:?}");
        }

        let session = new_session(&self.inner, module)?;
        session.add_imports(store, imports)?;
        Ok(NapiInstantiationState {
            session: Some(session),
        })
    }

    /// Completes memory, table, and guest allocation wiring after
    /// instantiation.
    ///
    /// `state` must be the value returned by the
    /// [`Self::additional_imports`] call whose imports this instance was
    /// created with.
    pub fn configure_instance(
        &self,
        module: &Module,
        store: &mut StoreMut<'_>,
        instance: &Instance,
        imported_memory: Option<&wasmer::Memory>,
        state: NapiInstantiationState,
    ) -> Result<()> {
        let (napi_version, napi_extension_version) = NapiCtx::module_needs_napi(module);
        if napi_version.is_none() && napi_extension_version.is_none() {
            return Ok(());
        }

        let session = state.session.context(
            "missing N-API session for module instance setup \
             (the state was not created for this module's imports)",
        )?;
        session.configure_instance(store, instance, imported_memory)
    }
}

impl NapiSession {
    pub fn create_imports(&self, store: &mut StoreMut<'_>) -> Result<Imports> {
        let mut import_object = Imports::new();
        self.add_imports(store, &mut import_object)?;
        Ok(import_object)
    }

    fn add_imports(&self, store: &mut StoreMut<'_>, import_object: &mut Imports) -> Result<()> {
        register_env_imports(store, import_object);

        let mut napi_env = NapiEnv::new(
            Arc::clone(&self.inner.ctx.budget),
            Arc::clone(&self.inner.ctx.pending_messages),
            self.inner.ctx.limits.max_envs,
            Arc::clone(&self.inner.ctx.envs),
            Arc::clone(&self.inner.ctx.host_stopped),
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            Arc::clone(&self.inner.ctx.next_native_env_id),
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            self.inner.ctx.managed_lane_activator.clone(),
        );
        napi_env.webassembly = self.inner.ctx.webassembly;
        let func_env = FunctionEnv::new(store, napi_env);
        {
            let mut guard = self
                .inner
                .func_env
                .lock()
                .expect("poisoned NapiSession mutex");
            *guard = Some(func_env.clone());
        }
        register_napi_imports(store, &func_env, import_object);

        if let Some(memory_type) = self.inner.imported_memory_type {
            let memory = if let Some(existing) = import_object.get_export("env", "memory") {
                let Extern::Memory(memory) = existing else {
                    bail!("env.memory import for N-API module is not a memory");
                };
                memory
            } else {
                let memory = wasmer::Memory::new(&mut *store, memory_type)?;
                import_object.define("env", "memory", memory.clone());
                memory
            };
            func_env.as_mut(&mut *store).memory = Some(memory);
        }

        if let Some(table_type) = self.inner.imported_table_type {
            let table = if let Some(existing) =
                import_object.get_export("env", "__indirect_function_table")
            {
                let Extern::Table(table) = existing else {
                    bail!("env.__indirect_function_table import for N-API module is not a table");
                };
                table
            } else {
                let table = Table::new(&mut *store, table_type, Value::FuncRef(None))?;
                import_object.define("env", "__indirect_function_table", table.clone());
                table
            };
            func_env.as_mut(&mut *store).table = Some(table);
        }

        // Wasmer owns this FunctionEnv until its store is destroyed. Retain
        // the admission slot with it; the setup session is consumed by
        // configure_instance long before the guest module stops running.
        func_env.as_mut(&mut *store).session_lease = Some(Arc::clone(&self.inner.lease));

        Ok(())
    }

    pub fn configure_instance(
        &self,
        store: &mut StoreMut<'_>,
        instance: &Instance,
        imported_memory: Option<&wasmer::Memory>,
    ) -> Result<()> {
        let func_env = {
            let guard = self
                .inner
                .func_env
                .lock()
                .expect("poisoned NapiSession mutex");
            guard
                .clone()
                .context("missing runtime function env during instance setup")?
        };

        // Imports may already have bound a memory and a start section may
        // already have created its guest heap from it. Never replace that
        // memory with a different exported one in a multi-memory module.
        // Otherwise a module-defined export can be bound after instantiation
        // for a later first N-API call. Keep the guest heap lazy either way.
        if func_env.as_ref(&*store).memory.is_none() {
            let instance_memory = imported_memory
                .cloned()
                .or_else(|| instance.exports.get_memory("memory").ok().cloned());
            if let Some(memory) = instance_memory {
                func_env.as_mut(&mut *store).memory = Some(memory);
            }
        }

        // Keep import-only modules cheap, including those with no linear
        // memory. The first environment creation validates memory and installs
        // the allocator after Edge has admitted the lane. A start section may
        // already have installed its heap; never replace it here.

        // The browser backend cannot expose a native pointer into Wasmer's JS
        // Memory. Keep its established guest allocator: exported malloc owns
        // the bytes, and the JS bridge copies/aliases them through typed views.
        #[cfg(all(target_arch = "wasm32", feature = "js"))]
        {
            let malloc = ["unofficial_napi_guest_malloc", "malloc"]
                .into_iter()
                .find_map(|name| {
                    instance
                        .exports
                        .get_typed_function::<i32, i32>(&mut *store, name)
                        .ok()
                });
            func_env.as_mut(&mut *store).malloc_fn = malloc;
        }

        if let Ok(table) = instance.exports.get_table("__indirect_function_table") {
            func_env.as_mut(&mut *store).table = Some(table.clone());
        }
        Ok(())
    }
}

fn napi_wasmer_extension_version_from_namespace(
    namespace: &str,
) -> Option<NapiWasmerExtensionVersion> {
    if namespace == NAPI_EXTENSION_WASMER_MODULE_NAME {
        return Some(NapiWasmerExtensionVersion::V0);
    }

    let suffix = namespace.strip_prefix(NAPI_EXTENSION_WASMER_MODULE_PREFIX)?;
    Some(match suffix {
        "0" => NapiWasmerExtensionVersion::V0,
        _ => NapiWasmerExtensionVersion::Unknown,
    })
}

#[cfg(test)]
mod tests {
    use super::{NapiCtx, WasmLimits, WasmPolicy};
    use crate::{NapiVersion, NapiWasmerExtensionVersion};
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    use std::{
        sync::{
            Arc,
            atomic::{AtomicUsize, Ordering},
            mpsc,
        },
        thread,
        time::Duration,
    };
    use wasmer::{AsStoreMut, Instance, Module, Store};
    use wat::parse_str;

    const EMPTY_WASM_MODULE: &[u8] = b"\0asm\x01\0\0\0";

    #[test]
    fn runtime_stop_is_sticky_without_live_envs() {
        let ctx = NapiCtx::default();
        ctx.runtime_control().terminate_all();
        assert!(
            ctx.inner
                .host_stopped
                .load(std::sync::atomic::Ordering::Acquire)
        );
    }

    #[test]
    fn runtime_control_shares_stop_state_with_the_context() {
        // Edge holds a control surface for both the memory-budget callback and
        // the instance-kill path, so a stop requested through any clone must be
        // visible to every env the context later hands out.
        let ctx = NapiCtx::default();
        let control = ctx.runtime_control();
        let other_control = control.clone();

        assert!(
            !ctx.inner
                .host_stopped
                .load(std::sync::atomic::Ordering::Acquire)
        );

        other_control.terminate_all();

        assert!(
            ctx.inner
                .host_stopped
                .load(std::sync::atomic::Ordering::Acquire)
        );
    }

    #[test]
    fn webassembly_policy_defaults_to_restricted_and_reaches_store_envs() {
        assert_eq!(WasmPolicy::default(), WasmPolicy::Restricted);
        assert_eq!(
            NapiCtx::default().webassembly_policy(),
            WasmPolicy::Restricted
        );
        // Mirrors NapiWebAssemblyPolicy in restricted_context.h.
        assert_eq!(WasmPolicy::Restricted.bridge_code(), 0);
        assert_eq!(WasmPolicy::EnabledUnmetered.bridge_code(), 1);
        assert_eq!(
            WasmPolicy::EnabledMetered(WasmLimits::default()).bridge_code(),
            2
        );

        let mut store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
            (import "napi" "napi_get_undefined" (func (param i32 i32) (result i32)))
            (import "env" "memory" (memory 1 512))
        )"#,
        );
        for policy in [
            WasmPolicy::Restricted,
            WasmPolicy::EnabledUnmetered,
            WasmPolicy::EnabledMetered(WasmLimits::default()),
        ] {
            let hooks = NapiCtx::builder()
                .webassembly(policy)
                .build()
                .runtime_hooks();
            let (_imports, state) = hooks
                .additional_imports(&module, &mut store.as_store_mut())
                .unwrap();
            let env_policy = state
                .session
                .as_ref()
                .unwrap()
                .inner
                .func_env
                .lock()
                .unwrap()
                .as_ref()
                .unwrap()
                .as_ref(&store)
                .webassembly;
            assert_eq!(env_policy, policy);
        }
    }

    #[test]
    fn max_sessions_limit_is_enforced() {
        let store = Store::default();
        let module = Module::new(&store, EMPTY_WASM_MODULE).expect("empty wasm module compiles");
        let ctx = NapiCtx::builder().max_sessions(1).build();

        let first = ctx
            .prepare_module(&module)
            .expect("first session should be created");
        assert_eq!(ctx.active_sessions(), 1);
        assert!(ctx.prepare_module(&module).is_err());

        drop(first);
        assert_eq!(ctx.active_sessions(), 0);

        let _second = ctx
            .prepare_module(&module)
            .expect("session slot should be released after drop");
        assert_eq!(ctx.active_sessions(), 1);
    }

    #[test]
    fn configured_store_keeps_its_session_slot_until_drop() {
        let mut first_store = Store::default();
        let mut second_store = Store::new(first_store.engine().clone());
        let module = compile_wat(
            &first_store,
            r#"(module
                (import "napi" "napi_get_undefined" (func (param i32 i32) (result i32)))
                (import "env" "memory" (memory 1))
            )"#,
        );
        let ctx = NapiCtx::builder().max_sessions(1).build();
        let hooks = ctx.runtime_hooks();

        let (imports, state) = hooks
            .additional_imports(&module, &mut first_store.as_store_mut())
            .expect("first session should be admitted");
        let instance = Instance::new(&mut first_store, &module, &imports).unwrap();
        hooks
            .configure_instance(
                &module,
                &mut first_store.as_store_mut(),
                &instance,
                None,
                state,
            )
            .unwrap();
        assert_eq!(ctx.active_sessions(), 1);
        assert!(
            hooks
                .additional_imports(&module, &mut second_store.as_store_mut())
                .is_err(),
            "a live store must retain its session slot"
        );

        drop(instance);
        drop(imports);
        drop(first_store);
        assert_eq!(ctx.active_sessions(), 0);
        let (_imports, _state) = hooks
            .additional_imports(&module, &mut second_store.as_store_mut())
            .expect("dropping the first store releases the session slot");
    }

    #[test]
    fn failed_import_setup_releases_its_session_slot() {
        let mut store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi" "napi_get_undefined" (func (param i32 i32) (result i32)))
                (import "env" "memory" (memory 1))
            )"#,
        );
        let ctx = NapiCtx::builder().max_sessions(1).build();
        let hooks = ctx.runtime_hooks();
        let mut invalid = wasmer::Imports::new();
        invalid.define(
            "env",
            "memory",
            wasmer::Function::new_typed(&mut store, || 0_i32),
        );
        assert!(
            hooks
                .add_imports(&module, &mut store.as_store_mut(), &mut invalid)
                .is_err()
        );
        assert_eq!(ctx.active_sessions(), 0);

        let mut valid = wasmer::Imports::new();
        let state = hooks
            .add_imports(&module, &mut store.as_store_mut(), &mut valid)
            .expect("a failed setup must not consume the live-session limit");
        drop(state);
        assert_eq!(ctx.active_sessions(), 1);
        drop(valid);
        drop(store);
        assert_eq!(ctx.active_sessions(), 0);
    }

    fn compile_wat(store: &Store, wat: &str) -> Module {
        let wasm = parse_str(wat).expect("wat module parses");
        Module::new(store, wasm).expect("wat module compiles")
    }

    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn managed_hooks(
        spawns: Arc<AtomicUsize>,
        finished: mpsc::Sender<()>,
    ) -> (
        super::NapiRuntimeHooks,
        Arc<std::sync::Mutex<Option<Arc<crate::ManagedV8Lane>>>>,
    ) {
        let slot = Arc::new(std::sync::Mutex::new(None));
        let activator = {
            let slot = Arc::clone(&slot);
            Arc::new(move || {
                let mut guard = slot.lock().unwrap();
                if let Some(lane) = &*guard {
                    return Ok(Arc::clone(lane));
                }
                let lane =
                    crate::ManagedV8Lane::new(256, Arc::new(|| Box::new(())), Arc::new(|| {}))?;
                let worker_lane = Arc::clone(&lane);
                let finished = finished.clone();
                thread::Builder::new()
                    .name("test-v8-lane".into())
                    .spawn(move || {
                        worker_lane.run();
                        let _ = finished.send(());
                    })?;
                spawns.fetch_add(1, Ordering::SeqCst);
                *guard = Some(Arc::clone(&lane));
                Ok(lane)
            })
        };
        (NapiCtx::builder().build_managed_imports(activator), slot)
    }

    #[test]
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn lazy_hooks_start_lane_only_when_guest_calls_napi() {
        let spawns = Arc::new(AtomicUsize::new(0));
        let (finished_tx, finished_rx) = mpsc::channel();
        let (hooks, lane_slot) = managed_hooks(Arc::clone(&spawns), finished_tx);
        let mut store = Store::default();
        let import_only = compile_wat(
            &store,
            r#"(module
            (import "napi" "napi_get_undefined" (func (param i32 i32) (result i32)))
            (import "env" "memory" (memory 1 512))
        )"#,
        );
        let (imports, state) = hooks
            .additional_imports(&import_only, &mut store.as_store_mut())
            .unwrap();
        let memory = state
            .session
            .as_ref()
            .unwrap()
            .inner
            .func_env
            .lock()
            .unwrap()
            .as_ref()
            .unwrap()
            .as_ref(&store)
            .memory
            .clone()
            .unwrap();
        let pages_before = memory.size(&store);
        let charge_before = hooks.budget().snapshot().mem_charged;
        let instance = Instance::new(&mut store, &import_only, &imports).unwrap();
        hooks
            .configure_instance(
                &import_only,
                &mut store.as_store_mut(),
                &instance,
                None,
                state,
            )
            .unwrap();
        assert!(!lane_slot.lock().unwrap().is_some());
        assert_eq!(spawns.load(Ordering::SeqCst), 0);
        assert_eq!(memory.size(&store), pages_before);
        assert_eq!(hooks.budget().snapshot().mem_charged, charge_before);

        let invoked = compile_wat(
            &store,
            r#"(module
            (import "napi" "napi_wasm_init_env" (func $init (result i32)))
            (import "env" "memory" (memory 1 512))
            (func (export "invoke") (result i32) call $init)
        )"#,
        );
        let (imports, state) = hooks
            .additional_imports(&invoked, &mut store.as_store_mut())
            .unwrap();
        let instance = Instance::new(&mut store, &invoked, &imports).unwrap();
        hooks
            .configure_instance(&invoked, &mut store.as_store_mut(), &instance, None, state)
            .unwrap();
        assert_eq!(spawns.load(Ordering::SeqCst), 0);
        let invoke = instance
            .exports
            .get_typed_function::<(), i32>(&store, "invoke")
            .unwrap();
        assert!(invoke.call(&mut store).unwrap() > 0);
        assert!(lane_slot.lock().unwrap().is_some());
        assert_eq!(spawns.load(Ordering::SeqCst), 1);
        lane_slot.lock().unwrap().as_ref().unwrap().stop();
        finished_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("lane stops");
    }

    #[test]
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn legacy_flags_and_malformed_create_leave_lane_and_heap_lazy() {
        let spawns = Arc::new(AtomicUsize::new(0));
        let (finished_tx, _finished_rx) = mpsc::channel();
        let (hooks, lane_slot) = managed_hooks(Arc::clone(&spawns), finished_tx);
        let mut store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi" "unofficial_napi_set_flags_from_string"
                    (func $flags (param i32 i32) (result i32)))
                (import "napi" "unofficial_napi_create_env_with_options"
                    (func $create (param i32 i32 i32 i32) (result i32)))
                (import "env" "memory" (memory 1 512))
                (data (i32.const 64) "--js-source-phase-imports --harmony-import-attributes")
                (func (export "defaults") (result i32)
                    i32.const 64 i32.const 53 call $flags)
                (func (export "other_flags") (result i32)
                    i32.const 64 i32.const 8 call $flags)
                (func (export "bad_create") (result i32)
                    i32.const 8 i32.const 65528 i32.const 8 i32.const 12 call $create)
            )"#,
        );
        let (imports, state) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .unwrap();
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        hooks
            .configure_instance(&module, &mut store.as_store_mut(), &instance, None, state)
            .unwrap();
        let charged = hooks.budget().snapshot().mem_charged;
        let call = |store: &mut Store, name: &str| {
            instance
                .exports
                .get_typed_function::<(), i32>(&*store, name)
                .unwrap()
                .call(store)
                .unwrap()
        };
        assert_eq!(call(&mut store, "defaults"), 0);
        assert_eq!(call(&mut store, "other_flags"), 1);
        assert_eq!(call(&mut store, "bad_create"), 1);
        assert_eq!(spawns.load(Ordering::SeqCst), 0);
        assert!(lane_slot.lock().unwrap().is_none());
        assert_eq!(hooks.budget().snapshot().mem_charged, charged);
    }

    #[test]
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn runtime_options_accept_only_default_flags_and_leave_lane_lazy() {
        let spawns = Arc::new(AtomicUsize::new(0));
        let (finished_tx, _finished_rx) = mpsc::channel();
        let (hooks, lane_slot) = managed_hooks(Arc::clone(&spawns), finished_tx);
        let mut store = Store::default();
        // Runtime options are `{ size, version, engine_flags, engine_flags_length }`.
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
                    (func $configure (param i32) (result i32)))
                (import "env" "memory" (memory 1 512))
                (data (i32.const 64) "--js-source-phase-imports --harmony-import-attributes")
                (func $options (param $flags i32) (param $length i32) (result i32)
                    (i32.store (i32.const 16) (i32.const 16))
                    (i32.store (i32.const 20) (i32.const 1))
                    (i32.store (i32.const 24) (local.get $flags))
                    (i32.store (i32.const 28) (local.get $length))
                    (call $configure (i32.const 16)))
                (func (export "no_options") (result i32)
                    i32.const 0 call $configure)
                (func (export "no_flags") (result i32)
                    i32.const 0 i32.const 0 call $options)
                (func (export "defaults") (result i32)
                    i32.const 64 i32.const 53 call $options)
                (func (export "one_default") (result i32)
                    i32.const 64 i32.const 25 call $options)
                (func (export "other_flags") (result i32)
                    i32.const 64 i32.const 8 call $options)
                (func (export "too_long") (result i32)
                    i32.const 64 i32.const 65 call $options)
                (func (export "out_of_bounds") (result i32)
                    i32.const 33554400 i32.const 53 call $options)
            )"#,
        );
        let (imports, state) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .unwrap();
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        hooks
            .configure_instance(&module, &mut store.as_store_mut(), &instance, None, state)
            .unwrap();
        let charged = hooks.budget().snapshot().mem_charged;
        let call = |store: &mut Store, name: &str| {
            instance
                .exports
                .get_typed_function::<(), i32>(&*store, name)
                .unwrap()
                .call(store)
                .unwrap()
        };
        assert_eq!(call(&mut store, "no_options"), 0);
        assert_eq!(call(&mut store, "no_flags"), 0);
        assert_eq!(call(&mut store, "defaults"), 0);
        assert_eq!(call(&mut store, "one_default"), 0);
        assert_eq!(call(&mut store, "other_flags"), 1);
        assert_eq!(call(&mut store, "too_long"), 1);
        assert_eq!(call(&mut store, "out_of_bounds"), 1);
        assert_eq!(spawns.load(Ordering::SeqCst), 0);
        assert!(lane_slot.lock().unwrap().is_none());
        assert_eq!(hooks.budget().snapshot().mem_charged, charged);
    }

    #[test]
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn memoryless_import_only_module_stays_inert() {
        let spawns = Arc::new(AtomicUsize::new(0));
        let (finished_tx, _finished_rx) = mpsc::channel();
        let (hooks, lane_slot) = managed_hooks(Arc::clone(&spawns), finished_tx);
        let mut store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi" "napi_wasm_init_env" (func $init (result i32)))
                (func (export "invoke") (result i32) call $init)
            )"#,
        );
        let (imports, state) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .unwrap();
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        hooks
            .configure_instance(&module, &mut store.as_store_mut(), &instance, None, state)
            .unwrap();
        assert_eq!(spawns.load(Ordering::SeqCst), 0);
        assert!(lane_slot.lock().unwrap().is_none());
        assert_eq!(hooks.budget().snapshot().mem_charged, 0);

        // Actual N-API use fails without guest memory before activation.
        let invoke = instance
            .exports
            .get_typed_function::<(), i32>(&store, "invoke")
            .unwrap();
        assert_eq!(invoke.call(&mut store).unwrap(), 0);
        assert_eq!(spawns.load(Ordering::SeqCst), 0);
        assert!(lane_slot.lock().unwrap().is_none());
    }

    #[test]
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn exported_memory_is_bound_without_initializing_napi() {
        let spawns = Arc::new(AtomicUsize::new(0));
        let (finished_tx, finished_rx) = mpsc::channel();
        let (hooks, lane_slot) = managed_hooks(Arc::clone(&spawns), finished_tx);
        let mut store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi" "napi_wasm_init_env" (func $init (result i32)))
                (memory (export "memory") 1 512)
                (func (export "invoke") (result i32) call $init)
            )"#,
        );
        let (imports, state) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .unwrap();
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        hooks
            .configure_instance(&module, &mut store.as_store_mut(), &instance, None, state)
            .unwrap();
        let memory = instance.exports.get_memory("memory").unwrap().clone();
        let pages_before = memory.size(&store);
        let charge_before = hooks.budget().snapshot().mem_charged;
        assert_eq!(spawns.load(Ordering::SeqCst), 0);
        assert!(lane_slot.lock().unwrap().is_none());
        assert_eq!(memory.size(&store), pages_before);
        assert_eq!(hooks.budget().snapshot().mem_charged, charge_before);

        let invoke = instance
            .exports
            .get_typed_function::<(), i32>(&store, "invoke")
            .unwrap();
        assert!(invoke.call(&mut store).unwrap() > 0);
        assert_eq!(spawns.load(Ordering::SeqCst), 1);
        lane_slot.lock().unwrap().as_ref().unwrap().stop();
        finished_rx.recv_timeout(Duration::from_secs(5)).unwrap();
    }

    #[test]
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn managed_imports_bind_the_existing_wasix_memory_before_start() {
        let spawns = Arc::new(AtomicUsize::new(0));
        let (finished_tx, finished_rx) = mpsc::channel();
        let (hooks, lane_slot) = managed_hooks(Arc::clone(&spawns), finished_tx);
        let mut store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi" "napi_wasm_init_env" (func $init (result i32)))
                (import "env" "memory" (memory 1 512))
                (import "env" "uv_get_free_memory" (func $free_memory (result i64)))
                (memory (export "memory") 1 512)
                (global $result (mut i32) (i32.const 0))
                (func $start call $init global.set $result)
                (start $start)
                (func (export "start_result") (result i32) global.get $result)
                (func (export "helper_result") (result i64) call $free_memory)
            )"#,
        );
        let memory =
            wasmer::Memory::new(&mut store, wasmer::MemoryType::new(1, Some(512), false)).unwrap();
        let mut imports = wasmer::Imports::new();
        imports.define("env", "memory", memory.clone());
        imports.define(
            "env",
            "uv_get_free_memory",
            wasmer::Function::new_typed(&mut store, || 123_i64),
        );
        let state = hooks
            .add_imports(&module, &mut store.as_store_mut(), &mut imports)
            .unwrap();
        let func_env = state
            .session
            .as_ref()
            .unwrap()
            .inner
            .func_env
            .lock()
            .unwrap()
            .as_ref()
            .unwrap()
            .clone();
        let bound_memory = func_env.as_ref(&store).memory.clone().unwrap();
        assert_eq!(
            bound_memory.view(&store).data_ptr(),
            memory.view(&store).data_ptr()
        );
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        assert_ne!(
            instance
                .exports
                .get_memory("memory")
                .unwrap()
                .view(&store)
                .data_ptr(),
            memory.view(&store).data_ptr(),
            "the exported memory must be distinct from imported env.memory"
        );
        hooks
            .configure_instance(&module, &mut store.as_store_mut(), &instance, None, state)
            .unwrap();
        assert!(func_env.as_ref(&store).guest_heap.is_some());
        assert_eq!(
            func_env
                .as_ref(&store)
                .memory
                .as_ref()
                .unwrap()
                .view(&store)
                .data_ptr(),
            memory.view(&store).data_ptr(),
            "configure_instance must retain the memory used by the start-section heap"
        );
        assert!(
            instance
                .exports
                .get_typed_function::<(), i32>(&store, "start_result")
                .unwrap()
                .call(&mut store)
                .unwrap()
                > 0
        );
        assert_eq!(
            instance
                .exports
                .get_typed_function::<(), i64>(&store, "helper_result")
                .unwrap()
                .call(&mut store)
                .unwrap(),
            123
        );
        assert_eq!(spawns.load(Ordering::SeqCst), 1);
        drop(imports);
        drop(instance);
        drop(store);
        lane_slot.lock().unwrap().as_ref().unwrap().stop();
        finished_rx.recv_timeout(Duration::from_secs(5)).unwrap();
    }

    #[test]
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn start_section_initialization_requires_a_guest_heap() {
        let spawns = Arc::new(AtomicUsize::new(0));
        let (finished_tx, finished_rx) = mpsc::channel();
        let (hooks, lane_slot) = managed_hooks(Arc::clone(&spawns), finished_tx);
        let mut store = Store::default();

        // A module-defined memory cannot be reached by imports until after
        // instantiation, so its start section must fail before V8 starts.
        let unavailable = compile_wat(
            &store,
            r#"(module
            (import "napi" "napi_wasm_init_env" (func $init (result i32)))
            (memory 1 512)
            (global $result (mut i32) (i32.const -1))
            (func $start call $init global.set $result)
            (start $start)
            (func (export "start_result") (result i32) global.get $result)
        )"#,
        );
        let (imports, _state) = hooks
            .additional_imports(&unavailable, &mut store.as_store_mut())
            .unwrap();
        let instance = Instance::new(&mut store, &unavailable, &imports).unwrap();
        let result = instance
            .exports
            .get_typed_function::<(), i32>(&store, "start_result")
            .unwrap();
        assert_eq!(result.call(&mut store).unwrap(), 0);
        assert_eq!(spawns.load(Ordering::SeqCst), 0);
        assert!(!lane_slot.lock().unwrap().is_some());

        let unavailable_extension = compile_wat(
            &store,
            r#"(module
            (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
              (func $create (param i32 i32 i32 i32) (result i32)))
            (memory 1 512)
            (global $result (mut i32) (i32.const -1))
            (func $start
              i32.const 8 i32.const 0 i32.const 4 i32.const 8
              call $create global.set $result)
            (start $start)
            (func (export "start_result") (result i32) global.get $result)
        )"#,
        );
        let (imports, _state) = hooks
            .additional_imports(&unavailable_extension, &mut store.as_store_mut())
            .unwrap();
        let instance = Instance::new(&mut store, &unavailable_extension, &imports).unwrap();
        let result = instance
            .exports
            .get_typed_function::<(), i32>(&store, "start_result")
            .unwrap();
        assert_ne!(result.call(&mut store).unwrap(), 0);
        assert_eq!(spawns.load(Ordering::SeqCst), 0);
        assert!(!lane_slot.lock().unwrap().is_some());

        // Imported memory is available during start. The first call installs
        // the guest heap before V8, and setup retains that same allocator.
        let available = compile_wat(
            &store,
            r#"(module
            (import "napi" "napi_wasm_init_env" (func $init (result i32)))
            (import "env" "memory" (memory 1 512))
            (global $result (mut i32) (i32.const -1))
            (func $start call $init global.set $result)
            (start $start)
            (func (export "start_result") (result i32) global.get $result)
            (func (export "invoke") (result i32) call $init)
        )"#,
        );
        let (imports, state) = hooks
            .additional_imports(&available, &mut store.as_store_mut())
            .unwrap();
        let session = state.session.as_ref().unwrap().clone();
        let instance = Instance::new(&mut store, &available, &imports).unwrap();
        let heap_before = session
            .inner
            .func_env
            .lock()
            .unwrap()
            .as_ref()
            .unwrap()
            .as_ref(&store)
            .guest_heap
            .clone()
            .expect("start section installs guest heap");
        hooks
            .configure_instance(
                &available,
                &mut store.as_store_mut(),
                &instance,
                None,
                state,
            )
            .unwrap();
        let heap_after = session
            .inner
            .func_env
            .lock()
            .unwrap()
            .as_ref()
            .unwrap()
            .as_ref(&store)
            .guest_heap
            .clone()
            .unwrap();
        assert!(Arc::ptr_eq(&heap_before, &heap_after));
        let result = instance
            .exports
            .get_typed_function::<(), i32>(&store, "start_result")
            .unwrap();
        let start_id = result.call(&mut store).unwrap();
        assert!(start_id > 0);
        let invoke = instance
            .exports
            .get_typed_function::<(), i32>(&store, "invoke")
            .unwrap();
        assert_eq!(invoke.call(&mut store).unwrap(), start_id);
        assert_eq!(spawns.load(Ordering::SeqCst), 1);
        lane_slot.lock().unwrap().as_ref().unwrap().stop();
        finished_rx.recv_timeout(Duration::from_secs(5)).unwrap();
    }

    #[test]
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn stopped_lazy_hooks_never_start_lane() {
        let spawns = Arc::new(AtomicUsize::new(0));
        let (finished_tx, _finished_rx) = mpsc::channel();
        let (hooks, lane_slot) = managed_hooks(Arc::clone(&spawns), finished_tx);
        hooks.runtime_control().terminate_all();
        let mut store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
            (import "napi" "napi_wasm_init_env" (func $init (result i32)))
            (import "env" "memory" (memory 1 512))
            (func (export "invoke") (result i32) call $init)
        )"#,
        );
        let (imports, state) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .unwrap();
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        hooks
            .configure_instance(&module, &mut store.as_store_mut(), &instance, None, state)
            .unwrap();
        let invoke = instance
            .exports
            .get_typed_function::<(), i32>(&store, "invoke")
            .unwrap();
        assert_eq!(invoke.call(&mut store).unwrap(), 0);
        assert_eq!(spawns.load(Ordering::SeqCst), 0);
        assert!(!lane_slot.lock().unwrap().is_some());
    }

    #[test]
    fn configure_instance_pairs_sessions_by_state() {
        let mut store_a = Store::default();
        let mut store_b = Store::new(store_a.engine().clone());
        let module = compile_wat(
            &store_a,
            r#"(module
                (import "napi" "napi_get_undefined" (func (param i32 i32) (result i32)))
                (import "env" "memory" (memory 1))
                (func (export "malloc") (param i32) (result i32) i32.const 8)
            )"#,
        );

        let ctx = NapiCtx::default();
        let hooks = ctx.runtime_hooks();
        let (imports_a, state_a) = hooks
            .additional_imports(&module, &mut store_a.as_store_mut())
            .expect("imports register for store A");
        assert!(
            state_a.session.is_some(),
            "session state travels with the caller"
        );
        let (imports_b, state_b) = hooks
            .additional_imports(&module, &mut store_b.as_store_mut())
            .expect("imports register for store B");

        // Instance setup completes in the reverse of the additional_imports
        // order, like two threads racing to cold-start the same module in
        // separate stores. Since each caller hands back the state it was
        // given, configure_instance always receives the session whose
        // function env lives in its own store.
        let instance_b = Instance::new(&mut store_b, &module, &imports_b).expect("instance B");
        hooks
            .configure_instance(
                &module,
                &mut store_b.as_store_mut(),
                &instance_b,
                None,
                state_b,
            )
            .expect("configure instance B");
        let instance_a = Instance::new(&mut store_a, &module, &imports_a).expect("instance A");
        hooks
            .configure_instance(
                &module,
                &mut store_a.as_store_mut(),
                &instance_a,
                None,
                state_a,
            )
            .expect("configure instance A");
    }

    #[test]
    fn configure_instance_rejects_missing_state() {
        let mut store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi" "napi_get_undefined" (func (param i32 i32) (result i32)))
                (memory (export "memory") 1)
            )"#,
        );

        let ctx = NapiCtx::default();
        let hooks = ctx.runtime_hooks();
        let (imports, _state) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .expect("imports register");
        let instance = Instance::new(&mut store, &module, &imports).expect("instance");

        let empty_state = super::NapiInstantiationState { session: None };
        let err = hooks
            .configure_instance(
                &module,
                &mut store.as_store_mut(),
                &instance,
                None,
                empty_state,
            )
            .expect_err("configuring an N-API instance without its session must fail");
        assert!(err.to_string().contains("missing N-API session"));
    }

    #[test]
    fn module_needs_napi_detects_none() {
        let store = Store::default();
        let module = Module::new(&store, EMPTY_WASM_MODULE).expect("empty wasm module compiles");

        assert_eq!(NapiCtx::module_needs_napi(&module), (None, None));
    }

    #[test]
    fn module_needs_napi_detects_core_napi_v10() {
        let store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi" "napi_get_undefined" (func (param i32 i32) (result i32)))
            )"#,
        );

        assert_eq!(
            NapiCtx::module_needs_napi(&module),
            (Some(NapiVersion::V10), None)
        );
    }

    #[test]
    fn module_needs_napi_rejects_wrong_frozen_signature() {
        let mut store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi" "unofficial_napi_create_env" (func (param i32 i32) (result i32)))
            )"#,
        );

        assert_eq!(
            NapiCtx::module_needs_napi(&module),
            (Some(NapiVersion::Unknown), None)
        );
        assert!(
            NapiCtx::default()
                .runtime_hooks()
                .additional_imports(&module, &mut store.as_store_mut())
                .is_err()
        );
    }

    #[test]
    fn module_needs_napi_detects_unknown_core_napi() {
        let store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi" "napi_future_function" (func))
            )"#,
        );

        assert_eq!(
            NapiCtx::module_needs_napi(&module),
            (Some(NapiVersion::Unknown), None)
        );
    }

    #[test]
    fn module_needs_napi_detects_extension_v0() {
        let store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi_extension_wasmer_v0" "unofficial_napi_get_hash_seed" (func))
            )"#,
        );

        assert_eq!(
            NapiCtx::module_needs_napi(&module),
            (None, Some(NapiWasmerExtensionVersion::V0))
        );
    }

    #[test]
    fn module_needs_napi_detects_unknown_extension_version() {
        let store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi_extension_wasmer_v1" "unofficial_napi_get_hash_seed" (func))
            )"#,
        );

        assert_eq!(
            NapiCtx::module_needs_napi(&module),
            (None, Some(NapiWasmerExtensionVersion::Unknown))
        );
    }

    #[test]
    fn module_needs_napi_detects_mixed_namespaces() {
        let store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "napi" "napi_get_undefined" (func (param i32 i32) (result i32)))
                (import "napi_extension_wasmer_v0" "unofficial_napi_get_hash_seed" (func))
            )"#,
        );

        assert_eq!(
            NapiCtx::module_needs_napi(&module),
            (Some(NapiVersion::V10), Some(NapiWasmerExtensionVersion::V0))
        );
    }

    #[test]
    fn napi_version_compatibility_is_additive() {
        assert!(NapiVersion::V10.is_compatible_with(NapiVersion::V10));
        assert!(!NapiVersion::V10.is_compatible_with(NapiVersion::Unknown));
        assert!(NapiVersion::Unknown.is_compatible_with(NapiVersion::V10));
        assert!(!NapiVersion::Unknown.is_compatible_with(NapiVersion::Unknown));
    }

    #[test]
    fn napi_wasmer_extension_version_compatibility_is_strict() {
        assert!(NapiWasmerExtensionVersion::V0.is_compatible_with(NapiWasmerExtensionVersion::V0));
        assert!(
            !NapiWasmerExtensionVersion::V0.is_compatible_with(NapiWasmerExtensionVersion::Unknown)
        );
        assert!(
            !NapiWasmerExtensionVersion::Unknown.is_compatible_with(NapiWasmerExtensionVersion::V0)
        );
        assert!(
            !NapiWasmerExtensionVersion::Unknown
                .is_compatible_with(NapiWasmerExtensionVersion::Unknown)
        );
    }
}
