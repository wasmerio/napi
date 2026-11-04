use std::{
    env,
    io::Read,
    path::{Path, PathBuf},
};

const PREBUILT_V8_VERSION: &str = "11.9.7";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum V8Method {
    Local,
    Prebuilt,
    Source,
}

#[allow(unused)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum V8Origin {
    Local,
    Prebuilt,
}

#[derive(Debug, Clone)]
struct V8Config {
    include_dir: PathBuf,
    library_path: PathBuf,
    extra_links: Vec<ExtraLink>,
    #[allow(unused)]
    origin: V8Origin,
}

#[derive(Debug, Clone)]
enum ExtraLink {
    LibraryPath(PathBuf),
    Framework(PathBuf),
    Name(String),
}

fn main() {
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
    if target_arch == "wasm32" {
        if env::var_os("CARGO_FEATURE_JS").is_none() {
            panic!("wasmer-napi on wasm32 requires the `js` feature");
        }
        // The wasm32 backend is implemented in Rust with wasm-bindgen and must
        // not download, build, or link the native V8 bridge.
        return;
    }

    println!("cargo:rerun-if-changed=src/napi_bridge_init.cc");
    println!("cargo:rerun-if-changed=include");
    println!("cargo:rerun-if-changed=lib/src");
    println!("cargo:rerun-if-changed=v8/src");
    println!("cargo:rerun-if-env-changed=OBJCOPY");
    println!("cargo:rerun-if-env-changed=NM");
    println!("cargo:rerun-if-env-changed=V8_INCLUDE_DIR");
    println!("cargo:rerun-if-env-changed=V8_LIB_DIR");
    println!("cargo:rerun-if-env-changed=V8_DEFINES");
    println!("cargo:rerun-if-env-changed=NAPI_V8_BUILD_METHOD");
    println!("cargo:rerun-if-env-changed=NAPI_V8_DEFINES");
    println!("cargo:rerun-if-env-changed=NAPI_V8_EXTRA_LIBS");
    println!("cargo:rerun-if-env-changed=NAPI_V8_FORCE_LOCAL_BUILD");
    println!("cargo:rerun-if-env-changed=NAPI_V8_INCLUDE_DIR");
    println!("cargo:rerun-if-env-changed=NAPI_V8_LIBRARY");
    println!("cargo:rerun-if-env-changed=NAPI_V8_V8_DEFINES");
    println!("cargo:rerun-if-env-changed=NAPI_V8_V8_EXTRA_LIBS");
    println!("cargo:rerun-if-env-changed=NAPI_V8_V8_INCLUDE_DIR");
    println!("cargo:rerun-if-env-changed=NAPI_V8_V8_LIBRARY");
    println!("cargo:rerun-if-env-changed=NAPI_V8_V8_MONOLITH_LIB");

    let manifest_dir =
        PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR is not set"));
    let napi_include = manifest_dir.join("include");
    let napi_lib_src = manifest_dir.join("lib/src");
    let napi_v8_src = manifest_dir.join("v8/src");
    let mut v8 = resolve_v8_config().unwrap_or_else(|err| panic!("{err}"));
    assert!(
        v8.include_dir.join("v8.h").exists(),
        "V8 headers not found in {}",
        v8.include_dir.display()
    );
    assert!(
        v8.library_path.exists(),
        "V8 library not found at {}",
        v8.library_path.display()
    );

    let v8_defines = read_env_value("V8_DEFINES", &["NAPI_V8_DEFINES", "NAPI_V8_V8_DEFINES"])
        .unwrap_or_else(|| "V8_COMPRESS_POINTERS".to_string());

    let mut build = cc::Build::new();
    // This private layout/stack-start contract is audited only for the pinned
    // Linux-amd64 prebuilt. Explicit/local builds keep ordinary native entry
    // and decline coroutine entry rather than assuming their GC/build flags.
    if v8.origin == V8Origin::Prebuilt
        && target_arch == "x86_64"
        && env::var("CARGO_CFG_TARGET_POINTER_WIDTH").as_deref() == Ok("64")
        && env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("linux")
        && env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("gnu")
        && PREBUILT_V8_VERSION == "11.9.7"
    {
        v8.library_path = adapt_pinned_v8_archive(&v8.library_path, &build)
            .unwrap_or_else(|error| panic!("{error}"));
        build.define("SNAPI_PINNED_V8_STACK_LIMIT", Some("1"));
        if env::var_os("CARGO_FEATURE_NATIVE_STACK_DIAGNOSTICS").is_some() {
            build.define("SNAPI_NATIVE_STACK_DIAGNOSTICS", Some("1"));
        }
    }
    build
        .cpp(true)
        .flag_if_supported("-std=c++20")
        .flag_if_supported("-fno-rtti")
        .flag_if_supported("-w")
        .define("NAPI_EXTERN", Some(""))
        .include(&v8.include_dir)
        .include(napi_include.to_str().unwrap())
        .include(napi_lib_src.to_str().unwrap())
        .include(napi_v8_src.to_str().unwrap())
        .file("src/napi_bridge_init.cc")
        .file(napi_lib_src.join("napi_error_state.cc").to_str().unwrap())
        .file(
            napi_lib_src
                .join("napi_lifetime_tracker.cc")
                .to_str()
                .unwrap(),
        )
        .file(napi_lib_src.join("napi_buffer_lease.cc").to_str().unwrap())
        .file(napi_lib_src.join("napi_periodic_gate.cc").to_str().unwrap())
        .file(napi_lib_src.join("napi_text.cc").to_str().unwrap())
        .file(
            napi_lib_src
                .join("napi_typedarray_metadata.cc")
                .to_str()
                .unwrap(),
        )
        .file(napi_v8_src.join("js_native_api_v8.cc").to_str().unwrap())
        .file(
            napi_v8_src
                .join("internal/napi_escapable_handle_scope_wrapper.cc")
                .to_str()
                .unwrap(),
        )
        .file(
            napi_v8_src
                .join("internal/napi_external_wrapper.cc")
                .to_str()
                .unwrap(),
        )
        .file(
            napi_v8_src
                .join("internal/napi_function_callback_info.cc")
                .to_str()
                .unwrap(),
        )
        .file(
            napi_v8_src
                .join("internal/napi_getter_callback_info.cc")
                .to_str()
                .unwrap(),
        )
        .file(
            napi_v8_src
                .join("internal/napi_handle_scope_wrapper.cc")
                .to_str()
                .unwrap(),
        )
        .file(
            napi_v8_src
                .join("internal/napi_lifetime_tracker.cc")
                .to_str()
                .unwrap(),
        )
        .file(napi_v8_src.join("internal/napi_ref.cc").to_str().unwrap())
        .file(
            napi_v8_src
                .join("internal/napi_ref_tracker.cc")
                .to_str()
                .unwrap(),
        )
        .file(
            napi_v8_src
                .join("internal/napi_ref_with_data.cc")
                .to_str()
                .unwrap(),
        )
        .file(
            napi_v8_src
                .join("internal/napi_ref_with_finalizer.cc")
                .to_str()
                .unwrap(),
        )
        .file(
            napi_v8_src
                .join("internal/napi_setter_callback_info.cc")
                .to_str()
                .unwrap(),
        )
        .file(napi_v8_src.join("unofficial_napi.cc").to_str().unwrap())
        .file(
            napi_v8_src
                .join("unofficial_napi_error_utils.cc")
                .to_str()
                .unwrap(),
        )
        .file(
            napi_v8_src
                .join("unofficial_napi_contextify.cc")
                .to_str()
                .unwrap(),
        )
        .file(napi_v8_src.join("edge_v8_platform.cc").to_str().unwrap());

    for raw in v8_defines.split(&[';', ',', ' '][..]) {
        let entry = raw.trim();
        if entry.is_empty() {
            continue;
        }
        if let Some((name, value)) = entry.split_once('=') {
            build.define(name.trim(), Some(value.trim()));
        } else {
            build.define(entry, Some("1"));
        }
    }

    build.compile("napi_bridge");

    emit_library_link(&v8.library_path);
    for extra_link in &v8.extra_links {
        emit_extra_link(extra_link);
    }

    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "macos" || target_os == "ios" {
        println!("cargo:rustc-link-lib=dylib=c++");
    } else {
        println!("cargo:rustc-link-lib=dylib=stdc++");
        println!("cargo:rustc-link-lib=dylib=dl");
        println!("cargo:rustc-link-lib=dylib=m");
        println!("cargo:rustc-link-lib=dylib=pthread");
        println!("cargo:rustc-link-lib=dylib=rt");
    }
}

