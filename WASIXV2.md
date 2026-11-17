# Edge WASIX v2 integration

This branch starts from `edge-napi-webassembly-metered` at
`af20edeae76192e3cdd49c5966c34cc81d0192d6`. It composes the Wasmer-next
integration from `next` at `c17595ac738853fa3935096bf3ec28718c62deda` with
Edge's native provider. The merge-base for that integration is
`82b328f50ddaac78de388b67b4707f21864b9245`.

Native embedders register `NapiHook::new(context)` or
`NapiHook::from_hooks(builder.build_managed_imports(activator))` with the
v2 `MachineBuilder`. Hook configuration binds each actual Store/instance to
its own session, protected guest arena and kernel-backed refill. The original
runner owns deactivation and lease-release cleanup. The hook does not own a
Store. Ordinary low-level NapiSession entry points remain available.

Managed lane activation stays lazy. Its callback runs from the parked native
creation owner, outside borrowed Store access. The lane, policy, shared budget
and unpublished provider move together through creation and physical retirement.
Soft charges and provider-limit reporting retain Edge's budget pools; protected
lease accounting identifies already-funded engine memory to avoid double billing.
The native bridge retains Edge's frozen legacy imports and native policy/code
budget controls alongside the v2 frame/GC and callback error custody.

The v2 facade configures the attachment before invoking the deferred Wasm start
section. NAPI activation during start therefore uses the same protected heap and
refill as ordinary entry. No extra NAPI hook phase or opt-in is needed. Source
must pass through exec/facade loading or instrumentation. The CLI's permitted
instrumentation fallback uses `InstrumentError::fallback_source`, preserving
start deferral and its admission metadata in the cached compiler input.
Direct unconfigured instantiation still rejects NAPI activation; it bypasses this
facade lifecycle. Old compiled caches must be invalidated when adopting deferral.

The native engine no longer has the private v1 `do_wait_interruptible` method.
WASIX v2 execution rewrites guest atomic waits through the kernel's cancellable
futex path. Low-level raw engine waits retain the engine's ordinary semantics.

Build this submodule in the Wasmer-next workspace (one Git engine source and the
sealed local WLK crates). The NAPI CI profile is `cargo check -p wasmer-napi
--features cli`; native SDK uses `wasmer-sdk/napi-v8`. Browser builds use
`--target wasm32-unknown-unknown --no-default-features --features js,wasix`.
A check proves compilation, not native linking, runtime metering, or browser
execution. Platform/frame-provider behavior needs the retained native/JSPI and
browser suites in CI.

## Initial port validation

Checked in the isolated `wasmer-next-napi-check` clone of product
`95918f0bd0b5db60451a9adff1dfb76bf879656e`, whose lock pins runtime
`9cc2b035f6715cbf47b3efa9f481673f2949705d`:

- `cargo check -p wasmer-napi --features cli`
- `cargo check --offline --locked -p wasmer-napi --features cli --all-targets`
- `cargo check --locked -p wasmer-cli -p wasmer-c-api -p wasmer-sdk -p wasmer-napi --features wasmer-cli/cranelift,wasmer-sdk/napi-v8,wasmer-napi/cli`
- `cargo check --offline --locked -p wasmer-napi --lib --no-default-features --features js,wasix --target wasm32-unknown-unknown`

All passed (with warnings). Rust formatting and `git diff --check` passed.
The build downloaded V8 and compiled the native bridge. No executable linking,
runtime tests, browser execution or cross-platform execution was performed.
The standalone lock was generated and pinned to this same product/engine pair.
The later exec start-deferral work is not part of this validation snapshot.

## Deferred-start validation

Updated against wasmer-next main `a49d8ddce79278d944c3214e31241e8a9dbcadf7`
and its locked runtime `93ebbc98e695c0727f9107c444edfb166bf22ac6` in the
same isolated scratch checkout. The scratch product lock required only the
NAPI `parking_lot` dependency entry; no product source was changed. The standalone
lock now selects this product/engine pair.

The combined CLI/C API/SDK/NAPI check, native NAPI all-targets check, and wasm32
`js,wasix` library check above all passed again with `--locked`.

`cargo test --locked --offline -p wasmer-napi --features cli --test wasix_facade
facade_native_start_evaluates_js_with_protected_leases -- --exact --nocapture`
passed without an external stack override. It executes the Edge-style start
section reading `41+1` from imported guest memory, then a variant allocating a
32 MiB Uint8Array during start to require protected lease refill. Both verify
`done` output and retired session/isolate/memory accounting.

