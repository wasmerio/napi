use anyhow::{Context, Result, bail};
use std::{
    collections::HashSet,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
};
use wasmer::{Extern, ExternType, FunctionEnv, Imports, Instance, Module, StoreMut, Table, Value};

use crate::{
    NAPI_EXTENSION_WASMER_MODULE_NAME, NAPI_EXTENSION_WASMER_MODULE_PREFIX, NAPI_MODULE_NAME,
    NapiEnv, NapiVersion, NapiWasmerExtensionVersion,
    budget::{NapiMemoryAccountant, ResourceBudget},
    guest::napi::{is_known_napi_import, register_env_imports, register_napi_imports},
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

#[derive(Default)]
pub struct NapiCtxBuilder {
    limits: NapiLimits,
    accountant: Option<Arc<dyn NapiMemoryAccountant>>,
}

#[derive(Clone, Debug)]
pub struct NapiCtx {
    inner: Arc<NapiCtxInner>,
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

impl NapiInstantiationState {
    /// The session created for these imports. Retain a clone when an
    /// instantiation attachment also owns explicit retirement of its resources.
    pub fn session(&self) -> Option<&NapiSession> {
        self.session.as_ref()
    }
}

/// Runtime hooks that provide N-API imports for WASIX guests.
// The import phase creates a per-instantiation session holding the function
// env backing the imported host functions, and hands it to the caller as
// opaque state. The hooks themselves are stateless, so concurrent
// instantiations — same module or not, same store or not — cannot receive
// each other's sessions.
#[derive(Clone, Debug)]
pub struct NapiRuntimeHooks {
    ctx: NapiCtx,
}

/// Opaque control surface for stopping every V8 isolate owned by a context.
#[derive(Clone, Debug)]
pub struct NapiRuntimeControl {
    envs: Arc<Mutex<HashSet<usize>>>,
    host_stopped: Arc<AtomicBool>,
}

impl NapiRuntimeControl {
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
    }
}

#[derive(Debug)]
struct NapiCtxInner {
    limits: NapiLimits,
    active_sessions: AtomicUsize,
    /// One shared accountant per app, `Arc`-shared into the engine's budgeted
    /// tunables (guest wasm linear memory) and, in later phases, the V8 heap,
    /// external-memory, and CPU paths.
    budget: Arc<ResourceBudget>,
    envs: Arc<Mutex<HashSet<usize>>>,
    host_stopped: Arc<AtomicBool>,
}

struct NapiSessionInner {
    ctx: Arc<NapiCtxInner>,
    imported_memory_type: Option<wasmer::MemoryType>,
    imported_table_type: Option<wasmer::TableType>,
    state: Mutex<SessionState>,
}

enum SessionState {
    Unprepared,
    Attached(FunctionEnv<NapiEnv>),
    Deactivated,
}

impl std::fmt::Debug for NapiSession {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("NapiSession").finish_non_exhaustive()
    }
}

impl Drop for NapiSessionInner {
    fn drop(&mut self) {
        self.ctx.active_sessions.fetch_sub(1, Ordering::AcqRel);
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

    pub fn build(self) -> NapiCtx {
        let budget = match self.accountant {
            Some(accountant) => ResourceBudget::with_accountant(accountant),
            None => match self.limits.memory_budget_bytes() {
                Some(bytes) => ResourceBudget::with_memory_limit(bytes),
                None => ResourceBudget::unlimited(),
            },
        };
        NapiCtx {
            inner: Arc::new(NapiCtxInner {
                limits: self.limits,
                active_sessions: AtomicUsize::new(0),
                budget,
                envs: Arc::new(Mutex::new(HashSet::new())),
                host_stopped: Arc::new(AtomicBool::new(false)),
            }),
        }
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
        NapiRuntimeHooks { ctx: self.clone() }
    }

    pub fn runtime_control(&self) -> NapiRuntimeControl {
        NapiRuntimeControl {
            envs: Arc::clone(&self.inner.envs),
            host_stopped: Arc::clone(&self.inner.host_stopped),
        }
    }

    pub fn new_session(&self, module: &Module) -> Result<NapiSession> {
        let previous = self.inner.active_sessions.fetch_add(1, Ordering::AcqRel);
        if let Some(max_sessions) = self.inner.limits.max_sessions
            && previous >= max_sessions
        {
            self.inner.active_sessions.fetch_sub(1, Ordering::AcqRel);
            bail!("refusing to create more than {max_sessions} active N-API sessions");
        }

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
                ctx: Arc::clone(&self.inner),
                imported_memory_type,
                imported_table_type,
                state: Mutex::new(SessionState::Unprepared),
            }),
        })
    }
}