const PINNED_V8_ARCHIVE_SHA256: &str =
    "c58981886d0aabc93eae4da63ea6f8ce253603f95efe26d604aaaf185ab8109a";
const PINNED_STACK_OBJECT_SHA256: &str =
    "f48ebc83f7693073b85889b843649cca1d5c6192ec0c2cb5731b6c9eb28ecacc";
const STACK_SCANNER_SYMBOL: &str =
    "_ZNK4heap4base5Stack26IteratePointersUntilMarkerEPNS0_12StackVisitorE";
const ORIGINAL_STACK_SCANNER_SYMBOL: &str = "snapi_original_v8_stack_iterate_pointers_until_marker";

fn sha256(bytes: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    use std::fmt::Write as _;

    let mut hex = String::with_capacity(64);
    for byte in Sha256::digest(bytes) {
        write!(&mut hex, "{byte:02x}").expect("formatting into a String cannot fail");
    }
    hex
}

// The pinned archive uses GNU ar. Preserve every data member byte-for-byte;
// only its symbol index and the one audited stack.o member may change.
fn archive_members(bytes: &[u8]) -> Result<Vec<(&str, &[u8])>, String> {
    if !bytes.starts_with(b"!<arch>\n") {
        return Err("pinned V8 must be an ordinary GNU archive".into());
    }
    let mut position = 8usize;
    let mut members = Vec::new();
    while position < bytes.len() {
        let header = bytes
            .get(position..position + 60)
            .ok_or("truncated V8 archive member")?;
        if &header[58..60] != b"`\n" {
            return Err("invalid V8 archive member header".into());
        }
        let name = std::str::from_utf8(&header[..16])
            .map_err(|_| "invalid archive member name")?
            .trim_end();
        let size: usize = std::str::from_utf8(&header[48..58])
            .map_err(|_| "invalid archive member length")?
            .trim()
            .parse()
            .map_err(|_| "invalid archive member length")?;
        let start = position.checked_add(60).ok_or("archive offset overflow")?;
        let end = start.checked_add(size).ok_or("archive member overflow")?;
        let data = bytes
            .get(start..end)
            .ok_or("truncated archive member data")?;
        if name != "/" && name != "/SYM64/" {
            members.push((name, data));
        }
        position = end
            .checked_add(size % 2)
            .ok_or("archive padding overflow")?;
    }
    if position != bytes.len() {
        return Err("invalid V8 archive padding".into());
    }
    Ok(members)
}