The first execution exhausted libtest's default host thread stack. The native
subprocess harness now explicitly uses a 16 MiB host test-thread stack; guest
stack limits are unchanged. The focused test passed after that harness fix.
Broader runtime suites and browser execution were not run.

## Independent review repairs: upstream providers

The recursive merge had two merge bases and silently interpreted PRs #67–#77
as reversions. Restore their provider delta from `82b328f50..a65e9cb86`:
QuickJS 0.17 (`89fb17a8`), external-value/liveness fixes, handled synchronous
module rejections, browser synchronous module graph traversal/evaluation, and
typed-array/promise/module-evaluation regression coverage. Browser changes use
next's short `LiveEnv` loans rather than restoring long Rust state borrows.

V8 stays at **11.9.7** consistently across Cargo, Make, fixture scripts and
CMake. next's native stack and GC adapters certify exactly that archive on
Linux x64 and Darwin arm64. Updating to 11.9.9 requires separately certifying
those adapters. Product `.github/workflows/build.yml` explicitly disables
`enable_napi_v8` for Linux arm64; its enabled native jobs use other platforms.
Linux arm64 can use an explicitly supplied local provider (including upstream's
`libatomic` link), but there is no certified 11.9.7 Linux arm64 prebuilt/managed
coroutine provider. Do not substitute an unverified 11.9.9 archive.

Actual native CTest runs: V8 **113 passed, 0 failed**, QuickJS **102 passed,
0 failed**. These use Make's existing deep-freeze test exclusion. V8's invalid
policy test now uses 3: policy 2 is the valid Edge metered policy.

## Independent review repairs: ownership and lifecycle

- C1/C2/H2/L1: holder-aware message drop/take/dispose; 64-byte deferred charges;
  finalizer record erase/uncharge and the cancelled-record teardown sweep;
  serialized-message native overhead restored.
- H1: forward shared-memory detachment, keep the charge with the retained owner,
  and delete the obsolete legacy-wait cfg. The ownership regression now checks
  last-owner release. Defined-memory tests use the production wrapper once.
- H3/M4: provider fatal/OOM termination walks all sibling isolates and clears
  pending messages while parked. Limit notifications drain even on callback
  failure/panic, preserving the original failure and avoiding later Env access.
  Provider fatal errors remain terminal v2 RuntimeErrors, rather than fabricating
  a v1 WasiError::Exit(1).
- H5/M2/L2–L5: direct configuration binds exported/imported memory without a
  prefund, accepts memoryless inert modules, and creates the heap only on use.
  Missing memory, stop and lane admission precede funding. Accepted lanes survive
  later creation failures. Runtime initialization occurs in that lane; unmanaged
  consumers retain next's explicit runtime flags. Output validation allocates
  nothing. Cancellation after host stop re-terminates defensively.
- M1: the default policy preserves SDK WebAssembly availability. Edge selects
  Restricted or EnabledMetered explicitly. Tests for restriction configure it
  explicitly; the three H5 Edge tests retain their assertions.
- L6/L7/L8/L10: release rejected allocator contexts in the allocator scope, park
  legacy bridge calls, retain positive configure-before-start coverage, and remove
  dead value-limit plumbing. Raw direct C++ constructors retain their published
  provider contract; Rust uses a separate private pending constructor and next's
  prepare/publish retirement protocol.
- M3: clone/thread detachment no longer takes the failing mutation token, with a
  concurrent grow/clone regression. Copy still needs a serialized high-water
  snapshot; a TODO records the callback-safe read-guard work. It can reject a
  concurrent mutation, rather than risk copying uncharged retained backing.
- L9: restored unknown-policy allocator ownership coverage against NativeGuestArena.
  A TODO records stale nonshared direct-memory registrations retained by messages;
  protected leased arenas retain and authenticate their physical allocation.

Some retained next tests assumed uncharged host copies, eager direct prefunds,
process-wide message ids across unrelated apps, or an unused value-limit query.
Their harnesses now exercise the same intended lifecycle cases under Edge's
accounting and context ownership. The bounded facade denial fixture leaves room
for the restored V8 heap/slack and bookkeeping charges before denying a 32 MiB
buffer. Explicit-release finalizers were verified through the SDK.


## Final review validation

