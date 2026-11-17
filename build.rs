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
    println!("cargo:rerun-if-env-changed=AR");
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
    let sandbox_define = v8_defines
        .split(&[';', ',', ' '][..])
        .any(|define| matches!(define, "V8_ENABLE_SANDBOX" | "V8_ENABLE_SANDBOX=1"));
    if sandbox_define {
        panic!(
            "V8_ENABLE_SANDBOX is unsupported: N-API backing stores can reside outside the V8 sandbox"
        );
    }
    // The bridge exposes Wasmer linear memory and other embedder-owned
    // allocations as V8 ArrayBuffer backing stores. V8's sandbox requires
    // every such store to reside inside its sandbox address range. Until the
    // bridge has a sandbox-resident backing-store implementation, accepting
    // this archive would make ordinary guest code abort the host process.
    if read_sandbox_build_flag(&v8.library_path) == Some(true) {
        panic!(
            "sandbox-enabled V8 archives are unsupported: N-API backing stores can reside outside the V8 sandbox"
        );
    }

    let mut build = cc::Build::new();
    // Each private layout/stack-start profile is separately artifact-audited.
    // Explicit/local builds keep ordinary native entry and decline coroutine
    // entry rather than assuming their GC/build flags.
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
    if v8.origin == V8Origin::Prebuilt
        && target_arch == "aarch64"
        && env::var("CARGO_CFG_TARGET_POINTER_WIDTH").as_deref() == Ok("64")
        && env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos")
        && PREBUILT_V8_VERSION == "11.9.7"
    {
        v8.library_path =
            adapt_pinned_darwin_v8_archive(&v8.library_path, &v8.include_dir, &v8_defines)
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
        // Linux arm64 local-provider builds retain upstream's atomic runtime.
        // Product CI disables NAPI V8 there; 11.9.7 has no certified arm64 archive.
        if target_os == "linux" && env::var("CARGO_CFG_TARGET_ARCH").as_deref() == Ok("aarch64") {
            println!("cargo:rustc-link-lib=dylib=atomic");
        }
    }
}

const PINNED_V8_ARCHIVE_SHA256: &str =
    "c58981886d0aabc93eae4da63ea6f8ce253603f95efe26d604aaaf185ab8109a";
const PINNED_STACK_OBJECT_SHA256: &str =
    "f48ebc83f7693073b85889b843649cca1d5c6192ec0c2cb5731b6c9eb28ecacc";
const STACK_SCANNER_SYMBOL: &str =
    "_ZNK4heap4base5Stack26IteratePointersUntilMarkerEPNS0_12StackVisitorE";
const ORIGINAL_STACK_SCANNER_SYMBOL: &str = "snapi_original_v8_stack_iterate_pointers_until_marker";
const PINNED_PLATFORM_OBJECT_SHA256: &str =
    "20fe10400d298b3b2dd5953a59a1523f8e3f3ff26d57512cc572ba225275242e";
const PINNED_STACK_GUARD_OBJECT_SHA256: &str =
    "808afcaaf805b5f71cf7f6b259ac414ac60622296efa2f65251ae6d27180ac1f";

fn sha256(bytes: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    use std::fmt::Write as _;

    let mut hex = String::with_capacity(64);
    for byte in Sha256::digest(bytes) {
        write!(&mut hex, "{byte:02x}").expect("formatting into a String cannot fail");
    }
    hex
}

// GNU ar permits duplicate names. Ordinals include the long-name table but
// exclude only the rebuilt symbol index; preserve every other raw record.
struct ArchiveMember<'a> {
    name: &'a str,
    header: &'a [u8],
    data: &'a [u8],
    record: &'a [u8],
}

