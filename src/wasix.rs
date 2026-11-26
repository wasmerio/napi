//! N-API as an instantiation hook of the WASIX v2 runtime.

use wasixv2_exec::{ExecEnv, InstanceAttachment, InstantiationHook};
use wasmer::{FunctionEnv, Imports, Instance, Memory, Module, RuntimeError, StoreMut};

use crate::{NapiCtx, NapiInstantiationState, NapiRuntimeHooks};

/// Gives the guests of a WASIX v2 machine their N-API imports.
///
/// Register it with the machine's builder
/// (`MachineBuilder::instantiation_hook`). The machine then asks it for the
/// imports of every instance it creates: a process's main module, each of its
/// threads, the images it `execve`s. An instance whose module imports no N-API
/// function gets nothing and costs nothing.
///
/// Every instance of one hook draws on the hook's [`NapiCtx`]: its budget, its
/// environment ids and its runtime control. Use one hook per application.
///
/// # What the embedder provides
///
/// **Stack.** V8 runs on the stack of the thread that polls the guest, below
/// the frames of the machine's executor. It takes up to 984 KiB from where the
/// guest's first import finds it, less when the thread has less left, and
/// then throws a `RangeError`. A thread that runs a guest therefore needs the
/// executor's depth plus about 1.2 MiB. A debug build of the machine alone
/// goes several megabytes deep: give guest threads a large stack there
/// (`MachineBuilder::task_executor`), as `cli` does.
///
/// **A runtime whose other threads finish the guest's waits.** JavaScript
/// calls guest functions from inside V8's frames, which cannot be suspended,
/// so the thread stays in such a call until the guest function returns. While
/// the guest function waits in a syscall, the thread sleeps: the wait has to
/// be finished by another thread, such as a worker of a multi-thread tokio
/// runtime, its blocking pool or a timer thread. A task that only this
/// thread's executor can run does not progress meanwhile, so a guest function
/// that JavaScript calls must not wait for one (for a child process that the
/// machine runs on this same thread, say).
///
/// # Limits
///
/// - A guest function that JavaScript calls is not one of the machine's own
///   calls into the guest. It can make syscalls, exit and be interrupted, but
///   it cannot unwind and rewind its stack: no `fork` and no `setjmp` built
///   on asyncify from inside a callback.
/// - A process is ended by a fault in its own code, not in a callback: as in
///   every N-API embedding, a callback that traps is reported and JavaScript
///   goes on with `undefined`.
#[derive(Clone, Debug)]
pub struct NapiHook {
    hooks: NapiRuntimeHooks,
}

impl NapiHook {
    /// A hook whose instances belong to `context`.
    pub fn new(context: NapiCtx) -> Self {
        Self {
            hooks: context.runtime_hooks(),
        }
    }

    /// A hook over hooks that already exist, such as the managed ones
    /// [`NapiCtxBuilder::build_managed_imports`] returns.
    ///
    /// [`NapiCtxBuilder::build_managed_imports`]: crate::NapiCtxBuilder::build_managed_imports
    pub fn from_hooks(hooks: NapiRuntimeHooks) -> Self {
        Self { hooks }
    }

    /// The hooks this registers; their context is the application's.
    pub fn hooks(&self) -> &NapiRuntimeHooks {
        &self.hooks
    }

    /// Adds what `module` imports of this hook's functions to `imports`.
    fn add_imports(
        &self,
        module: &Module,
        store: &mut StoreMut<'_>,
        imports: &mut Imports,
    ) -> Result<NapiInstantiationState, RuntimeError> {
        let state = self
            .hooks
            .add_imports(module, store, imports)
            .map_err(runtime_error)?;
        // The `env` functions are declared for every module, so they are also
        // provided to one that links libuv's host functions without N-API.
        if !state.has_session()
            && module.imports().any(|import| {
                import.module() == "env" && NapiCtx::provides_import("env", import.name())
            })
        {
            crate::guest::napi::register_env_imports(store, imports);
        }
        Ok(state)
    }
}