Validation uses the isolated product checkout
`/home/theduke/dev/github.com/wasmerio/wasmer-next-napi-check` at GitHub main
`9c23b0c2c3591021a5b11ce69a91bd384aa0d387`, with locked runtime
`93ebbc98e695c0727f9107c444edfb166bf22ac6`. No source in the main product
checkout or runtime repository was changed. The product lock's only local
change adds `parking_lot 0.12.5` to wasmer-napi's dependency list.

The full native command was:

```sh
RUST_MIN_STACK=16777216 cargo test --locked --offline --no-fail-fast \
  -p wasmer-napi --features cli -- --test-threads=1
```

It executed every native target, including the original manifest fixtures. Its
one remaining library failure exposed an Edge exact-memory check against next's
source-retaining shared-memory transfer test. The fix permits an owned, pinned
shared mapping across this app's worker memories, keeping app ownership and
nonshared exact-memory checks. After that change this command passed:

```sh
RUST_MIN_STACK=16777216 cargo test --locked --offline --no-fail-fast \
  -p wasmer-napi --features cli --lib --test message_memory \
  --test legacy_manifest -- --test-threads=1
```

The table reports the last successful run of each target; it does not claim
that the earlier full invocation exited successfully. Consolidated native
results: **282 passed, 0 remaining failures, 3 ignored**.

| Target | Passed | Failed | Ignored |
| --- | ---: | ---: | ---: |
| wasmer_napi | 137 | 0 | 0 |
| napi_wasmer | 4 | 0 | 0 |
| callback_control | 38 | 0 | 0 |
| callback_control_js | 0 | 0 | 0 |
| cli_facade_mounts | 2 | 0 | 0 |
| core_callbacks | 2 | 0 | 0 |
| external_transfer | 1 | 0 | 0 |
| fatal_hook_lookup | 1 | 0 | 0 |
| guest_boundary_accounting | 6 | 0 | 0 |
| guest_error_callbacks | 1 | 0 | 0 |
| legacy_manifest | 12 | 0 | 1 |
| manifest_tests | 1 | 0 | 0 |
| message_memory | 3 | 0 | 0 |
| native_jspi | 1 | 0 | 0 |
| review_memory | 6 | 0 | 0 |
| v8_heap_limit | 6 | 0 | 0 |
| v8_host_control | 21 | 0 | 0 |
| v8_lazy_lane | 1 | 0 | 0 |
| v8_page_accounting | 9 | 0 | 1 |
| v8_wasm_guest | 3 | 0 | 0 |
| v8_wasm_metering | 11 | 0 | 1 |
| v8_wasm_process_budget | 1 | 0 | 0 |
| v8_wasm_tiering | 1 | 0 | 0 |
| v8_worker_threads | 1 | 0 | 0 |
| wasix_facade | 10 | 0 | 0 |
| wasix_facade_js | 0 | 0 | 0 |
| wasix_leases | 1 | 0 | 0 |
| wasix_pending | 2 | 0 | 0 |
| doctests | 0 | 0 | 0 |

`manifest_tests` is one Rust test covering **34 original native fixtures and
34 corresponding WASIX guests**, all executed and compared. Native fixtures
were linked from their original sources and `tests/napi_native_init.cc` against
the CTest-verified CMake V8 provider archive, avoiding 34 recompilations of that
same provider. The normal fixture script then verified their freshness. The
WASIX fixtures were produced with the installed wasixcc 0.4.2/sysroot; none were
substituted with synthetic success fixtures.

The public SDK NAPI test passed: **1 test, 0 failures**, executing eight runs of
its three original fixtures across independent sandboxes. The facade's ten
native tests include configure-before-start guest-memory JS evaluation and a
32 MiB start-time protected-lease refill scenario.

Native CTest: V8 **113/113**, QuickJS **102/102**; Make's existing
`SandboxGlobalThisAndMarkerAreNotEnumerableForDeepFreeze` exclusion remains.

The combined CLI/C API/SDK/NAPI `cargo check --locked --offline` with
`wasmer-cli/cranelift,wasmer-sdk/napi-v8,wasmer-napi/cli` passed. The browser
`wasm32-unknown-unknown` library check with `--no-default-features --features
js,wasix` passed. The native NAPI `--features cli --all-targets` check also passed.

