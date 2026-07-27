//! Integration with the WASIX runtime's instantiation hooks.

use anyhow::{Context, Result};
use wasmer::{Imports, Instance, Memory, Module, StoreMut};
use wasmer_wasix::runtime::{InstantiationHook, InstantiationState};

use crate::{NapiInstantiationState, NapiRuntimeHooks};

/// Lets WASIX embedders register the hooks directly, e.g. through
/// `PluggableRuntime::with_instantiation_hook`.
impl InstantiationHook for NapiRuntimeHooks {
    fn additional_imports(
        &self,
        module: &Module,
        store: &mut StoreMut,
    ) -> Result<(Imports, InstantiationState)> {
        let (imports, state) = NapiRuntimeHooks::additional_imports(self, module, store)?;
        Ok((imports, InstantiationState::new(state)))
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
