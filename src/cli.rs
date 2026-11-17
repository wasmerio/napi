use anyhow::{Context, Result};
#[cfg(unix)]
use std::any::Any;
use std::io;
use std::panic::{AssertUnwindSafe, catch_unwind, resume_unwind};
use std::path::{Component, Path, PathBuf};
use std::sync::{Arc, Mutex};
use wasixv2::{
    ByteSink, EmptySource, Host, Image, LoadError, Loader, LocalFuture, Machine, MachineBuilder,
    ProcessConfig, Stdio, TokioHost,
};
#[cfg(unix)]
use wasixv2::{
    Signaller,
    terminal::{HostTerminal, RawMode},
};
use wasmer::sys::{Cranelift, EngineBuilder, Features};
use wasmer::{Engine, Module, Store};
use wasmer_cache::{Cache, FileSystemCache, Hash as CacheHash};
use wasmer_types::ModuleHash;

use crate::{NapiCtx, NapiHook, budget::ResourceBudget, budget::budgeted_tunables};
use wasmer_c_api_imports::WasmCapiRuntimeHooks;

#[derive(Debug, Clone)]
pub struct GuestMount {
    pub host_path: PathBuf,
    pub guest_path: PathBuf,
}

fn normalized_mounts(mounts: &[GuestMount]) -> Result<Vec<GuestMount>> {
    mounts
        .iter()
        .map(|mount| {
            let mut guest = PathBuf::from("/");
            for component in mount.guest_path.components() {
                match component {
                    Component::RootDir => guest = PathBuf::from("/"),
                    Component::CurDir => {}
                    Component::Normal(part) => guest.push(part),
                    Component::ParentDir => {
                        if !guest.pop() {
                            anyhow::bail!(
                                "guest mount path escapes the root: {}",
                                mount.guest_path.display()
                            );
                        }
                    }
                    Component::Prefix(_) => {
                        anyhow::bail!("platform-specific guest mount prefixes are unsupported")
                    }
                }
            }
            Ok(GuestMount {
                host_path: mount.host_path.clone(),
                guest_path: guest,
            })
        })
        .collect()
}

pub struct LoadedWasm {
    pub store: Store,
    pub module: Module,
    pub module_hash: ModuleHash,
}

fn create_cli_store(budget: Arc<ResourceBudget>) -> Store {
    let mut features = Features::default();
    features.exceptions(true);

    let mut engine = EngineBuilder::new(Cranelift::default())
        .set_features(Some(features))
        .engine();
    // Charge the guest's (imported) wasm linear memory against the app budget.
    // The runtime instantiates the guest with this engine, so its budgeted
    // tunables cover the main store and every worker store cloned from it.
    let tunables = budgeted_tunables(budget);
    engine.set_tunables(tunables);
    Store::new(engine)
}

fn wasmer_cache_dir() -> PathBuf {
    std::env::var_os("WASMER_NAPI_CACHE_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| std::env::temp_dir().join("wasmer-napi-cache"))
}

fn load_or_compile_module(store: &Store, wasm_bytes: &[u8]) -> Result<Module> {
    let binary = wasmer::wat2wasm(wasm_bytes).context("failed to parse wasm module")?;
    let prepared = wasixv2_dylink::prepare_image(&binary)?;
    let (compiler_input, failed) = if prepared == binary.as_ref() {
        wasixv2::prepare_for_compile(&prepared)?
    } else {
        (wasixv2::instrument(&prepared)?, None)
    };
    // Dynamic preparation precedes instrumentation, and the persistent cache
    // identifies the exact compiler input rather than its original source.
    let key = CacheHash::generate(&compiler_input);
    let mut cache = FileSystemCache::new(wasmer_cache_dir())
        .context("failed to create/access Wasmer cache directory")?;

    let module = match unsafe { cache.load(store, key) } {
        Ok(module) => module,
        Err(_) => {
            let module =
                Module::new(store, &compiler_input).context("failed to compile wasm module")?;
            let _ = cache.store(key, &module);
            module
        }
    };
    if let Some(error) = failed {
        wasixv2::warn_not_preemptible(&error);
    }
    Ok(module)
}

struct CliLoader {
    engine: Engine,
}

impl Loader for CliLoader {
    fn load<'a>(
        &'a self,
        _: &'a Engine,
        image: &'a Image,
    ) -> LocalFuture<'a, std::result::Result<(Engine, Module), LoadError>> {
        Box::pin(async move {
            // Every exec image uses the same budgeted engine as the initial
            // module, including its memory wrappers and compiler features.
            let store = Store::new(self.engine.clone());
            let module = load_or_compile_module(&store, image.bytes()).map_err(LoadError::other)?;
            Ok((self.engine.clone(), module))
        })
    }
}