fn tool_output(command: &mut std::process::Command) -> Result<Vec<u8>, String> {
    let result = command
        .output()
        .map_err(|error| format!("could not run {command:?}: {error}"))?;
    if !result.status.success() {
        return Err(format!(
            "{command:?} failed: {}",
            String::from_utf8_lossy(&result.stderr)
        ));
    }
    Ok(result.stdout)
}

fn adapt_pinned_v8_archive(source: &Path, build: &cc::Build) -> Result<PathBuf, String> {
    let source_bytes = std::fs::read(source).map_err(|error| error.to_string())?;
    if sha256(&source_bytes) != PINNED_V8_ARCHIVE_SHA256 {
        return Err("V8 coroutine stack support requires re-audit of this archive".into());
    }
    let before = archive_members(&source_bytes)?;
    let stack_members: Vec<_> = before
        .iter()
        .filter(|(name, _)| *name == "stack.o/")
        .collect();
    if stack_members.len() != 1 || sha256(stack_members[0].1) != PINNED_STACK_OBJECT_SHA256 {
        return Err("audited V8 stack.o member is missing or changed".into());
    }
    let directory =
        PathBuf::from(env::var_os("OUT_DIR").ok_or("OUT_DIR is not set")?).join("v8-native-stack");
    std::fs::create_dir_all(&directory).map_err(|error| error.to_string())?;
    let original_object = directory.join("stack-original.o");
    let adapted_object = directory.join("stack.o");
    let destination = directory.join("libv8_snapi_stack.a");
    std::fs::write(&original_object, stack_members[0].1).map_err(|error| error.to_string())?;
    std::fs::copy(source, &destination).map_err(|error| error.to_string())?;

    let objcopy = env::var_os("OBJCOPY").unwrap_or_else(|| "objcopy".into());
    tool_output(
        std::process::Command::new(objcopy)
            .arg("--redefine-sym")
            .arg(format!(
                "{STACK_SCANNER_SYMBOL}={ORIGINAL_STACK_SCANNER_SYMBOL}"
            ))
            .arg(&original_object)
            .arg(&adapted_object),
    )?;
    // stack.o contains no relocation to its scanner symbol (including its
    // inlined testing-only loop); all production references are other members.
    let nm = env::var_os("NM").unwrap_or_else(|| "nm".into());
    let original_symbols = tool_output(std::process::Command::new(&nm).arg(&original_object))?;
    let adapted_symbols = tool_output(std::process::Command::new(&nm).arg(&adapted_object))?;
    let original_symbols =
        String::from_utf8(original_symbols).map_err(|error| error.to_string())?;
    let adapted_symbols = String::from_utf8(adapted_symbols).map_err(|error| error.to_string())?;
    if original_symbols.matches(STACK_SCANNER_SYMBOL).count() != 1 {
        return Err("unexpected scanner definition/reference in audited stack.o".into());
    }
    // nm sorts by symbol name; compare sorted lines after the one exact rename.
    let expected = original_symbols.replace(STACK_SCANNER_SYMBOL, ORIGINAL_STACK_SCANNER_SYMBOL);
    let mut expected: Vec<_> = expected.lines().collect();
    let mut actual: Vec<_> = adapted_symbols.lines().collect();
    expected.sort_unstable();
    actual.sort_unstable();
    if expected != actual {
        return Err("V8 scanner transformation changed unrelated symbols".into());
    }

    let mut replace = build.get_archiver();
    replace.arg("r").arg(&destination).arg(&adapted_object);
    tool_output(&mut replace)?;
    let mut index = build.get_archiver();
    index.arg("s").arg(&destination);
    tool_output(&mut index)?;
    let output_bytes = std::fs::read(&destination).map_err(|error| error.to_string())?;
    let after = archive_members(&output_bytes)?;
    if before.len() != after.len() {
        return Err("V8 scanner transformation changed archive membership".into());
    }
    let adapted_bytes = std::fs::read(&adapted_object).map_err(|error| error.to_string())?;
    for ((old_name, old), (new_name, new)) in before.iter().zip(&after) {
        if old_name != new_name
            || if *old_name == "stack.o/" {
                *new != adapted_bytes
            } else {
                old != new
            }
        {
            return Err(format!(
                "V8 scanner transformation changed unrelated member {old_name}"
            ));
        }
    }
    if sha256(&std::fs::read(source).map_err(|error| error.to_string())?)
        != PINNED_V8_ARCHIVE_SHA256
    {
        return Err("source V8 archive changed during private adaptation".into());
    }
    Ok(destination)
}