fn archive_members(bytes: &[u8]) -> Result<Vec<ArchiveMember<'_>>, String> {
    if !bytes.starts_with(b"!<arch>\n") {
        return Err("pinned V8 must be an ordinary GNU archive".into());
    }
    let mut position = 8usize;
    let mut members = Vec::new();
    let mut indexes = 0usize;
    while position < bytes.len() {
        let start = position.checked_add(60).ok_or("archive offset overflow")?;
        let header = bytes
            .get(position..start)
            .ok_or("truncated V8 archive member")?;
        if &header[58..60] != b"`\n" {
            return Err("invalid V8 archive member header".into());
        }
        let name = std::str::from_utf8(&header[..16])
            .map_err(|_| "invalid archive member name")?
            .trim_end_matches(' ');
        if name.is_empty() || !name.bytes().all(|byte| byte.is_ascii_graphic()) {
            return Err("invalid GNU archive member name".into());
        }
        let symbol_index = name == "/" || name == "/SYM64/";
        let long_name = name.strip_prefix('/').is_some_and(|offset| {
            !offset.is_empty() && offset.bytes().all(|byte| byte.is_ascii_digit())
        });
        if !symbol_index
            && name != "//"
            && !long_name
            && (name.starts_with('/') || name.starts_with("#1/") || !name.ends_with('/'))
        {
            return Err("unsupported GNU archive member name".into());
        }
        let size = std::str::from_utf8(&header[48..58])
            .map_err(|_| "invalid archive member length")?
            .trim_matches(' ');
        if size.is_empty() || !size.bytes().all(|byte| byte.is_ascii_digit()) {
            return Err("invalid archive member length".into());
        }
        let size: usize = size.parse().map_err(|_| "invalid archive member length")?;
        let end = start.checked_add(size).ok_or("archive member overflow")?;
        let padded_end = end
            .checked_add(size % 2)
            .ok_or("archive padding overflow")?;
        let data = bytes
            .get(start..end)
            .ok_or("truncated archive member data")?;
        let record = bytes
            .get(position..padded_end)
            .ok_or("truncated archive member padding")?;
        if symbol_index {
            indexes += 1;
            if indexes != 1 || position != 8 {
                return Err("unsupported GNU archive symbol index shape".into());
            }
        } else {
            members.push(ArchiveMember {
                name,
                header,
                data,
                record,
            });
        }
        position = padded_end;
    }
    if indexes != 1 {
        return Err("indexed GNU archive required".into());
    }
    Ok(members)
}