struct CaptureSink {
    output: Arc<dyn ByteSink>,
    writing: tokio::sync::Mutex<()>,
    bytes: Mutex<Vec<u8>>,
    error: Mutex<Option<io::Error>>,
}

impl CaptureSink {
    fn new(output: Arc<dyn ByteSink>) -> Arc<Self> {
        Arc::new(Self {
            output,
            writing: tokio::sync::Mutex::new(()),
            bytes: Mutex::new(Vec::new()),
            error: Mutex::new(None),
        })
    }

    fn captured(&self) -> Result<String> {
        if let Some(error) = self
            .error
            .lock()
            .map_err(|_| anyhow::anyhow!("stdio capture mutex poisoned"))?
            .take()
        {
            return Err(error).context("failed writing drained WASIX stdio");
        }
        let bytes = self
            .bytes
            .lock()
            .map_err(|_| anyhow::anyhow!("stdio capture mutex poisoned"))?;
        String::from_utf8(bytes.clone()).context("WASIX stdio was not valid UTF-8")
    }
}

impl ByteSink for CaptureSink {
    fn write<'a>(&'a self, data: &'a [u8]) -> LocalFuture<'a, io::Result<usize>> {
        Box::pin(async move {
            // Serialize forwarding and capture together, preserving the order
            // of concurrent guest writes to each descriptor.
            let _writing = self.writing.lock().await;
            let n = match self.output.write(data).await {
                Ok(n) => n,
                Err(error) => {
                    let kind = error.kind();
                    let mut first = self
                        .error
                        .lock()
                        .map_err(|_| io::Error::other("stdio capture mutex poisoned"))?;
                    if first.is_none() {
                        *first = Some(error);
                    }
                    return Err(kind.into());
                }
            };
            self.bytes
                .lock()
                .map_err(|_| io::Error::other("stdio capture mutex poisoned"))?
                .extend_from_slice(&data[..n]);
            Ok(n)
        })
    }
}

#[cfg(unix)]
type Panic = Box<dyn Any + Send>;
type Outcome<T> = std::thread::Result<Result<T>>;

fn discard_cleanup<T>(value: T) {
    let _ = catch_unwind(AssertUnwindSafe(|| drop(value)));
}

fn preserve_cleanup<T>(outcome: &mut Outcome<T>, cleanup: Outcome<()>) {
    match cleanup {
        Ok(Ok(())) => {}
        Ok(Err(error)) => {
            if matches!(outcome, Ok(Ok(_))) {
                *outcome = Ok(Err(error));
            } else {
                discard_cleanup(error);
            }
        }
        Err(panic) => {
            if matches!(outcome, Ok(Ok(_))) {
                *outcome = Err(panic);
            } else {
                discard_cleanup(panic);
            }
        }
    }
}

#[cfg(unix)]
#[derive(Default)]
struct SignalForwarders {
    workers: Vec<tokio::task::JoinHandle<()>>,
}

#[cfg(unix)]
impl SignalForwarders {
    fn install(&mut self, signaller: Signaller) -> io::Result<()> {
        use tokio::signal::unix::{SignalKind, signal};
        // WasiRunner attached only ctrl_c, for direct and captured runs alike.
        // Keep the same process-directed SIGINT subscription here.
        let mut stream = signal(SignalKind::interrupt())?;
        self.workers.push(tokio::spawn(async move {
            while stream.recv().await.is_some() {
                let _ = signaller.signal(2);
            }
        }));
        Ok(())
    }