fn resolve_v8_config() -> Result<V8Config, String> {
    let include_override = read_env_value(
        "NAPI_V8_INCLUDE_DIR",
        &["V8_INCLUDE_DIR", "NAPI_V8_V8_INCLUDE_DIR"],
    );
    let library_override = read_env_value(
        "NAPI_V8_LIBRARY",
        &["NAPI_V8_V8_LIBRARY", "NAPI_V8_V8_MONOLITH_LIB"],
    );
    let lib_dir_override = env::var("V8_LIB_DIR")
        .ok()
        .filter(|value| !value.trim().is_empty());
    let extra_override = read_env_value("NAPI_V8_EXTRA_LIBS", &["NAPI_V8_V8_EXTRA_LIBS"]);

    if include_override.is_some() || library_override.is_some() || lib_dir_override.is_some() {
        return resolve_explicit_v8(
            include_override,
            library_override,
            lib_dir_override,
            extra_override,
        );
    }

    let requested_method = requested_v8_method();
    match requested_method {
        V8Method::Local => resolve_local_v8(extra_override.as_deref()).ok_or_else(|| {
            "local V8 requested but no supported installation was found".to_string()
        }),
        V8Method::Prebuilt => {
            resolve_prebuilt_v8(extra_override.as_deref()).or_else(|prebuilt_err| {
                resolve_local_v8(extra_override.as_deref()).ok_or_else(|| {
                    format!("failed to resolve V8 via prebuilt or local strategies: {prebuilt_err}")
                })
            })
        }
        V8Method::Source => Err(
            "NAPI_V8_BUILD_METHOD=source is not supported by napi cargo build.rs yet".to_string(),
        ),
    }
}