impl NapiRuntimeHooks {
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

        let session = self.ctx.prepare_module(module)?;
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
    /// Clears this session's store-local provider and guest-memory resources.
    /// Repeated calls do nothing. The session limit remains charged until the
    /// last clone of this session is dropped.
    ///
    /// # Safety
    /// `store` must be the exact store used to create this session's imports.
    /// All native invocations, callbacks, finalizers and browser Promise/JSPI
    /// continuations using this session must have finished or been retired.
    /// Call on the provider's owning thread.
    pub unsafe fn deactivate(&self, store: &mut StoreMut<'_>) {
        let state = std::mem::replace(
            &mut *self.inner.state.lock().expect("poisoned NapiSession mutex"),
            SessionState::Deactivated,
        );
        if let SessionState::Attached(func_env) = state {
            func_env.as_mut(&mut *store).deactivate();
        }
    }

    /// Creates imports for exactly one store-local instance attachment.
    /// Repeated calls, including after deactivation, return an error.
    pub fn create_imports(&self, store: &mut StoreMut<'_>) -> Result<Imports> {
        let mut import_object = Imports::new();
        self.add_imports(store, &mut import_object)?;
        Ok(import_object)
    }

    fn add_imports(&self, store: &mut StoreMut<'_>, import_object: &mut Imports) -> Result<()> {
        let mut state = self.inner.state.lock().expect("poisoned NapiSession mutex");
        if !matches!(*state, SessionState::Unprepared) {
            bail!("N-API session imports have already been created or deactivated");
        }
        let mut additions = Imports::new();
        let memory = if let Some(memory_type) = self.inner.imported_memory_type {
            let memory = if let Some(existing) = import_object.get_export("env", "memory") {
                let Extern::Memory(memory) = existing else {
                    bail!("env.memory import for N-API module is not a memory");
                };
                memory
            } else {
                let memory = wasmer::Memory::new(&mut *store, memory_type)?;
                additions.define("env", "memory", memory.clone());
                memory
            };
            Some(memory)
        } else {
            None
        };

        let table = if let Some(table_type) = self.inner.imported_table_type {
            let table = if let Some(existing) =
                import_object.get_export("env", "__indirect_function_table")
            {
                let Extern::Table(table) = existing else {
                    bail!("env.__indirect_function_table import for N-API module is not a table");
                };
                table
            } else {
                let table = Table::new(&mut *store, table_type, Value::FuncRef(None))?;
                additions.define("env", "__indirect_function_table", table.clone());
                table
            };
            Some(table)
        } else {
            None
        };

        // Validate shared imports before appending an environment to the Store.
        let mut napi_env = NapiEnv::new(
            Arc::clone(&self.inner.ctx.budget),
            self.inner.ctx.limits.max_envs,
            Arc::clone(&self.inner.ctx.envs),
            Arc::clone(&self.inner.ctx.host_stopped),
        );
        napi_env.memory = memory;
        napi_env.table = table;
        let func_env = FunctionEnv::new(store, napi_env);
        register_env_imports(store, &mut additions);
        register_napi_imports(store, &func_env, &mut additions);

        for (namespace, name, value) in additions.iter() {
            if !import_object.exists(namespace, name) {
                import_object.define(namespace, name, value.clone());
            }
        }
        *state = SessionState::Attached(func_env);
        Ok(())
    }