fn replace_archive_record(member: &ArchiveMember<'_>, data: &[u8]) -> Result<Vec<u8>, String> {
    let size = data.len().to_string();
    if size.len() > 10 {
        return Err("adapted archive member exceeds ten-column size field".into());
    }
    let mut header = member.header.to_vec();
    header[48..58].fill(b' ');
    header[48..48 + size.len()].copy_from_slice(size.as_bytes());
    let mut record = header;
    record.extend_from_slice(data);
    if data.len() % 2 != 0 {
        record.push(b'\n');
    }
    Ok(record)
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
    // Each original machine-code object contains exactly one definition and
    // no local relocation to the symbol being renamed. Other members retain
    // their upstream references, which resolve to the physical-context adapter.
    let adaptations: &[(usize, &str, &str, &[(&str, &str)])] = &[
        (
            803,
            "stack.o/",
            PINNED_STACK_OBJECT_SHA256,
            &[(STACK_SCANNER_SYMBOL, ORIGINAL_STACK_SCANNER_SYMBOL)],
        ),
        (
            26,
            "platform.o/",
            PINNED_PLATFORM_OBJECT_SHA256,
            &[
                (
                    "_ZN2v84base5Stack13GetStackStartEv",
                    "snapi_original_v8_get_stack_start",
                ),
                (
                    "_ZN2v84base5Stack22GetStackStartUncheckedEv",
                    "snapi_original_v8_get_stack_start_unchecked",
                ),
            ],
        ),
        (
            184,
            "stack-guard.o/",
            PINNED_STACK_GUARD_OBJECT_SHA256,
            &[(
                "_ZN2v88internal10StackGuard10InitThreadERKNS0_15ExecutionAccessE",
                "snapi_original_v8_stack_guard_init_thread",
            )],
        ),
    ];
    let directory =
        PathBuf::from(env::var_os("OUT_DIR").ok_or("OUT_DIR is not set")?).join("v8-native-stack");
    std::fs::create_dir_all(&directory).map_err(|error| error.to_string())?;
    let destination = directory.join("libv8_snapi_stack.a");
    let objcopy = env::var_os("OBJCOPY").unwrap_or_else(|| "objcopy".into());
    let nm = env::var_os("NM").unwrap_or_else(|| "nm".into());
    let mut adapted_members = Vec::new();
    for &(ordinal, name, expected_hash, renames) in adaptations {
        let member = before
            .get(ordinal)
            .ok_or("audited V8 member ordinal missing")?;
        if member.name != name || sha256(member.data) != expected_hash {
            return Err(format!(
                "audited V8 member {ordinal} ({name}) is missing or changed"
            ));
        }
        let filename = name
            .strip_suffix('/')
            .ok_or("invalid audited member name")?;
        let original_object = directory.join(format!("original-{filename}"));
        let adapted_object = directory.join(filename);
        std::fs::write(&original_object, member.data).map_err(|error| error.to_string())?;
        let mut transform = std::process::Command::new(&objcopy);
        for &(original, adapted) in renames {
            transform
                .arg("--redefine-sym")
                .arg(format!("{original}={adapted}"));
        }
        tool_output(transform.arg(&original_object).arg(&adapted_object))?;
        let original_symbols = tool_output(std::process::Command::new(&nm).arg(&original_object))?;
        let adapted_symbols = tool_output(std::process::Command::new(&nm).arg(&adapted_object))?;
        let mut expected =
            String::from_utf8(original_symbols).map_err(|error| error.to_string())?;
        for &(original, adapted) in renames {
            if expected.matches(original).count() != 1 {
                return Err(format!(
                    "unexpected definition/reference of {original} in {name}"
                ));
            }
            expected = expected.replace(original, adapted);
        }
        let actual = String::from_utf8(adapted_symbols).map_err(|error| error.to_string())?;
        // nm sorts by symbol name; compare after only the exact renames.
        let mut expected: Vec<_> = expected.lines().collect();
        let mut actual: Vec<_> = actual.lines().collect();
        expected.sort_unstable();
        actual.sort_unstable();
        if expected != actual {
            return Err(format!(
                "V8 {name} transformation changed unrelated symbols"
            ));
        }
        let bytes = std::fs::read(&adapted_object).map_err(|error| error.to_string())?;
        adapted_members.push((ordinal, replace_archive_record(member, &bytes)?));
    }
    // Rebuild by ordered record, never ar r/name: another platform.o at
    // ordinal 650 is unrelated and must remain byte-for-byte unchanged.
    // Dropping only the old symbol index lets ar s calculate new offsets.
    let mut output = b"!<arch>\n".to_vec();
    for (ordinal, member) in before.iter().enumerate() {
        let record = adapted_members
            .iter()
            .find(|(selected, _)| *selected == ordinal)
            .map_or(member.record, |(_, record)| record.as_slice());
        output.extend_from_slice(record);
    }
    std::fs::write(&destination, &output).map_err(|error| error.to_string())?;
    drop(output);
    let mut index = build.get_archiver();
    index.arg("s").arg(&destination);
    tool_output(&mut index)?;
    let output_bytes = std::fs::read(&destination).map_err(|error| error.to_string())?;
    let after = archive_members(&output_bytes)?;
    if before.len() != after.len() {
        return Err("V8 physical-context transformation changed archive membership".into());
    }
    for (ordinal, (old, new)) in before.iter().zip(&after).enumerate() {
        let expected = adapted_members
            .iter()
            .find(|(selected, _)| *selected == ordinal)
            .map_or(old.record, |(_, record)| record.as_slice());
        if new.record != expected {
            return Err(format!(
                "V8 transformation changed archive record {ordinal} ({})",
                old.name
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

// Darwin's BSD archive indexer normalizes every container header, unlike GNU
// ar s. Keep its parser and canonical-container proof separate from Linux's
// strict raw-record proof above. The full original and transformed object hashes
// certify this one arm64 package, not arbitrary Mach-O or local V8 builds.
const PINNED_DARWIN_V8_ARCHIVE_SHA256: &str =
    "b2d1b99e8d6177b35e90ee2ea58d02dd5de37a6a33dd673d5f1afedf6a5eeb87";

struct BsdArchiveMember<'a> {
    offset: usize,
    name: &'a str,
    header: &'a [u8],
    inline_name: &'a [u8],
    data: &'a [u8],
    record: &'a [u8],
}

fn archive_decimal(bytes: &[u8]) -> Result<usize, String> {
    let value = std::str::from_utf8(bytes)
        .map_err(|_| "invalid BSD archive decimal field")?
        .trim_matches(' ');
    if value.is_empty() || !value.bytes().all(|byte| byte.is_ascii_digit()) {
        return Err("invalid BSD archive decimal field".into());
    }
    value
        .parse()
        .map_err(|_| "BSD archive decimal overflow".into())
}

fn bsd_archive_members(bytes: &[u8]) -> Result<Vec<BsdArchiveMember<'_>>, String> {
    if !bytes.starts_with(b"!<arch>\n") {
        return Err("pinned Darwin V8 must be an ordinary BSD archive".into());
    }
    let mut offset = 8usize;
    let mut members = Vec::new();
    while offset < bytes.len() {
        let start = offset
            .checked_add(60)
            .ok_or("BSD archive offset overflow")?;
        let header = bytes
            .get(offset..start)
            .ok_or("truncated BSD archive header")?;
        if &header[58..] != b"`\n" {
            return Err("invalid BSD archive terminator".into());
        }
        let name_field = std::str::from_utf8(&header[..16])
            .map_err(|_| "invalid BSD archive name field")?
            .trim_end_matches(' ');
        let inline_length = archive_decimal(
            name_field
                .strip_prefix("#1/")
                .ok_or("unsupported BSD archive name encoding")?
                .as_bytes(),
        )?;
        let size = archive_decimal(&header[48..58])?;
        if inline_length == 0 || inline_length > size {
            return Err("invalid BSD archive inline filename length".into());
        }
        let end = start
            .checked_add(size)
            .ok_or("BSD archive member overflow")?;
        let padded_end = end
            .checked_add(size % 2)
            .ok_or("BSD archive padding overflow")?;
        let contents = bytes.get(start..end).ok_or("truncated BSD archive data")?;
        let record = bytes
            .get(offset..padded_end)
            .ok_or("truncated BSD archive padding")?;
        if size % 2 != 0 && bytes[end] != b'\n' {
            return Err("noncanonical BSD archive outer padding".into());
        }
        let (inline_name, data) = contents.split_at(inline_length);
        let name_end = inline_name
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(inline_length);
        if inline_name[name_end..].iter().any(|byte| *byte != 0) {
            return Err("invalid BSD archive inline filename padding".into());
        }
        let name = std::str::from_utf8(&inline_name[..name_end])
            .map_err(|_| "invalid BSD archive filename")?;
        if name.is_empty()
            || !name.bytes().all(|byte| byte.is_ascii_graphic())
            || name.contains('/')
            || (name.starts_with("__.SYMDEF") && name != "__.SYMDEF")
        {
            return Err("unsupported BSD archive filename".into());
        }
        if (members.is_empty()) != (name == "__.SYMDEF") {
            return Err("one leading BSD archive symbol index required".into());
        }
        members.push(BsdArchiveMember {
            offset,
            name,
            header,
            inline_name,
            data,
            record,
        });
        offset = padded_end;
    }
    if members.is_empty() {
        return Err("indexed BSD archive required".into());
    }
    Ok(members)
}

fn replace_bsd_archive_record(
    member: &BsdArchiveMember<'_>,
    data: &[u8],
) -> Result<Vec<u8>, String> {
    let mut contents = member.inline_name.to_vec();
    contents.extend_from_slice(data);
    // The existing writer changes only the checked size column and odd pad.
    replace_archive_record(
        &ArchiveMember {
            name: member.name,
            header: member.header,
            data: member.data,
            record: member.record,
        },
        &contents,
    )
}

fn check_canonical_bsd_member(
    member: &BsdArchiveMember<'_>,
    timestamp: usize,
    index: bool,
) -> Result<(), String> {
    // LLVM Darwin sD aligns object bytes to eight and numbers duplicate names
    // 1..N (unique names have timestamp zero), including their original order.
    let unpadded = member
        .offset
        .checked_add(60)
        .and_then(|offset| offset.checked_add(member.name.len()))
        .ok_or("BSD archive filename offset overflow")?;
    let name_length = member
        .name
        .len()
        .checked_add((8 - unpadded % 8) % 8)
        .ok_or("BSD archive filename length overflow")?;
    let size = name_length
        .checked_add(member.data.len())
        .ok_or("BSD archive size overflow")?;
    let name_field = format!("#1/{name_length}");
    let mode = if index { "0" } else { "644" };
    let header = format!(
        "{name_field:<16}{timestamp:<12}{:<6}{:<6}{mode:<8}{size:<10}`\n",
        0, 0
    );
    if header.len() != 60
        || member.header != header.as_bytes()
        || member.inline_name.len() != name_length
        || &member.inline_name[..member.name.len()] != member.name.as_bytes()
        || member.inline_name[member.name.len()..]
            .iter()
            .any(|byte| *byte != 0)
    {
        return Err(format!(
            "noncanonical Darwin archive header/name for {}",
            member.name
        ));
    }
    Ok(())
}

fn adapt_pinned_darwin_v8_archive(
    source: &Path,
    include: &Path,
    defines: &str,
) -> Result<PathBuf, String> {
    let definitions: Vec<_> = defines
        .split(&[';', ',', ' '][..])
        .filter(|part| !part.is_empty())
        .collect();
    if definitions != ["V8_COMPRESS_POINTERS"] {
        return Err("Darwin V8 coroutine support requires re-audit of these build defines".into());
    }
    for (name, expected) in [
        (
            "v8-version.h",
            "7a945b923eec649417fc366a50b1439af006869ca764f37d1d07be15eed47802",
        ),
        (
            "v8config.h",
            "56baeb70e071f303124e0bed804287bf3115d1dac3f59b4336ab20c57446960d",
        ),
        (
            "v8-internal.h",
            "24eace83fd7bb925a200c11fdd9b3b35c14baf146aa0dc4763ea0afc1feede06",
        ),
    ] {
        if sha256(&std::fs::read(include.join(name)).map_err(|error| error.to_string())?)
            != expected
        {
            return Err(format!(
                "Darwin V8 coroutine support requires re-audit of {name}"
            ));
        }
    }
    let source_bytes = std::fs::read(source).map_err(|error| error.to_string())?;
    if sha256(&source_bytes) != PINNED_DARWIN_V8_ARCHIVE_SHA256 {
        return Err("Darwin V8 coroutine support requires re-audit of this archive".into());
    }
    let before = bsd_archive_members(&source_bytes)?;
    if before.len() != 1769 {
        return Err("audited Darwin archive membership changed".into());
    }
    // Non-index ordinals. In particular platform.o at ordinal 656 is unrelated.
    let adaptations: &[(usize, &str, &str, &str, &[(&str, &str)])] = &[
        (
            25,
            "platform.o",
            "123cb8a9cf22a1a4e9c371fe3b789cef6126fc9dbdc80413789d3c60402018f8",
            "1c2dcf5a2c195d79ac7224a9be9d0ba66bd95eff762c0ae21d74a9d448048b97",
            &[
                (
                    "__ZN2v84base5Stack13GetStackStartEv",
                    "_snapi_original_v8_get_stack_start",
                ),
                (
                    "__ZN2v84base5Stack22GetStackStartUncheckedEv",
                    "_snapi_original_v8_get_stack_start_unchecked",
                ),
            ],
        ),
        (
            184,
            "stack-guard.o",
            "12d2731ccccb6785eae649548a54ace30569446e7487d5c9a68f80eedc45c01b",
            "a7d19cd8c9e972bb74fc4c2fbeee4c043171f38a837e529762438b63a86cf1d8",
            &[(
                "__ZN2v88internal10StackGuard10InitThreadERKNS0_15ExecutionAccessE",
                "_snapi_original_v8_stack_guard_init_thread",
            )],
        ),
        (
            809,
            "stack.o",
            "b941c867cf83f995bc6f00315fc7b0c33bbd7542426effaadfa32223f2b74d6d",
            "3afef441a88af6e19f81f4b21f97433ce943fb58680b3bd064112e297003c6be",
            &[(
                "__ZNK4heap4base5Stack26IteratePointersUntilMarkerEPNS0_12StackVisitorE",
                "_snapi_original_v8_stack_iterate_pointers_until_marker",
            )],
        ),
    ];
    let directory = PathBuf::from(env::var_os("OUT_DIR").ok_or("OUT_DIR is not set")?)
        .join("v8-native-stack-darwin");
    std::fs::create_dir_all(&directory).map_err(|error| error.to_string())?;
    let destination = directory.join("libv8_snapi_stack.a");
    let objcopy = env::var_os("OBJCOPY").unwrap_or_else(|| "llvm-objcopy".into());
    let nm = env::var_os("NM").unwrap_or_else(|| "llvm-nm".into());
    let ar = env::var_os("AR").unwrap_or_else(|| "llvm-ar".into());
    let mut adapted_members = Vec::new();
    for &(ordinal, name, expected_hash, adapted_hash, renames) in adaptations {
        let member = before
            .get(ordinal + 1)
            .ok_or("audited Darwin member missing")?;
        if member.name != name || sha256(member.data) != expected_hash {
            return Err(format!("audited Darwin member {ordinal} ({name}) changed"));
        }
        let original = directory.join(format!("original-{name}"));
        let adapted = directory.join(name);
        std::fs::write(&original, member.data).map_err(|error| error.to_string())?;
        let mut transform = std::process::Command::new(&objcopy);
        for &(old, new) in renames {
            transform.arg("--redefine-sym").arg(format!("{old}={new}"));
        }
        tool_output(transform.arg(&original).arg(&adapted))?;
        let original_symbols = tool_output(std::process::Command::new(&nm).arg(&original))?;
        let adapted_symbols = tool_output(std::process::Command::new(&nm).arg(&adapted))?;
        let mut expected =
            String::from_utf8(original_symbols).map_err(|error| error.to_string())?;
        for &(old, new) in renames {
            if expected.matches(old).count() != 1 {
                return Err(format!("unexpected Darwin definition/reference of {old}"));
            }
            expected = expected.replace(old, new);
        }
        let actual = String::from_utf8(adapted_symbols).map_err(|error| error.to_string())?;
        let mut expected: Vec<_> = expected.lines().collect();
        let mut actual: Vec<_> = actual.lines().collect();
        expected.sort_unstable();
        actual.sort_unstable();
        if expected != actual {
            return Err(format!("Darwin {name} changed unrelated symbols"));
        }
        let bytes = std::fs::read(&adapted).map_err(|error| error.to_string())?;
        if sha256(&bytes) != adapted_hash {
            return Err(format!(
                "Darwin {name} transform requires tool/output re-audit"
            ));
        }
        adapted_members.push((ordinal, replace_bsd_archive_record(member, &bytes)?, bytes));
    }
    let mut output = b"!<arch>\n".to_vec();
    for (ordinal, member) in before.iter().skip(1).enumerate() {
        let record = adapted_members
            .iter()
            .find(|(selected, _, _)| *selected == ordinal)
            .map_or(member.record, |(_, record, _)| record.as_slice());
        output.extend_from_slice(record);
    }
    std::fs::write(&destination, output).map_err(|error| error.to_string())?;
    tool_output(
        std::process::Command::new(&ar)
            .arg("sD")
            .arg("--format=darwin")
            .arg(&destination),
    )?;
    let output_bytes = std::fs::read(&destination).map_err(|error| error.to_string())?;
    let after = bsd_archive_members(&output_bytes)?;
    if before.len() != after.len() {
        return Err("Darwin archive membership changed".into());
    }
    check_canonical_bsd_member(&after[0], 0, true)?;
    let mut totals = std::collections::HashMap::new();
    for member in before.iter().skip(1) {
        *totals.entry(member.name).or_insert(0usize) += 1;
    }
    let mut occurrences = std::collections::HashMap::new();
    for (ordinal, (old, new)) in before.iter().skip(1).zip(after.iter().skip(1)).enumerate() {
        if old.name != new.name {
            return Err(format!("Darwin archive member {ordinal} reordered"));
        }
        let occurrence = occurrences.entry(new.name).or_insert(0usize);
        *occurrence += 1;
        check_canonical_bsd_member(
            new,
            if totals[new.name] > 1 { *occurrence } else { 0 },
            false,
        )?;
        if let Some((_, _, expected)) = adapted_members
            .iter()
            .find(|(selected, _, _)| *selected == ordinal)
        {
            let padding = (8 - expected.len() % 8) % 8;
            if new.data.len() != expected.len() + padding
                || !new.data.starts_with(expected)
                || new.data[expected.len()..].iter().any(|byte| *byte != b'\n')
            {
                return Err(format!(
                    "Darwin adapted member {ordinal} bytes/padding changed"
                ));
            }
        } else if old.data != new.data {
            return Err(format!(
                "Darwin unrelated member {ordinal} ({}) changed",
                old.name
            ));
        }
    }
    // Verify private originals exist exactly once and all retained upstream
    // references still target the bridge names, never a second mangled body.
    let symbols = String::from_utf8(tool_output(
        std::process::Command::new(&nm).arg(&destination),
    )?)
    .map_err(|error| error.to_string())?;
    for &(_, _, _, _, renames) in adaptations {
        for &(old, new) in renames {
            let count = |name: &str, kind: &str| {
                symbols
                    .lines()
                    .filter(|line| {
                        let words: Vec<_> = line.split_whitespace().collect();
                        words.len() >= 2
                            && words[words.len() - 1] == name
                            && words[words.len() - 2] == kind
                    })
                    .count()
            };
            if count(old, "T") != 0 || count(new, "T") != 1 || count(old, "U") == 0 {
                return Err(format!(
                    "Darwin archive rename/reference proof failed for {old}"
                ));
            }
        }
    }
    if sha256(&std::fs::read(source).map_err(|error| error.to_string())?)
        != PINNED_DARWIN_V8_ARCHIVE_SHA256
    {
        return Err("source Darwin V8 archive changed during adaptation".into());
    }
    Ok(destination)
}

fn read_sandbox_build_flag(library_path: &Path) -> Option<bool> {
    let config_path = library_path.parent()?.parent()?.join("build-config.txt");
    println!("cargo:rerun-if-changed={}", config_path.display());
    let config = match std::fs::read_to_string(&config_path) {
        Ok(config) => config,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => return None,
        Err(err) => panic!("failed to read {}: {err}", config_path.display()),
    };
    match config
        .lines()
        .find_map(|line| line.strip_prefix("v8_enable_sandbox="))
    {
        Some("true") => Some(true),
        Some("false") => Some(false),
        _ => panic!(
            "{} lacks a valid v8_enable_sandbox flag",
            config_path.display()
        ),
    }
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