fn resolve_explicit_v8(
    include_override: Option<String>,
    library_override: Option<String>,
    lib_dir_override: Option<String>,
    extra_override: Option<String>,
) -> Result<V8Config, String> {
    let include_dir = include_override
        .map(PathBuf::from)
        .ok_or_else(|| "V8 include directory is not set. Set NAPI_V8_INCLUDE_DIR.".to_string())?;

    let library_path = match (library_override, lib_dir_override) {
        (Some(path), _) => PathBuf::from(path),
        (None, Some(dir)) => resolve_primary_library(&PathBuf::from(dir))
            .ok_or_else(|| "V8 library not found in V8_LIB_DIR.".to_string())?,
        (None, None) => {
            return Err("V8 library path is not set. Set NAPI_V8_LIBRARY.".to_string());
        }
    };

    let extra_links = extra_override
        .as_deref()
        .map(parse_extra_links)
        .unwrap_or_else(|| default_local_extra_links(&library_path));

    Ok(V8Config {
        include_dir,
        library_path,
        extra_links,
        origin: V8Origin::Local,
    })
}

fn resolve_local_v8(extra_override: Option<&str>) -> Option<V8Config> {
    let mut candidates = Vec::new();
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "macos" {
        candidates.push(PathBuf::from("/opt/homebrew/Cellar/v8/14.5.201.9"));
        candidates.push(PathBuf::from("/opt/homebrew/opt/v8"));
    }

    for root in candidates {
        let include_dir = root.join("include");
        let library_path = root.join("lib/libv8.dylib");
        if include_dir.join("v8.h").exists() && library_path.exists() {
            let extra_links = extra_override
                .map(parse_extra_links)
                .unwrap_or_else(|| default_local_extra_links(&library_path));
            return Some(V8Config {
                include_dir,
                library_path,
                extra_links,
                origin: V8Origin::Local,
            });
        }
    }

    None
}

