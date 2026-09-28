# Released EdgeJS 0.0.1 N-API compatibility

The frozen guest is the `edgejs` atom from `wasmer/edgejs@0.0.1`, SHA-256
`ca6467e67c8503474cb4204cd2dbbae387c8fa19cad6f6c23131844143e27ccc`.
`tests/fixtures/edgejs-0.0.1-napi-imports.txt` records its import section;
`tests/fixtures/edgejs-0.0.1-napi-signatures.txt` records all 186 `napi`
function types. Linking rejects a changed signature. The legacy names use the
same provider environment, resource budget, lane admission, and sticky stop as
the versioned extension.

## Verified paths

- The exact atom prints `v24.13.2-pre` for `--version` and runs
  `-e "console.log('LEGACY_NAPI_BOOT_OK')"`.
- Its default V8 flag string is validated and inert. Unexpected flags fail
  before creating a V8 environment.
- Legacy serialized message handles retain their app budget until explicit
  release. Deserialization may read the same handle repeatedly. Release,
  stale-handle access, double release, and host-stop cleanup are covered by
  `tests/legacy_manifest.rs`; host stop removes the native holder and charge.

The exact atom probes require a local copy and are intentionally opt in:

```sh
NAPI_EDGEJS_0_0_1_ATOM=/path/to/edgejs \
  ./cargo-standalone.sh test --quiet --features cli --test legacy_manifest
```

## Current limits

- Dynamic `import('data:text/javascript,export default 7')` starts from a
  running `-e` script, then rejects in the released guest's ESM translator:
  `internalBinding('cjs_lexer')` is undefined. The probe does not reach a
  `module_wrap_create` host call. The ESM test is ignored until the released
  guest's binding path and the remaining legacy module-wrap operations are
  resolved.
- The worker probe passes legacy message serialization and creates a worker
  environment, but exits 127 before posting a message. The released guest
  asserts `state->owning_thread == std::this_thread::get_id()` in
  `edge_runtime_platform_v8.cc:134`. A bounded host trace showed worker env
  creation, foreground-hook installation, and near-heap callback removal on
  the same host thread. No `release_env_with_loop` import occurs before the
  assertion. Historical `binding_worker.cc` calls
  `EdgeWorkerEnvRunCleanupPreserveLoop` between near-heap removal and release;
  its runtime-platform cleanup stage contains this assertion. The test is
  ignored pending a guest thread-identity diagnosis.
- Some frozen legacy imports still return failure because their behavior has
  no implemented provider adapter. In particular, legacy module-wrap creation
  and the serdes binding remain incomplete. The exact manifest test proves
  type and link compatibility, not behavioral coverage of every import.

These limitations are recorded so a successful import-only link or zero exit
from an ESM flag probe is not mistaken for application compatibility.
