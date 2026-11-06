//! Every program in `tests/programs/manifest.json`, built twice and checked
//! against the same expected output: natively against V8 with
//! `tests/build-test-native.sh`, and for WASIX with `tests/build-test-wasix.sh`,
//! run through the `napi_wasmer` CLI, which the test builds first.
//!
//! Besides what the other tests need, this one needs `wasixcc` (on `PATH` or
//! in `~/.wasixcc/bin`) with a WASIX sysroot. The native programs link the V8
//! that `NAPI_V8_INCLUDE_DIR` and `NAPI_V8_LIBRARY` name, or else the one that
//! `build.rs` downloaded for a debug build in the same target directory.

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::OnceLock;

use serde::Deserialize;

/// The checkout itself. `cargo-standalone.sh` builds from a temporary
/// directory that holds only a manifest and symlinks to `src`, `tests` and the
/// other sources, so `CARGO_MANIFEST_DIR` is not always the checkout. The
/// `tests` symlink resolves back to the checkout's own `tests` directory.
fn repo_root() -> &'static Path {
    static ROOT: OnceLock<PathBuf> = OnceLock::new();
    ROOT.get_or_init(|| {
        let tests = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests");
        let tests = std::fs::canonicalize(&tests)
            .unwrap_or_else(|err| panic!("failed to resolve {}: {err}", tests.display()));
        tests
            .parent()
            .expect("the tests directory has a parent")
            .to_path_buf()
    })
}

/// The target directory this test was built in, wherever Cargo was told to put
/// it. The build scripts and the nested CLI build are pointed at it, so they
/// find the V8 that `build.rs` downloaded and leave their output where this
/// test looks for it.
fn cargo_target_dir() -> &'static Path {
    Path::new(env!("CARGO_TARGET_TMPDIR"))
        .parent()
        .expect("CARGO_TARGET_TMPDIR is inside the target directory")
}

fn path_from_env_or(name: &str, default: PathBuf) -> PathBuf {
    env::var_os(name)
        .map(PathBuf::from)
        .map(|path| {
            if path.is_absolute() {
                path
            } else {
                repo_root().join(path)
            }
        })
        .unwrap_or(default)
}

fn native_test_dir() -> PathBuf {
    path_from_env_or(
        "NAPI_NATIVE_TEST_OUT_DIR",
        cargo_target_dir().join("native"),
    )
}

fn wasix_test_dir() -> PathBuf {
    path_from_env_or(
        "NAPI_WASIX_TEST_OUT_DIR",
        cargo_target_dir().join("wasm32-wasix/release"),
    )
}

/// A command run from the checkout with the same target and output
/// directories as this test.
fn repo_command(program: &str) -> Command {
    let root = repo_root();
    let mut command = Command::new(root.join(program));
    command
        .current_dir(root)
        .env("CARGO_TARGET_DIR", cargo_target_dir())
        .env("NAPI_NATIVE_TEST_OUT_DIR", native_test_dir())
        .env("NAPI_WASIX_TEST_OUT_DIR", wasix_test_dir());
    command
}

#[derive(Deserialize)]
struct TestManifest {
    tests: Vec<TestCase>,
}

#[derive(Deserialize)]
struct TestCase {
    name: String,
    description: String,
    expected_stdout: Option<String>,
    expected_stdout_prefix: Option<String>,
}

fn build_native_test(name: &str) -> PathBuf {
    let status = repo_command("tests/build-test-native.sh")
        .arg(name)
        .status()
        .expect("failed to execute tests/build-test-native.sh");
    assert!(status.success(), "failed to build native test: {name}");
    native_test_dir().join(name)
}

fn build_wasix_test(name: &str) -> PathBuf {
    let status = repo_command("tests/build-test-wasix.sh")
        .arg(name)
        .status()
        .expect("failed to execute tests/build-test-wasix.sh");
    assert!(status.success(), "failed to build WASIX test: {name}");
    wasix_test_dir().join(format!("{name}.wasm"))
}

fn build_cli_binary() -> PathBuf {
    static CLI_PATH: OnceLock<PathBuf> = OnceLock::new();
    CLI_PATH
        .get_or_init(|| {
            let status = repo_command("cargo-standalone.sh")
                .args(["build", "--features", "cli", "--bin", "napi_wasmer"])
                .status()
                .expect("failed to build napi_wasmer CLI binary for tests");
            assert!(status.success(), "failed to build napi_wasmer CLI binary");
            cargo_target_dir().join("debug/napi_wasmer")
        })
        .clone()
}

#[test]
fn manifest_tests_match_native_and_wasix() {
    let cli = build_cli_binary();
    let manifest_path = repo_root().join("tests/programs/manifest.json");
    let manifest_content = std::fs::read_to_string(&manifest_path)
        .expect("failed to read test/programs/manifest.json");
    let manifest: TestManifest =
        serde_json::from_str(&manifest_content).expect("failed to parse test manifest json");

    assert!(
        !manifest.tests.is_empty(),
        "manifest must contain at least one test"
    );

    for case in &manifest.tests {
        eprintln!("running test: {} ({})", case.name, case.description);

        // Build and run native test
        let native = build_native_test(&case.name);
        let native_out = Command::new(&native)
            .output()
            .expect("failed to run native test");
        assert_eq!(
            native_out.status.code().unwrap_or(-1),
            0,
            "native test exited non-zero: {}",
            case.name
        );
        let native_stdout = String::from_utf8(native_out.stdout).expect("native output not utf-8");

        if let Some(expected) = &case.expected_stdout {
            assert_eq!(
                &native_stdout, expected,
                "native stdout mismatch for {}",
                case.name
            );
        }
        if let Some(prefix) = &case.expected_stdout_prefix {
            assert!(
                native_stdout.starts_with(prefix),
                "native stdout prefix mismatch for {}",
                case.name
            );
        }

        // Build and run WASIX test
        let wasix_path = build_wasix_test(&case.name);
        let wasix_out = Command::new(&cli)
            .arg(&wasix_path)
            .output()
            .expect("failed to run WASIX test via napi_wasmer CLI");
        assert_eq!(
            wasix_out.status.code().unwrap_or(-1),
            0,
            "wasix test exited non-zero: {}",
            case.name
        );
        let wasix_stdout = String::from_utf8(wasix_out.stdout).expect("wasix output not utf-8");
        if let Some(expected) = &case.expected_stdout {
            assert_eq!(
                &wasix_stdout, expected,
                "wasix stdout mismatch for {}",
                case.name
            );
        }
        if let Some(prefix) = &case.expected_stdout_prefix {
            assert!(
                wasix_stdout.starts_with(prefix),
                "wasix stdout prefix mismatch for {}",
                case.name
            );
        }
    }
}
