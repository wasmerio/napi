use anyhow::{Context, Result};
use std::io;
use std::path::{Component, Path, PathBuf};
use std::sync::{Arc, Mutex};
#[cfg(unix)]
use wasixv2::terminal::HostTerminal;
use wasixv2::{
    ByteSink, EmptySource, Executor, Host, Image, LoadError, Loader, LocalFuture, Machine,
    MachineBuilder, ProcessConfig, Stdio, ThreadStart, TokioHost,
};
#[cfg(feature = "cli-llvm")]
use wasmer::sys::LLVM;
use wasmer::sys::{Cranelift, EngineBuilder, Features};
use wasmer::{Engine, Module, Store};
use wasmer_c_api_imports_wasixv2::WasmCapiInstantiationHook;
use wasmer_cache::{Cache, FileSystemCache, Hash as CacheHash};
use wasmer_types::ModuleHash;

use crate::{NapiCtx, NapiHook, budget::ResourceBudget, budget::budgeted_tunables};

#[derive(Debug, Clone)]
pub struct GuestMount {
    pub host_path: PathBuf,
    pub guest_path: PathBuf,
}

pub struct LoadedWasm {
    pub store: Store,
    pub module: Module,
    pub module_hash: ModuleHash,
}

#[derive(Clone, Copy)]
enum CliCompiler {
    Cranelift,
    #[cfg(feature = "cli-llvm")]
    Llvm,
}

impl CliCompiler {
    fn from_env() -> Result<Self> {
        match std::env::var("WASMER_NAPI_COMPILER") {
            Ok(name) => match name.as_str() {
                "cranelift" => Ok(Self::Cranelift),
                #[cfg(feature = "cli-llvm")]
                "llvm" => Ok(Self::Llvm),
                #[cfg(not(feature = "cli-llvm"))]
                "llvm" => anyhow::bail!("LLVM requires building with --features cli-llvm"),
                _ => anyhow::bail!("WASMER_NAPI_COMPILER must be cranelift or llvm"),
            },
            Err(std::env::VarError::NotPresent) => {
                #[cfg(feature = "cli-llvm")]
                return Ok(Self::Llvm);
                #[cfg(not(feature = "cli-llvm"))]
                return Ok(Self::Cranelift);
            }
            Err(error) => Err(error.into()),
        }
    }

    fn name(self) -> &'static str {
        match self {
            Self::Cranelift => "cranelift",
            #[cfg(feature = "cli-llvm")]
            Self::Llvm => "llvm",
        }
    }
}

fn create_cli_store(budget: Arc<ResourceBudget>, compiler: CliCompiler) -> Store {
    let mut features = Features::default();
    features.exceptions(true);

    let builder = match compiler {
        CliCompiler::Cranelift => EngineBuilder::new(Cranelift::default()),
        #[cfg(feature = "cli-llvm")]
        CliCompiler::Llvm => EngineBuilder::new(LLVM::default()),
    };
    let mut engine = builder.set_features(Some(features)).engine();
    // Charge the guest's (imported) wasm linear memory against the app budget.
    // The machine instantiates the guest with this engine, so its budgeted
    // tunables cover the main store and every thread's store made from it.
    let tunables = budgeted_tunables(budget);
    engine.set_tunables(tunables);
    Store::new(engine)
}