fn resolve_prebuilt_v8(extra_override: Option<&str>) -> Result<V8Config, String> {
    let (asset_name, platform_name) = prebuilt_asset_name()?;
    let out_dir = PathBuf::from(env::var("OUT_DIR").map_err(|err| err.to_string())?);
    let cache_dir = out_dir
        .join("v8-prebuilt")
        .join(PREBUILT_V8_VERSION)
        .join(platform_name);
    let archive_path = cache_dir.join(asset_name);

    std::fs::create_dir_all(&cache_dir)
        .map_err(|err| format!("failed to create {}: {err}", cache_dir.display()))?;

    if !cache_dir.join("include/v8.h").exists()
        || resolve_primary_library(&cache_dir.join("lib")).is_none()
    {
        download_prebuilt_archive(asset_name, &archive_path)?;
        unpack_prebuilt_archive(&archive_path, &cache_dir)?;
    }

    let include_dir = cache_dir.join("include");
    let library_dir = cache_dir.join("lib");
    let library_path = resolve_primary_library(&library_dir).ok_or_else(|| {
        format!(
            "prebuilt V8 archive did not produce an expected library under {}",
            library_dir.display()
        )
    })?;

    let extra_links = extra_override
        .map(parse_extra_links)
        .unwrap_or_else(default_prebuilt_extra_links);

    Ok(V8Config {
        include_dir,
        library_path,
        extra_links,
        origin: V8Origin::Prebuilt,
    })
}

fn requested_v8_method() -> V8Method {
    if env_truthy("NAPI_V8_FORCE_LOCAL_BUILD") {
        return V8Method::Local;
    }

    match env::var("NAPI_V8_BUILD_METHOD")
        .unwrap_or_else(|_| "prebuilt".to_string())
        .to_lowercase()
        .as_str()
    {
        "local" => V8Method::Local,
        "source" => V8Method::Source,
        _ => V8Method::Prebuilt,
    }
}

fn prebuilt_asset_name() -> Result<(&'static str, &'static str), String> {
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
    let target_env = env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();

    match (
        target_os.as_str(),
        target_arch.as_str(),
        target_env.as_str(),
    ) {
        ("macos", "aarch64", _) => Ok(("v8-darwin-aarch64.tar.xz", "darwin-arm64")),
        ("macos", "x86_64", _) => Ok(("v8-darwin-amd64.tar.xz", "darwin-amd64")),
        ("linux", "x86_64", "gnu") => Ok(("v8-linux-amd64.tar.xz", "linux-amd64")),
        ("linux", "x86_64", "musl") => Ok(("v8-linux-musl-amd64.tar.xz", "linux-musl-amd64")),
        ("android", "aarch64", _) => Ok(("v8-android-arm64.tar.xz", "android-arm64")),
        ("windows", "x86_64", _) => Ok(("v8-windows-amd64.tar.xz", "windows-amd64")),
        (os, arch, env_kind) => Err(format!(
            "unsupported target for prebuilt V8: os={os}, arch={arch}, env={env_kind}"
        )),
    }
}

fn download_prebuilt_archive(asset_name: &str, archive_path: &Path) -> Result<(), String> {
    let url = format!(
        "https://github.com/wasmerio/v8-custom-builds/releases/download/{PREBUILT_V8_VERSION}/{asset_name}"
    );
    let response = ureq::get(&url)
        .call()
        .map_err(|err| format!("failed to download {url}: {err}"))?;

    let mut reader = response.into_reader();
    let mut tar_data = Vec::new();
    reader
        .read_to_end(&mut tar_data)
        .map_err(|err| format!("failed to read response body from {url}: {err}"))?;

    std::fs::write(archive_path, tar_data)
        .map_err(|err| format!("failed to write {}: {err}", archive_path.display()))?;

    Ok(())
}

fn unpack_prebuilt_archive(archive_path: &Path, cache_dir: &Path) -> Result<(), String> {
    let tar_data = std::fs::read(archive_path)
        .map_err(|err| format!("failed to read {}: {err}", archive_path.display()))?;
    let tar = xz::read::XzDecoder::new(tar_data.as_slice());
    let mut archive = tar::Archive::new(tar);
    archive
        .unpack(cache_dir)
        .map_err(|err| format!("failed to unpack {}: {err}", archive_path.display()))
}

fn resolve_primary_library(dir: &Path) -> Option<PathBuf> {
    for candidate in [
        "libv8.a",
        "libv8.so",
        "libv8.dylib",
        "libv8_monolith.a",
        "libv8_monolith.so",
        "libv8_monolith.dylib",
    ] {
        let path = dir.join(candidate);
        if path.exists() {
            return Some(path);
        }
    }
    None
}