Browser runtime execution was attempted by building `wasix_facade_js`; it
could not link `__wasixv2_thread_cleanup` with the installed stable std.
The browser host's documented prerequisite is its separately patched nightly
std (`wlk/crates/host/tests/browser/prepare-std.py` and build guide), which is not
installed here. Therefore **0 browser runtime tests executed**; this is not
reported as a passing browser suite. No product/runtime API or fake cleanup
stub was added to manufacture a result.

The first broad test build exhausted local disk during simultaneous debug
linking. Task-owned debug sections/incremental outputs were removed and build
concurrency reduced to two; the completed runs above follow that recovery.
Existing dependency/deprecation warnings remain.

Manager integration: set `lib/napi`'s tracked branch to `edge-wasixv2`, bump its
gitlink to this branch's final head, and include the lock dependency entry for
`parking_lot 0.12.5`. No product submodule pointer was changed in its main
checkout. No further blocking facade API GAP was found. Remaining TODOs are M3
(copy contention/read guard) and L9 (direct nonshared registration retirement).

## Protected heap prefunding follow-up

Protected allocator backing is now funded after managed-lane admission and before
planning/reserving an isolate's heap ceiling. A clamped ceiling therefore sees
the already-funded arena rather than consuming the bytes needed to create it.
Protected host reservations use soft WasmLinear accounting, including memory
growth through budgeted tunables. The budget-specific scope covers only each
reservation poll and restores the previous mode on return, Pending or panic;
ordinary guest memory growth retains its existing accounting.

The protected-lease suite adds a 160 MiB aggregate-budget regression with a
104 MiB guest, verifying the actual protected heap acquired its 16 MiB prefund
before isolate ceiling planning. A budget unit regression
checks actual memory-growth denial and restoration after Pending and panic.
NAPI-only file command admission remains an upstream facade/exec gap; this
change neither alters its fixtures nor adds a synthetic WASIX import to them.

Validation on product `9c23b0c2c3591021a5b11ce69a91bd384aa0d387`
(runtime `93ebbc98e695c0727f9107c444edfb166bf22ac6`):

```sh
CARGO_TARGET_DIR=$PWD/target RUST_MIN_STACK=16777216 CARGO_BUILD_JOBS=2 \
  cargo test --quiet --locked --offline --no-fail-fast \
  -p wasmer-napi --features cli -- --test-threads=1
```

The final full invocation passed: **284 passed, 0 failed, 3 ignored**, including
all 34 original native/WASIX fixture comparisons. The protected-heap order
regression was separately proven failing with the original order and passing
with the fix. V8 CTest passed **113/113** and QuickJS CTest **102/102**, retaining
Make's existing deep-freeze exclusion. Rust formatting and `git diff --check`
passed. Strict Clippy remains blocked by three unchanged `build.rs` findings:
`manual_is_multiple_of` at line 394 and `type_complexity` at lines 422 and 759.

Edge validation uses a detached scratch worktree of `wasixv2/n` at
`9f80e75debef8d9b43edbcce68b5479ac8d975ef`, with a local `wasmer-napi` patch
pointing at this branch's sources. The scratch lock changes only that package's
source identity; product stays pinned to `2ae152aa3aaa0dac00b74c58ece2121f9c77ae0e`.
Build outputs live in the scratch worktree. The build uses installed Nix
CMake/pkg-config and OpenSSL (`OPENSSL_NO_VENDOR=1`). No Edge source or commits
are changed. The focused command passed **1/1**:

```sh
RUST_MIN_STACK=16777216 cargo test --quiet --offline -p edge \
  --test napi_metering wasm_memory_shrinks_the_v8_heap_within_one_quota \
  -- --exact --nocapture
```

The full Edge command (`cargo test --quiet --locked --offline -p edge --test
napi_metering -- --test-threads=1`) finished **14 passed, 4 failed**. All four
remaining failures report `ENOEXEC` from NAPI-only file admission:

- `importing_napi_with_exported_memory_does_not_initialize_it`
- `importing_napi_without_calling_it_does_not_start_a_v8_lane`
- `legacy_create_env_activates_one_instance_lane_on_first_call`
- `legacy_default_flags_do_not_activate_or_charge_an_instance_lane`

NAPI-ABI remains assigned upstream; no admission workaround or Edge fixture
changes are included here. The start-trap test nominally passes, but its error
and drain assertions alone do not establish that NAPI activated before the
failure, so that result is not evidence of resolved NAPI-only start admission.