fn wasmer_cache_dir(compiler: CliCompiler) -> PathBuf {
    std::env::var_os("WASMER_NAPI_CACHE_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| std::env::temp_dir().join("wasmer-napi-cache"))
        .join(compiler.name())
}

fn load_or_compile_module(
    store: &Store,
    wasm_bytes: &[u8],
    compiler: CliCompiler,
) -> Result<Module> {
    load_or_compile_module_in(store, wasm_bytes, &wasmer_cache_dir(compiler))
}

/// Compiles `wasm_bytes` as the machine runs it, or loads that from the cache
/// in `cache_dir`.
///
/// The machine does not run a module as it was built: it adds the preemption
/// points that let a busy guest see signals and its group's exit, replaces
/// atomic waits and `memory.grow` with its own, and defers the start function
/// until the instance is bound. A dynamically linked main module is rewritten
/// before that. What is compiled, and cached, is the result, which is what
/// `wasixv2::load` compiles too.
fn load_or_compile_module_in(store: &Store, wasm_bytes: &[u8], cache_dir: &Path) -> Result<Module> {
    let binary = wasmer::wat2wasm(wasm_bytes).context("failed to parse wasm module")?;
    let prepared =
        wasixv2_dylink::prepare_image(&binary).context("failed to prepare wasm module")?;
    let (compiler_input, not_preemptible) = if prepared == binary.as_ref() {
        wasixv2::prepare_for_compile(&prepared).context("failed to prepare wasm module")?
    } else {
        (
            wasixv2::instrument(&prepared).context("failed to prepare wasm module")?,
            None,
        )
    };
    // The key names what the compiler was given, so a module prepared by
    // another version of the machine is compiled again.
    let key = CacheHash::generate(&compiler_input);
    let mut cache = FileSystemCache::new(cache_dir)
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
    if let Some(error) = not_preemptible {
        wasixv2::warn_not_preemptible(&error);
    }
    Ok(module)
}

/// Compiles the images the guest `execve`s the way its main module was.
struct CliLoader {
    engine: Engine,
    compiler: CliCompiler,
}

impl Loader for CliLoader {
    fn load<'a>(
        &'a self,
        _: &'a Engine,
        image: &'a Image,
    ) -> LocalFuture<'a, std::result::Result<wasixv2::Loaded, LoadError>> {
        Box::pin(async move {
            // The engine of the main module: its budgeted memories and its
            // compiler features.
            let store = Store::new(self.engine.clone());
            let module = load_or_compile_module(&store, image.bytes(), self.compiler)
                .map_err(LoadError::other)?;
            let (engine, prepared) = (self.engine.clone(), false);
            Ok(wasixv2::Loaded {
                engine,
                module,
                prepared,
            })
        })
    }
}

/// Passes what the guest writes on to `output` and keeps a copy.
struct CaptureSink {
    output: Arc<dyn ByteSink>,
    /// Held across a write and its copy, so that concurrent writes are
    /// captured in the order they reached the host.
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

    /// What the guest wrote, or the first error that writing it on met: the
    /// guest may have ignored it.
    fn captured(&self) -> Result<String> {
        let poisoned = || anyhow::anyhow!("stdio capture mutex poisoned");
        if let Some(error) = self.error.lock().map_err(|_| poisoned())?.take() {
            return Err(error).context("failed writing drained WASIX stdio");
        }
        let bytes = self.bytes.lock().map_err(|_| poisoned())?;
        String::from_utf8(bytes.clone()).context("WASIX stdio was not valid UTF-8")
    }
}

impl ByteSink for CaptureSink {
    fn write<'a>(&'a self, data: &'a [u8]) -> LocalFuture<'a, io::Result<usize>> {
        Box::pin(async move {
            let poisoned = || io::Error::other("stdio capture mutex poisoned");
            let _writing = self.writing.lock().await;
            let written = match self.output.write(data).await {
                Ok(written) => written,
                Err(error) => {
                    let kind = error.kind();
                    self.error
                        .lock()
                        .map_err(|_| poisoned())?
                        .get_or_insert(error);
                    return Err(kind.into());
                }
            };
            self.bytes
                .lock()
                .map_err(|_| poisoned())?
                .extend_from_slice(&data[..written]);
            Ok(written)
        })
    }
}

pub fn load_wasix_module(wasm_path: &Path) -> Result<LoadedWasm> {
    load_wasix_module_with_budget(wasm_path, ResourceBudget::unlimited())
}

pub fn load_wasix_module_with_budget(
    wasm_path: &Path,
    budget: Arc<ResourceBudget>,
) -> Result<LoadedWasm> {
    load_wasix_module_with_compiler(wasm_path, budget, CliCompiler::from_env()?)
}

fn load_wasix_module_with_compiler(
    wasm_path: &Path,
    budget: Arc<ResourceBudget>,
    compiler: CliCompiler,
) -> Result<LoadedWasm> {
    let wasm_bytes = std::fs::read(wasm_path)
        .with_context(|| format!("failed to read wasm file at {}", wasm_path.display()))?;

    let store = create_cli_store(budget, compiler);

    let module = load_or_compile_module(&store, &wasm_bytes, compiler)?;

    let module_hash = ModuleHash::sha256(&wasm_bytes);

    Ok(LoadedWasm {
        store,
        module,
        module_hash,
    })
}

