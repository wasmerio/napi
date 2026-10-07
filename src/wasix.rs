//! Integration with the WASIX runtime's instantiation hooks.

use anyhow::{Context, Result};
use wasmer::{Imports, Instance, Memory, Module, StoreMut};
use wasmer_wasix::{
    capabilities::Capabilities,
    runtime::{InstantiationHook, InstantiationState},
};

use crate::{NapiInstantiationState, NapiRuntimeHooks};

/// Lets WASIX embedders register the hooks directly, e.g. through
/// `PluggableRuntime::with_instantiation_hook`.
impl InstantiationHook for NapiRuntimeHooks {
    /// Configures a process whose main module imports N-API. Other modules are
    /// left as the embedder configured them, so a process tree can mix the two:
    /// a shell without N-API that runs `node`, or `node` that spawns Python.
    ///
    /// See `Capabilities::enable_async_entrypoint` for the configurations WASIX
    /// supports and what each one gives up. N-API uses one per host.
    ///
    /// # No context switching, on every host
    ///
    /// N-API invokes guest callbacks through a C function that returns `u32`,
    /// synchronously, so a callback cannot suspend. The context-switching API
    /// has `call_dynamic` and the dynamic linker's lazy-binding stubs re-enter
    /// the guest in a way that lets it suspend, and that cannot be offered to a
    /// guest some of whose calls cannot. So it is turned off: the
    /// context-switching syscalls answer `Notsup`, and `call_dynamic` and the
    /// lazy-binding stubs call the guest synchronously.
    ///
    /// # Asynchronous entry on JavaScript hosts
    ///
    /// In the browser and in Node, N-API values live in the host's JavaScript
    /// realm. Promise reactions, timers and messages reach the guest only once
    /// its stack hands control back to that realm's event loop, so N-API's event
    /// loop checkpoint and message wait suspend the guest through JSPI, and
    /// only a guest entered asynchronously can suspend. So the entry stays
    /// asynchronous.
    ///
    /// The cost: the guest must not reach the checkpoint from inside an N-API
    /// callback, where it cannot suspend. If it does, the suspension is refused
    /// and the callback fails, which is also what the engine itself did before
    /// WASIX refused it first.
    ///
    /// # Synchronous entry on native hosts
    ///
    /// Natively, every import N-API registers is synchronous (the event loop
    /// checkpoint is a plain call into the bridge), and with the
    /// context-switching API gone so is every WASIX import. Nothing can suspend,
    /// so an asynchronous entry would only cost a coroutine and a stack of its
    /// own per process and per thread, and it is turned off. This is also how
    /// these guests have always run natively.
    ///
    /// The cost: the guest cannot suspend at all, so an asynchronous import
    /// added later, by N-API or by WASIX, would fail when called rather than
    /// wait.
    fn configure_capabilities(&self, module: &Module, capabilities: &mut Capabilities) {
        let (napi_version, napi_extension_version) = crate::module_needs_napi(module);
        if napi_version.is_none() && napi_extension_version.is_none() {
            return;
        }
        capabilities.enable_context_switching = false;
        #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
        {
            capabilities.enable_async_entrypoint = false;
        }
    }

    fn additional_imports(
        &self,
        module: &Module,
        store: &mut StoreMut,
    ) -> Result<(Imports, InstantiationState)> {
        let (imports, state) = NapiRuntimeHooks::additional_imports(self, module, store)?;
        Ok((imports, InstantiationState::new(state)))
    }

    fn prepare_imports(
        &self,
        module: &Module,
        store: &mut StoreMut,
        imports: &mut Imports,
    ) -> Result<InstantiationState> {
        let state = NapiRuntimeHooks::add_imports(self, module, store, imports)?;
        Ok(InstantiationState::new(state))
    }

    fn configure_new_instance(
        &self,
        module: &Module,
        store: &mut StoreMut,
        instance: &Instance,
        imported_memory: Option<&Memory>,
        state: InstantiationState,
    ) -> Result<()> {
        let state = state
            .take::<NapiInstantiationState>()
            .context("invalid N-API instance setup state")?;
        NapiRuntimeHooks::configure_instance(self, module, store, instance, imported_memory, state)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::NapiCtx;
    use wasmer::{AsStoreMut, Extern, MemoryType, Pages, Store};

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
        let _state = InstantiationHook::prepare_imports(
            &hooks,
            &module,
            &mut store.as_store_mut(),
            &mut imports,
        )
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