    async fn stop(&mut self) {
        for worker in &self.workers {
            worker.abort();
        }
        let mut first: Option<Panic> = None;
        while let Some(worker) = self.workers.last_mut() {
            // Borrow the handle until completion. Canceling this waiter keeps
            // the exact worker in our custody for the next cleanup attempt.
            if let Err(error) = worker.await
                && error.is_panic()
            {
                let panic = error.into_panic();
                if first.is_none() {
                    first = Some(panic);
                } else {
                    discard_cleanup(panic);
                }
            }
            drop(self.workers.pop());
        }
        if let Some(panic) = first {
            resume_unwind(panic);
        }
    }
}

#[cfg(unix)]
impl Drop for SignalForwarders {
    fn drop(&mut self) {
        for worker in &self.workers {
            worker.abort();
        }
    }
}

pub fn load_wasix_module(wasm_path: &Path) -> Result<LoadedWasm> {
    load_wasix_module_with_budget(wasm_path, ResourceBudget::unlimited())
}

pub fn load_wasix_module_with_budget(
    wasm_path: &Path,
    budget: Arc<ResourceBudget>,
) -> Result<LoadedWasm> {
    let wasm_bytes = std::fs::read(wasm_path)
        .with_context(|| format!("failed to read wasm file at {}", wasm_path.display()))?;

    let store = create_cli_store(budget);

    let module = load_or_compile_module(&store, &wasm_bytes)?;

    let module_hash = ModuleHash::sha256(&wasm_bytes);

    Ok(LoadedWasm {
        store,
        module,
        module_hash,
    })
}

/// Installs host mounts on a fresh v2 machine.
///
/// Root mappings remain copy-on-write lower layers, with the first root taking
/// precedence. Other mounts write directly to their host directories.
pub async fn configure_runner_mounts(
    machine: &mut Machine,
    _wasm_path: &Path,
    extra_mounts: &[GuestMount],
) -> Result<()> {
    let mounts = normalized_mounts(extra_mounts)?;
    // The old runner's root mounts were lower layers under its writable
    // root. v2 inserts each later layer on top, so reverse their order.
    let roots: Vec<_> = mounts
        .iter()
        .rev()
        .filter(|mount| mount.guest_path == Path::new("/"))
        .map(|mount| {
            machine.host_dir_layer(&mount.host_path).with_context(|| {
                format!(
                    "failed to create host root layer for {}",
                    mount.host_path.display()
                )
            })
        })
        .collect::<Result<_>>()?;
    if !roots.is_empty() {
        machine.mount_root_layers(roots).await?;
    }
    for mount in mounts {
        if mount.guest_path == Path::new("/") {
            continue;
        }
        let guest = mount
            .guest_path
            .to_str()
            .context("guest mount path was not UTF-8")?;
        machine
            .mount_host_dir(guest, &mount.host_path, false)
            .await
            .with_context(|| {
                format!("failed to mount host fs for {}", mount.host_path.display())
            })?;
    }

    Ok(())
}

pub fn run_wasix_main_capture_stdio(
    wasm_path: &Path,
    args: &[String],
    extra_mounts: &[GuestMount],
) -> Result<(i32, String, String)> {
    let ctx = NapiCtx::default();
    run_wasix_main_capture_stdio_with_ctx(&ctx, wasm_path, args, extra_mounts)
}

/// Execute with lightweight managed bindings; the lane activates only on first use.
pub fn run_wasix_main_capture_stdio_with_hooks(
    hooks: &crate::NapiRuntimeHooks,
    wasm_path: &Path,
    args: &[String],
    extra_mounts: &[GuestMount],
) -> Result<(i32, String, String)> {
    run_wasix_main_capture_stdio_with_ctx(&hooks.context(), wasm_path, args, extra_mounts)
}

pub fn run_wasix_main_capture_stdio_with_ctx(
    ctx: &NapiCtx,
    wasm_path: &Path,
    args: &[String],
    extra_mounts: &[GuestMount],
) -> Result<(i32, String, String)> {
    let inherited = Stdio::inherit();
    let stdout = CaptureSink::new(inherited.stdout);
    let stderr = CaptureSink::new(inherited.stderr);
    let stdio = Stdio {
        stdin: Arc::new(EmptySource),
        stdout: stdout.clone(),
        stderr: stderr.clone(),
    };
    let config = process_config("guest-test", args, &[], stdio);
    let exit_code = run_wasix_main_with_machine(ctx, wasm_path, extra_mounts, config, None, false)?;

    Ok((exit_code, stdout.captured()?, stderr.captured()?))
}