    pub fn configure_instance(
        &self,
        store: &mut StoreMut<'_>,
        instance: &Instance,
        imported_memory: Option<&wasmer::Memory>,
    ) -> Result<()> {
        let func_env = {
            let guard = self.inner.state.lock().expect("poisoned NapiSession mutex");
            let SessionState::Attached(func_env) = &*guard else {
                bail!("missing runtime function env during instance setup");
            };
            func_env.clone()
        };

        if func_env.as_ref(&*store).configured {
            bail!("N-API session instance has already been configured");
        }

        if let Some(memory) = imported_memory {
            func_env.as_mut(&mut *store).memory = Some(memory.clone());
        }

        #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
        {
            // Stand up the host-side guest-memory allocator now that the memory is
            // final. All bridge allocations of guest memory go through it, AND it
            // is the only backing store for V8 ArrayBuffers/Buffers/TypedArrays
            // (see TrackingArrayBufferAllocator in unofficial_napi.cc) — the
            // guest's own malloc is never called from the host, and host-heap
            // allocation is never used for guest-visible V8 memory. A module that
            // requests N-API imports without usable linear memory, or whose
            // memory can't support a GuestHeap, cannot run under this bridge: we
            // fail instantiation here rather than let allocation silently fall
            // back to unbudgeted, non-guest-shared host memory later.
            let memory = func_env
                .as_ref(&*store)
                .memory
                .clone()
                .context("N-API imports require the guest to have linear memory")?;
            let budget = Arc::clone(&func_env.as_ref(&*store).budget);
            let heap = crate::guest_heap::GuestHeap::get_or_create(&mut *store, &memory, budget)
                .context(
                    "failed to initialize the guest-heap allocator over the guest's linear memory",
                )?;
            func_env.as_mut(&mut *store).guest_heap = Some(heap);
        }

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
        // Wasm start functions run during Instance::new, before this setup.
        // Only a fully configured attachment may create provider environments.
        func_env.as_mut(&mut *store).configured = true;
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
    use super::NapiCtx;
    use crate::{NapiVersion, NapiWasmerExtensionVersion};
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

    fn compile_wat(store: &Store, wat: &str) -> Module {
        let wasm = parse_str(wat).expect("wat module parses");
        Module::new(store, wasm).expect("wat module compiles")
    }

    #[test]
    fn add_imports_preserves_earlier_bindings_and_shared_objects() {
        use wasmer::{
            Extern, Function, Imports, Memory, MemoryType, Table, TableType, Type, Value,
        };

        let mut store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "env" "memory" (memory 1 1))
                (import "env" "__indirect_function_table" (table 2 2 funcref))
                (import "env" "uv_get_total_memory" (func $total (result i64)))
                (import "napi" "napi_get_undefined" (func $undefined (param i32 i32) (result i32)))
                (import "napi" "napi_get_null" (func (param i32 i32) (result i32)))
                (func (export "earlier_env") (result i64) call $total)
                (func (export "earlier_napi") (result i32)
                    i32.const 0 i32.const 0 call $undefined)
            )"#,
        );
        let memory = Memory::new(&mut store, MemoryType::new(1, Some(1), false)).unwrap();
        let table = Table::new(
            &mut store,
            TableType::new(Type::FuncRef, 2, Some(2)),
            Value::FuncRef(None),
        )
        .unwrap();
        let mut imports = Imports::new();
        imports.define("env", "memory", memory.clone());
        imports.define("env", "__indirect_function_table", table.clone());
        imports.define(
            "env",
            "uv_get_total_memory",
            Function::new_typed(&mut store, || -> i64 { 123 }),
        );
        imports.define(
            "napi",
            "napi_get_undefined",
            Function::new_typed(&mut store, |_: i32, _: i32| -> i32 { 456 }),
        );
        let ctx = NapiCtx::default();
        let state = ctx
            .runtime_hooks()
            .add_imports(&module, &mut store.as_store_mut(), &mut imports)
            .unwrap();
        assert_eq!(
            imports.get_export("env", "memory"),
            Some(Extern::Memory(memory.clone()))
        );
        assert_eq!(
            imports.get_export("env", "__indirect_function_table"),
            Some(Extern::Table(table.clone()))
        );
        let session = state.session.as_ref().unwrap();
        let func_env = {
            let state = session.inner.state.lock().unwrap();
            let super::SessionState::Attached(func_env) = &*state else {
                panic!("session must own its imported environment");
            };
            func_env.clone()
        };
        assert_eq!(func_env.as_ref(&store).memory.as_ref(), Some(&memory));
        assert_eq!(func_env.as_ref(&store).table.as_ref(), Some(&table));
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        assert_eq!(
            instance
                .exports
                .get_typed_function::<(), i64>(&store, "earlier_env")
                .unwrap()
                .call(&mut store)
                .unwrap(),
            123
        );
        assert_eq!(
            instance
                .exports
                .get_typed_function::<(), i32>(&store, "earlier_napi")
                .unwrap()
                .call(&mut store)
                .unwrap(),
            456
        );
    }

    #[test]
    fn invalid_shared_import_does_not_publish_partial_bindings() {
        use wasmer::{Function, Imports};

        let mut store = Store::default();
        let module = compile_wat(
            &store,
            r#"(module
                (import "env" "memory" (memory 1))
                (import "napi" "napi_get_undefined" (func (param i32 i32) (result i32)))
            )"#,
        );
        let mut imports = Imports::new();
        let invalid_memory = Function::new_typed(&mut store, || {});
        imports.define("env", "memory", invalid_memory.clone());
        let ctx = NapiCtx::default();
        assert!(
            ctx.runtime_hooks()
                .add_imports(&module, &mut store.as_store_mut(), &mut imports)
                .is_err()
        );
        assert_eq!(imports.iter().count(), 1);
        assert_eq!(
            imports.get_export("env", "memory"),
            Some(invalid_memory.into())
        );
        assert_eq!(ctx.active_sessions(), 0);
    }

    #[test]
    fn session_import_attachment_is_unique_and_cannot_be_reactivated() {
        use wasmer::Extern;

        let mut store = Store::default();
        let mut other = Store::new(store.engine().clone());
        let module = compile_wat(
            &store,
            r#"(module
                (import "env" "memory" (memory 1))
                (import "napi" "napi_get_undefined" (func (param i32 i32) (result i32)))
            )"#,
        );
        let ctx = NapiCtx::default();
        let session = ctx.prepare_module(&module).unwrap();
        let imports = session.create_imports(&mut store.as_store_mut()).unwrap();
        assert!(session.create_imports(&mut store.as_store_mut()).is_err());
        assert!(session.create_imports(&mut other.as_store_mut()).is_err());
        // SAFETY: this session has never entered the provider or guest code.
        unsafe {
            session.deactivate(&mut store.as_store_mut());
            session.deactivate(&mut store.as_store_mut());
        }
        assert!(session.create_imports(&mut store.as_store_mut()).is_err());
        let Some(Extern::Function(init)) = imports.get_export("napi", "napi_wasm_init_env") else {
            panic!("original imports must retain their environment");
        };
        assert_eq!(init.call(&mut store, &[]).unwrap()[0].i32(), Some(0));
        assert_eq!(ctx.budget().snapshot().live_isolates, 0);
        assert_eq!(ctx.active_sessions(), 1);
        drop(session);
        assert_eq!(ctx.active_sessions(), 0);
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
                (import "napi" "napi_get_undefined" (func))
            )"#,
        );

        assert_eq!(
            NapiCtx::module_needs_napi(&module),
            (Some(NapiVersion::V10), None)
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
                (import "napi" "napi_get_undefined" (func))
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