fn default_local_extra_links(library_path: &Path) -> Vec<ExtraLink> {
    let mut extra_links = Vec::new();
    let Some(lib_dir) = library_path.parent() else {
        return extra_links;
    };

    for candidate in [
        "libv8_libplatform.a",
        "libv8_libplatform.so",
        "libv8_libplatform.dylib",
    ] {
        let path = lib_dir.join(candidate);
        if path.exists() {
            extra_links.push(ExtraLink::LibraryPath(path));
            break;
        }
    }

    for candidate in ["libv8_libbase.a", "libv8_libbase.so", "libv8_libbase.dylib"] {
        let path = lib_dir.join(candidate);
        if path.exists() {
            extra_links.push(ExtraLink::LibraryPath(path));
            break;
        }
    }

    extra_links
}

fn default_prebuilt_extra_links() -> Vec<ExtraLink> {
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "macos" || target_os == "ios" {
        vec![ExtraLink::Framework(PathBuf::from(
            "/System/Library/Frameworks/CoreFoundation.framework",
        ))]
    } else {
        Vec::new()
    }
}

fn parse_extra_links(raw: &str) -> Vec<ExtraLink> {
    raw.split(&[';', ','][..])
        .filter_map(|entry| {
            let trimmed = entry.trim();
            if trimmed.is_empty() {
                return None;
            }

            if trimmed.ends_with(".framework") {
                return Some(ExtraLink::Framework(PathBuf::from(trimmed)));
            }

            let path = PathBuf::from(trimmed);
            if path.is_absolute() || path.parent().is_some() {
                return Some(ExtraLink::LibraryPath(path));
            }

            Some(ExtraLink::Name(trimmed.to_string()))
        })
        .collect()
}

fn emit_library_link(path: &Path) {
    let dir = path
        .parent()
        .expect("linked library path has no parent directory");
    println!("cargo:rustc-link-search=native={}", dir.display());

    let extension = path
        .extension()
        .and_then(|ext| ext.to_str())
        .unwrap_or_default();
    let kind = if extension == "a" { "static" } else { "dylib" };
    let name = link_name_from_path(path);
    println!("cargo:rustc-link-lib={kind}={name}");
}

fn emit_extra_link(link: &ExtraLink) {
    match link {
        ExtraLink::LibraryPath(path) => emit_library_link(path),
        ExtraLink::Framework(path) => {
            let dir = path
                .parent()
                .expect("framework path has no parent directory");
            let name = path
                .file_stem()
                .and_then(|stem| stem.to_str())
                .expect("framework path is not valid UTF-8");
            println!("cargo:rustc-link-search=framework={}", dir.display());
            println!("cargo:rustc-link-lib=framework={name}");
        }
        ExtraLink::Name(name) => {
            println!("cargo:rustc-link-lib={name}");
        }
    }
}

fn link_name_from_path(path: &Path) -> String {
    let file_name = path
        .file_name()
        .and_then(|name| name.to_str())
        .expect("library path is not valid UTF-8");
    let without_prefix = file_name.strip_prefix("lib").unwrap_or(file_name);
    let without_suffix = without_prefix
        .strip_suffix(".a")
        .or_else(|| without_prefix.strip_suffix(".so"))
        .or_else(|| without_prefix.strip_suffix(".dylib"))
        .unwrap_or(without_prefix);
    without_suffix.to_string()
}

fn read_env_value(name: &str, aliases: &[&str]) -> Option<String> {
    env::var(name)
        .ok()
        .filter(|value| !value.trim().is_empty())
        .or_else(|| {
            aliases.iter().find_map(|alias| {
                env::var(alias)
                    .ok()
                    .filter(|value| !value.trim().is_empty())
            })
        })
}

fn env_truthy(name: &str) -> bool {
    env::var(name)
        .ok()
        .map(|value| {
            let normalized = value.trim();
            !normalized.is_empty() && normalized != "0" && normalized.to_lowercase() != "false"
        })
        .unwrap_or(false)
}