impl From<NapiRuntimeHooks> for NapiHook {
    fn from(hooks: NapiRuntimeHooks) -> Self {
        Self::from_hooks(hooks)
    }
}

impl<S: Send + Sync + 'static> InstantiationHook<S> for NapiHook {
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
        let state = self.add_imports(module, store, imports)?;
        if !state.has_session() {
            return Ok(None);
        }
        Ok(Some(Box::new(Attachment {
            hooks: self.hooks.clone(),
            state,
            exec: exec.clone(),
        })))
    }
}

/// The N-API half of one instance, from its imports to its end.
struct Attachment<S> {
    hooks: NapiRuntimeHooks,
    state: NapiInstantiationState,
    /// The instance's WASIX environment, which knows how far its ABI lets the
    /// memory grow.
    exec: FunctionEnv<ExecEnv<S>>,
}

impl<S: Send + Sync + 'static> InstanceAttachment for Attachment<S> {
    fn configure(
        &mut self,
        module: &Module,
        store: &mut StoreMut<'_>,
        instance: &Instance,
        memory: &Memory,
    ) -> Result<(), RuntimeError> {
        // The guest heap claims pages of the guest's memory with the host's
        // own `memory.grow`, which the limit of the guest's ABI does not see:
        // WASIX v1 keeps the top of the address space for itself.
        let memory_limit = self
            .exec
            .as_ref(&*store)
            .abi()
            .map(|abi| abi.memory_limit());
        self.hooks
            .configure_instance_within(
                module,
                store,
                instance,
                Some(memory),
                self.state.clone(),
                memory_limit,
            )
            .map_err(runtime_error)
    }

    fn deactivate(&mut self, store: &mut StoreMut<'_>) {
        self.state.release_envs(store);
    }
}

fn runtime_error(error: anyhow::Error) -> RuntimeError {
    match error.downcast::<RuntimeError>() {
        Ok(error) => error,
        Err(error) => RuntimeError::new(format!("{error:#}")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use wasmer::{AsStoreMut, Extern, MemoryType, Pages, Store};

    #[test]
    fn declares_exactly_the_imports_it_provides() {
        let hook = NapiHook::new(NapiCtx::default());
        let provides =
            |namespace, name| InstantiationHook::<()>::provides_import(&hook, namespace, name);
        assert!(provides("napi", "napi_get_undefined"));
        assert!(!provides("napi", "napi_function_of_the_future"));
        assert!(provides(
            "napi_extension_wasmer_v0",
            "unofficial_napi_create_env"
        ));
        assert!(!provides("napi_extension_wasmer_v1", "anything"));
        assert!(provides("env", "uv_cpu_info"));
        assert!(!provides("env", "memory"));
        assert!(!provides("wasi_snapshot_preview1", "fd_write"));
    }

    #[test]
    fn a_module_without_napi_still_gets_the_env_functions_it_imports() {
        let mut store = Store::default();
        let module = Module::new(
            &store,
            r#"(module
                (import "env" "uv_get_free_memory" (func (result i64)))
                (memory (export "memory") 1))"#,
        )
        .expect("module compiles");
        let hook = NapiHook::new(NapiCtx::default());
        let mut imports = Imports::new();
        let state = hook
            .add_imports(&module, &mut store.as_store_mut(), &mut imports)
            .expect("imports can be prepared");
        assert!(!state.has_session(), "nothing to configure without N-API");
        assert!(imports.get_export("env", "uv_get_free_memory").is_some());
        wasmer::Instance::new(&mut store, &module, &imports).expect("the module links");
    }

    #[test]
    fn add_imports_reuses_the_runtime_memory() {
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
                .expect("the runtime's memory can be created");
        memory
            .view(&store)
            .write(32, &[1, 2, 3, 4])
            .expect("marker can be written");
        let mut imports = Imports::new();
        imports.define("env", "memory", memory);

        let hooks = NapiCtx::default().runtime_hooks();
        let state = hooks
            .add_imports(&module, &mut store.as_store_mut(), &mut imports)
            .expect("N-API imports can be prepared");
        assert!(state.has_session());

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