/// The guest path of each mount without `.` and `..`, which must not lead
/// above the root.
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

/// Mounts host directories in a machine that has run nothing yet.
///
/// A mount at `/` becomes a layer below the machine's writable root: the guest
/// reads the host directory and its changes stay in the machine. The first of
/// several is on top. Every other mount is the host directory itself, which
/// the guest writes to.
pub async fn configure_runner_mounts(
    machine: &mut Machine,
    _wasm_path: &Path,
    extra_mounts: &[GuestMount],
) -> Result<()> {
    let mounts = normalized_mounts(extra_mounts)?;
    // The machine puts each later layer on top of the earlier ones.
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

/// Runs the guest's `_start` and returns its exit code with what it wrote to
/// stdout and stderr, which also goes to the host's own.
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

/// The stack of a thread that runs a guest.
///
/// The machine polls a guest from deep inside its own futures, and V8 runs on
/// that same stack below them, with up to a megabyte of JavaScript frames. A
/// thread of Rust's default 2 MiB overflows before the guest's first import
/// in a debug build. The pages are only committed as they are used.
const GUEST_STACK_BYTES: usize = 64 * 1024 * 1024;

/// Runs the machine's guest threads on stacks of [`GUEST_STACK_BYTES`], and
/// everything else as [`TokioHost`] does.
struct GuestThreads {
    tokio: TokioHost,
}

impl Executor for GuestThreads {
    fn spawn_local(&self, future: LocalFuture<'static, ()>) {
        self.tokio.executor().spawn_local(future);
    }

    fn spawn_blocking(&self, work: Box<dyn FnOnce() + Send>) -> LocalFuture<'static, ()> {
        self.tokio.executor().spawn_blocking(work)
    }

    fn spawn_thread(&self, start: ThreadStart) -> io::Result<()> {
        // The runtime of the thread that asks, which is the multi-thread one
        // this module builds: its workers drive I/O for every guest thread.
        let runtime = tokio::runtime::Handle::try_current().map_err(io::Error::other)?;
        std::thread::Builder::new()
            .name("wasixv2-guest".into())
            .stack_size(GUEST_STACK_BYTES)
            .spawn(move || {
                // A guest's futures are not `Send`: they stay on this thread.
                let local = tokio::task::LocalSet::new();
                runtime.block_on(local.run_until(start()));
            })?;
        Ok(())
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
    // The guest's main thread is one this function makes, so that it has the
    // stack a guest needs whatever thread the caller is on.
    std::thread::scope(|scope| {
        let main = std::thread::Builder::new()
            .name("wasixv2-guest".into())
            .stack_size(GUEST_STACK_BYTES)
            .spawn_scoped(scope, || {
                drive_machine(
                    ctx,
                    wasm_path,
                    extra_mounts,
                    config,
                    current_dir,
                    use_system_tty,
                )
            })
            .context("failed to start the thread that runs the guest")?;
        main.join()
            .unwrap_or_else(|panic| std::panic::resume_unwind(panic))
    })
}

fn drive_machine(
    ctx: &NapiCtx,
    wasm_path: &Path,
    extra_mounts: &[GuestMount],
    config: ProcessConfig,
    current_dir: Option<&Path>,
    use_system_tty: bool,
) -> Result<i32> {
    // A multi-thread runtime: its workers complete the guest's I/O and timers
    // while a guest thread waits in a callback that JavaScript called.
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("failed to create tokio runtime for WASIX")?;
    // The machine runs its guests on the thread that drives it, in a local
    // set: their futures are not `Send`.
    let local = tokio::task::LocalSet::new();
    let exit_code = runtime.block_on(local.run_until(run_in_machine(
        ctx,
        wasm_path,
        extra_mounts,
        config,
        current_dir,
        use_system_tty,
    )));
    drop(local);
    if use_system_tty {
        // A host stdin read may still occupy the blocking pool after the guest
        // closes its terminal. Waiting for that OS read while dropping the
        // runtime would keep the standalone CLI alive even though the guest
        // has exited.
        runtime.shutdown_background();
    }
    exit_code
}

async fn run_in_machine(
    ctx: &NapiCtx,
    wasm_path: &Path,
    extra_mounts: &[GuestMount],
    config: ProcessConfig,
    current_dir: Option<&Path>,
    use_system_tty: bool,
) -> Result<i32> {
    let compiler = CliCompiler::from_env()?;
    let loaded = load_wasix_module_with_compiler(wasm_path, ctx.budget(), compiler)?;
    let engine = loaded.store.engine().clone();
    let module = loaded.module;

    let host: Arc<dyn Host> = Arc::new(TokioHost::default());
    let builder = MachineBuilder::new(host)
        .task_executor(Arc::new(GuestThreads {
            tokio: TokioHost::default(),
        }))
        .loader(Arc::new(CliLoader {
            engine: engine.clone(),
            compiler,
        }))
        .instantiation_hook(Arc::new(NapiHook::new(ctx.clone())))
        .instantiation_hook(Arc::new(WasmCapiInstantiationHook::default()));
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
    #[cfg(not(unix))]
    let _ = use_system_tty;

    let mut machine = builder.build();
    if let Some(current_dir) = current_dir {
        machine.set_working_dir(
            current_dir
                .to_str()
                .context("guest working directory was not UTF-8")?,
        );
    }
    // Before the signaller: it seals the machine's filesystem configuration.
    configure_runner_mounts(&mut machine, wasm_path, extra_mounts).await?;

    // Both end with this scope, however it is left: the terminal gets its
    // settings back and the host's SIGINT its default.
    #[cfg(unix)]
    let _interrupts = InterruptForwarder::install(machine.signaller())
        .context("cannot install signal handlers")?;
    #[cfg(unix)]
    let _raw = terminal
        .as_ref()
        .map(HostTerminal::raw)
        .transpose()
        .context("cannot put the terminal in raw mode")?;

    let exit_code = machine
        .run_module(&engine, &module, config)
        .await
        .context("failed to run WASIX module through the machine");
    machine.shutdown().await;
    exit_code
}

/// Sends the host's SIGINT to the guest instead of ending the host with it,
/// until dropped.
#[cfg(unix)]
struct InterruptForwarder(tokio::task::JoinHandle<()>);

#[cfg(unix)]
impl InterruptForwarder {
    fn install(signaller: wasixv2::Signaller) -> io::Result<Self> {
        use tokio::signal::unix::{SignalKind, signal};

        let mut interrupts = signal(SignalKind::interrupt())?;
        Ok(Self(tokio::spawn(async move {
            while interrupts.recv().await.is_some() {
                let _ = signaller.signal(2);
            }
        })))
    }
}

#[cfg(unix)]
impl Drop for InterruptForwarder {
    fn drop(&mut self) {
        self.0.abort();
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
    fn mounts_are_normalized_and_cannot_leave_the_root() {
        let mount = |guest: &str| GuestMount {
            host_path: PathBuf::from("/host"),
            guest_path: PathBuf::from(guest),
        };
        let normalized = normalized_mounts(&[mount("/a/./b/../c"), mount("relative")]).unwrap();
        assert_eq!(normalized[0].guest_path, Path::new("/a/c"));
        assert_eq!(normalized[1].guest_path, Path::new("/relative"));
        assert!(normalized_mounts(&[mount("/a/../..")]).is_err());
    }

    #[test]
    fn the_loader_accepts_text_modules_and_caches_them() {
        let store = create_cli_store(ResourceBudget::unlimited(), CliCompiler::Cranelift);
        let cache = std::env::temp_dir().join(format!(
            "wasmer-napi-cli-test-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        let text = br#"(module (memory (export "memory") 1) (func (export "_start")))"#;
        for _ in 0..2 {
            let module = load_or_compile_module_in(&store, text, &cache).unwrap();
            assert!(module.exports().any(|export| export.name() == "_start"));
        }
        assert!(std::fs::read_dir(&cache).unwrap().next().is_some());
        let _ = std::fs::remove_dir_all(&cache);
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
