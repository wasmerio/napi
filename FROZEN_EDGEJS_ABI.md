# Released EdgeJS 0.0.1 N-API compatibility

The frozen guest is the `edgejs` atom from `wasmer/edgejs@0.0.1`, SHA-256
`ca6467e67c8503474cb4204cd2dbbae387c8fa19cad6f6c23131844143e27ccc`.
`tests/fixtures/edgejs-0.0.1-imports.txt` records its import section;
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
- The legacy module creation and state queries use the provider's bounded
  module handles. Its old heap-statistics layout is translated from one
  provider observation. Structured clone uses the existing transient budget.
- The old contextify cache entry compiles source without executing it and
  returns an empty guest-owned Buffer. It retains no code cache. The released
  guest reports `cachedDataProduced` as true when requested even though the
  buffer is empty; this legacy ABI does not pass that request to the host.

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
- The frozen worker probe now creates a worker, receives its posted message,
  and exits cleanly. The regression test runs whenever
  `NAPI_EDGEJS_0_0_1_ATOM` points to the exact released atom. Native environment
  IDs are allocated across all sessions of the instance, including WASIX
  workers; this prevents one worker's environment from resolving to another
  worker's native V8 environment. The earlier owning-thread assertion has not
  recurred in this probe.
- The frozen serdes binding, profiler/snapshot operations, and several stack
  introspection calls fail explicitly. Their existing native implementations
  retain buffers or samples without a workload budget, or have no equivalent
  in the current provider. Calling `node:v8` serialization therefore needs a
  metered implementation before it can be supported.
- A V8 fatal or out-of-memory callback records a workload stop if control
  returns to the N-API import boundary. V8 can terminate the host process
  before returning from a native fatal error; the callback alone cannot
  provide process-level fault containment.

These limitations are recorded so a successful import-only link or zero exit
from an ESM flag probe is not mistaken for application compatibility.