#[allow(clippy::too_many_arguments)]
pub fn run_wasix_main_with_ctx(
    ctx: &NapiCtx,
    wasm_path: &Path,
    program_name: &str,
    args: &[String],
    extra_mounts: &[GuestMount],
    envs: &[(String, String)],
    current_dir: Option<&Path>,
) -> Result<i32> {
    let config = process_config(program_name, args, envs, Stdio::inherit());
    run_wasix_main_with_machine(ctx, wasm_path, extra_mounts, config, current_dir, true)
}

fn process_config(
    program_name: &str,
    args: &[String],
    envs: &[(String, String)],
    stdio: Stdio,
) -> ProcessConfig {
    ProcessConfig {
        args: std::iter::once(program_name.to_string())
            .chain(args.iter().cloned())
            .collect(),
        env: envs
            .iter()
            .map(|(key, value)| format!("{key}={value}"))
            .collect(),
        stdio,
    }
}

fn run_wasix_main_with_machine(
    ctx: &NapiCtx,
    wasm_path: &Path,
    extra_mounts: &[GuestMount],
    config: ProcessConfig,
    current_dir: Option<&Path>,
    use_system_tty: bool,
) -> Result<i32> {
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("failed to create tokio runtime for WASIX")?;
    let guard = runtime.enter();
    let local = tokio::task::LocalSet::new();
    let mut machine = None;
    #[cfg(unix)]
    let mut forwarders = SignalForwarders::default();
    #[cfg(unix)]
    let mut raw: Option<RawMode> = None;

    let mut outcome = catch_unwind(AssertUnwindSafe(|| -> Result<i32> {
        let loaded = load_wasix_module_with_budget(wasm_path, ctx.budget())?;
        let engine = loaded.store.engine().clone();
        let module = loaded.module;
        let host: Arc<dyn Host> = Arc::new(TokioHost::default());
        let builder = MachineBuilder::new(host)
            .loader(Arc::new(CliLoader {
                engine: engine.clone(),
            }))
            .instantiation_hook(Arc::new(NapiHook::new(ctx.clone())))
            .instantiation_hook(Arc::new(WasmCapiRuntimeHooks::new()));
        #[cfg(unix)]
        let terminal = use_system_tty.then(HostTerminal::detect).flatten();
        #[cfg(unix)]
        let builder = match &terminal {
            Some(terminal) => {
                let (console, fds) = terminal
                    .console(&config.stdio)
                    .context("cannot use the terminal as the guest's console")?;
                builder.console(console, fds)
            }
            None => builder,
        };
        machine = Some(builder.build());
        let machine = machine.as_mut().unwrap();
        if let Some(current_dir) = current_dir {
            machine.set_working_dir(
                current_dir
                    .to_str()
                    .context("guest working directory was not UTF-8")?,
            );
        }
        runtime.block_on(local.run_until(async {
            configure_runner_mounts(machine, wasm_path, extra_mounts).await?;
            #[cfg(unix)]
            {
                // Root layers must be complete before signaller() seals the
                // machine's initial filesystem configuration.
                forwarders
                    .install(machine.signaller())
                    .context("cannot install signal handlers")?;
                raw = terminal
                    .as_ref()
                    .map(HostTerminal::raw)
                    .transpose()
                    .context("cannot put the terminal in raw mode")?;
            }
            machine
                .run_module(&engine, &module, config)
                .await
                .context("failed to run WASIX module through Machine")
        }))
    }));

    // Keep terminal and worker custody outside the caught guest future. Every
    // cleanup phase runs after an error or panic, preserving the first failure.
    preserve_cleanup(
        &mut outcome,
        catch_unwind(AssertUnwindSafe(|| {
            runtime.block_on(local.run_until(async {
                if let Some(machine) = &machine {
                    machine.shutdown().await;
                }
                Ok(())
            }))
        })),
    );
    #[cfg(unix)]
    preserve_cleanup(
        &mut outcome,
        catch_unwind(AssertUnwindSafe(|| {
            runtime.block_on(local.run_until(async {
                forwarders.stop().await;
                Ok(())
            }))
        })),
    );
    preserve_cleanup(
        &mut outcome,
        catch_unwind(AssertUnwindSafe(|| {
            runtime.block_on(local.run_until(async {
                drop(machine.take());
                Ok(())
            }))
        })),
    );
    preserve_cleanup(
        &mut outcome,
        catch_unwind(AssertUnwindSafe(|| {
            drop(local);
            Ok(())
        })),
    );
    preserve_cleanup(
        &mut outcome,
        catch_unwind(AssertUnwindSafe(|| {
            drop(guard);
            Ok(())
        })),
    );
    preserve_cleanup(
        &mut outcome,
        catch_unwind(AssertUnwindSafe(|| {
            if use_system_tty {
                runtime.shutdown_background();
            } else {
                drop(runtime);
            }
            Ok(())
        })),
    );
    #[cfg(unix)]
    preserve_cleanup(
        &mut outcome,
        catch_unwind(AssertUnwindSafe(|| {
            if let Some(mut raw) = raw.take() {
                raw.restore().context("cannot restore the host terminal")?;
            }
            Ok(())
        })),
    );
    match outcome {
        Ok(result) => result,
        Err(panic) => resume_unwind(panic),
    }
}

pub fn run_wasix_main_capture_stdout(
    wasm_path: &Path,
    args: &[String],
    extra_mounts: &[GuestMount],
) -> Result<(i32, String)> {
    let ctx = NapiCtx::default();
    run_wasix_main_capture_stdout_with_ctx(&ctx, wasm_path, args, extra_mounts)
}

pub fn run_wasix_main_capture_stdout_with_ctx(
    ctx: &NapiCtx,
    wasm_path: &Path,
    args: &[String],
    extra_mounts: &[GuestMount],
) -> Result<(i32, String)> {
    let (exit_code, stdout, _stderr) =
        run_wasix_main_capture_stdio_with_ctx(ctx, wasm_path, args, extra_mounts)?;
    Ok((exit_code, stdout))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cleanup_preserves_original_errors_and_panics() {
        #[derive(Debug)]
        struct Original(Arc<()>);
        impl std::fmt::Display for Original {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                f.write_str("original run error")
            }
        }
        impl std::error::Error for Original {}
        let original = Arc::new(());
        let mut outcome: Outcome<i32> = Ok(Err(Original(original.clone()).into()));
        preserve_cleanup(&mut outcome, Err(Box::new(Arc::new(11))));
        let error = outcome.unwrap().unwrap_err();
        assert!(Arc::ptr_eq(
            &error.downcast_ref::<Original>().unwrap().0,
            &original
        ));
        let first = Arc::new(22);
        let mut outcome: Outcome<i32> = Err(Box::new(first.clone()));
        preserve_cleanup(
            &mut outcome,
            Ok(Err(io::Error::other("cleanup error").into())),
        );
        assert!(Arc::ptr_eq(
            outcome.unwrap_err().downcast_ref::<Arc<i32>>().unwrap(),
            &first
        ));
        let mut outcome: Outcome<i32> = Ok(Ok(0));
        preserve_cleanup(
            &mut outcome,
            Ok(Err(io::Error::other("cleanup error").into())),
        );
        assert_eq!(outcome.unwrap().unwrap_err().to_string(), "cleanup error");
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn canceled_signal_join_retains_workers_until_they_are_stopped() {
        use std::{
            future::Future,
            sync::atomic::{AtomicBool, Ordering},
            task::Poll,
        };
        struct Marker(Arc<AtomicBool>);
        impl Drop for Marker {
            fn drop(&mut self) {
                self.0.store(true, Ordering::SeqCst);
            }
        }
        let dropped = Arc::new(AtomicBool::new(false));
        let marker = Marker(dropped.clone());
        let (ready, started) = tokio::sync::oneshot::channel();
        let mut forwarders = SignalForwarders::default();
        forwarders.workers.push(tokio::spawn(async move {
            let _marker = marker;
            ready.send(()).unwrap();
            std::future::pending::<()>().await;
        }));
        started.await.unwrap();
        let mut stop = Box::pin(forwarders.stop());
        std::future::poll_fn(|cx| {
            assert!(stop.as_mut().poll(cx).is_pending());
            Poll::Ready(())
        })
        .await;
        drop(stop);
        assert_eq!(forwarders.workers.len(), 1);
        forwarders.stop().await;
        assert!(forwarders.workers.is_empty());
        assert!(dropped.load(Ordering::SeqCst));
    }

    #[cfg(unix)]
    #[test]
    fn signal_worker_panic_replays_only_after_all_workers_finish() {
        use std::sync::atomic::{AtomicBool, Ordering};
        struct Marker(Arc<AtomicBool>);
        impl Drop for Marker {
            fn drop(&mut self) {
                self.0.store(true, Ordering::SeqCst);
            }
        }
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_time()
            .build()
            .unwrap();
        let first = Arc::new(42);
        let dropped = Arc::new(AtomicBool::new(false));
        let mut forwarders = SignalForwarders::default();
        let failure = catch_unwind(AssertUnwindSafe(|| {
            runtime.block_on(async {
                let marker = Marker(dropped.clone());
                forwarders.workers.push(tokio::spawn(async move {
                    let _marker = marker;
                    std::future::pending::<()>().await;
                }));
                let original = first.clone();
                let panicked = tokio::spawn(async move { std::panic::panic_any(original) });
                tokio::time::timeout(std::time::Duration::from_secs(5), async {
                    while !panicked.is_finished() {
                        tokio::task::yield_now().await;
                    }
                })
                .await
                .expect("signal worker did not finish its panic");
                forwarders.workers.push(panicked);
                forwarders.stop().await;
            })
        }))
        .unwrap_err();
        assert!(forwarders.workers.is_empty());
        assert!(dropped.load(Ordering::SeqCst));
        assert!(Arc::ptr_eq(
            failure.downcast_ref::<Arc<i32>>().unwrap(),
            &first
        ));
    }

    #[test]
    fn cli_loader_preserves_text_module_inputs() {
        let store = create_cli_store(ResourceBudget::unlimited());
        let module = load_or_compile_module(
            &store,
            br#"(module (memory (export "memory") 1) (func (export "_start")))"#,
        )
        .unwrap();
        assert!(module.exports().any(|export| export.name() == "_start"));
    }

    #[tokio::test]
    async fn capture_keeps_only_bytes_accepted_by_the_host() {
        struct Partial;
        impl ByteSink for Partial {
            fn write<'a>(&'a self, data: &'a [u8]) -> LocalFuture<'a, io::Result<usize>> {
                Box::pin(async move { Ok(data.len().min(2)) })
            }
        }
        let sink = CaptureSink::new(Arc::new(Partial));
        assert_eq!(sink.write(b"abcd").await.unwrap(), 2);
        assert_eq!(sink.write(b"cd").await.unwrap(), 2);
        assert_eq!(sink.captured().unwrap(), "abcd");
        assert_eq!(sink.write(&[0xff]).await.unwrap(), 1);
        assert!(sink.captured().is_err());
    }

    #[tokio::test]
    async fn capture_reports_host_output_errors_even_if_the_guest_ignores_them() {
        struct Broken;
        impl ByteSink for Broken {
            fn write<'a>(&'a self, _: &'a [u8]) -> LocalFuture<'a, io::Result<usize>> {
                Box::pin(async {
                    Err(io::Error::new(
                        io::ErrorKind::BrokenPipe,
                        "capture output closed",
                    ))
                })
            }
        }
        let sink = CaptureSink::new(Arc::new(Broken));
        assert_eq!(
            sink.write(b"hello").await.unwrap_err().kind(),
            io::ErrorKind::BrokenPipe
        );
        let error = sink.captured().unwrap_err();
        assert!(
            error
                .chain()
                .any(|error| error.to_string() == "capture output closed")
        );
    }
}
