# PR #127: module-state and Node-parity review assessment

Assessment date: 2026-11-06. Implementation and test results refer to
[`ab41860`](https://github.com/wasmerio/napi/commit/ab41860dbde7e58ca166f8cfebb93760d285d5f0),
which was pushed to `next` for [PR #127](https://github.com/wasmerio/napi/pull/127).
This document records the disposition of the findings; it does not imply that
all remaining engine differences or integration checks are complete.

## Summary

**Sixteen of the 19 findings have implementation fixes. Three Node-parity
findings received documentation changes while retaining their behavior.**

| Source report | Findings | Implementation fixes | Behavior retained and documented |
|---|---:|---:|---:|
| Module state | 14 | 14 | 0 |
| Node parity | 5 | 2 | 3 |
| Total | 19 | 16 | 3 |

The parity fixes are P1 and P3. Exact Node shutdown-status parity (P2) and
QuickJS microtask-discard parity (P4) remain unimplemented. P5 matches Node's
V8 behavior; the intended resolution is documentation, with no behavior change
planned in this follow-up.

“Implementation fix” means code changed to address the reported failure. It
is not a claim of complete Node equivalence, unrestricted module lifetime
semantics, or validation on every embedder and provider. The qualifications
under each finding and the outstanding validation below are part of this
assessment.

## Provenance and numbering

The original reviews examined `fin/next` at
`80b591ecb547623a0033d783c66186bc029c9d32`. Their files were saved in the
WebkitWasm checkout under `docs/research/napi-pr120-review-2026-11-05/`:

- `review-module-state.json`: 14 findings, identified here as M1–M14.
- `review-node-parity.json`: five findings, identified here as P1–P5.

That directory's name refers to the earlier PR #120 review, but these two
reports reviewed the later `fin/next` snapshot. They are separate from the
larger, older `review-findings.json` in the same directory.

The raw JSON reports remain local to WebkitWasm and were not included in the
napi push. This Markdown assessment and the implementation fixes are tracked
in napi. The implementation branch started at published `next` commit
`e45b11683d849bea4e1b21028699e205cc5e3406`.

The earlier assessment used one sequence numbered 1–19. M1–M14 correspond to
1–14, and P1–P5 correspond to 15–19. Severities below are the original
reviewers' ratings, rather than a new severity ranking. Some findings predate
`next`; the count is not a count of 19 new PR regressions.

## Findings at a glance

| Source ID | Earlier # | Severity | Provider | Disposition |
|---|---:|---|---|---|
| M1 | 1 | High | QuickJS | Implementation fix: require facade termination checks |
| M2 | 2 | Medium | QuickJS | Implementation fix: catchable stored module errors after cancel and retrievable failed-job exceptions |
| M3 | 3 | Medium | V8 | Implementation fix: facade uses module's creation realm |
| M4 | 4 | Medium | Both | Implementation fix: weak wrappers and GC-visible dependency edges |
| M5 | 5 | Medium | QuickJS; aligned on V8 | Implementation fix: reject relinking after instantiation |
| M6 | 6 | Medium | V8 | Implementation fix: invalidate incoming pointers on destruction |
| M7 | 7 | Medium | QuickJS | Implementation fix: defer destruction during active engine calls |
| M8 | 8 | Medium | QuickJS | Implementation fix: async status and unsettled-await detection; source-position limitation remains |
| M9 | 9 | Low | V8 | Implementation fix: cached-data termination checks and Buffer exception propagation |
| M10 | 10 | Low | QuickJS | Implementation fix: guard module creation and bypass error-code setters; engine stack-formatting gap remains |
| M11 | 11 | Low | QuickJS | Implementation fix: eliminate inherited code-setter exception residue |
| M12 | 12 | Low | QuickJS | Implementation fix: mark rejected facade promise handled |
| M13 | 13 | Low | V8; aligned on QuickJS | Implementation fix: reject cross-realm links |
| M14 | 14 | Low | Both | Implementation fix: align smaller module API results |
| P1 | 15 | Medium | V8 | Implementation fix: permit error creation without a code during termination |
| P2 | 16 | Low | Both | Documented divergence: Node's version-dependent refusal status/pending state is not implemented |
| P3 | 17 | Low | QuickJS | Implementation fix: expose null instead of an uncatchable termination value |
| P4 | 18 | Low | QuickJS | Documented divergence: leftover promise jobs remain queued after termination |
| P5 | 19 | Low | V8 | Documented Node-compatible limitation: native-only builtin callbacks can continue until a stack check |

## Module-state report: 14 implementation fixes

The main implementations are
[`quickjs/src/internal/napi_module_wrap.cc`](../quickjs/src/internal/napi_module_wrap.cc)
and [`v8/src/unofficial_napi_contextify.cc`](../v8/src/unofficial_napi_contextify.cc).
New module regressions are in
[`tests/runners/test_77_module_wrap_state.cc`](../tests/runners/test_77_module_wrap_state.cc).
The extended termination matrices are in
[`tests/runners/test_78_termination_until_cancelled.cc`](../tests/runners/test_78_termination_until_cancelled.cc).

### M1 — QuickJS require facade ignores termination

**Original severity: high. Status: implementation fix.**

The facade could link and evaluate a short module body after termination had
been requested, then return `napi_ok` and a namespace. QuickJS's periodic
interrupt polling did not reliably stop short bodies. Ordinary evaluation
already checked the termination flag, so this facade bypassed the provider's
shutdown contract.

`create_required_module_facade` now calls `fail_if_terminating` before running
code and checks termination again after evaluation. An interrupted evaluation
marks the promise handled, releases its temporary values, and returns the
termination failure. The facade was added to the existing matrices for calls
made after termination and calls whose execution is stopped by termination.
`FacadeDoesNotRunWhileTerminating` directly checks that the body does not run.

### M2 — Stored QuickJS module errors remain uncatchable after cancellation

**Original severity: medium. Status: implementation fix.**

A stopped module retained QuickJS's uncatchable interruption error as its
module-evaluation error. Cancellation cleared the environment's termination
flag but left the stored object uncatchable. Rethrowing that object or
encountering it through a module graph could bypass `catch` and `finally`
after cancellation. A failed pending job could also leave an exception in the
runtime without transferring it to the environment's exception slot.

Cancellation now visits module records and clears the uncatchable flag on
stored evaluation errors. The failed-job path in
[`napi_util__::run_pending_jobs`](../quickjs/src/internal/napi_util.cc)
transfers a runtime exception into the environment when execution is no
longer terminating, making it available through the public exception API.

`CancelledModuleErrorIsCatchableAndCheckpointExceptionCanBeTaken` checks that
taken and stored termination-related errors are catchable after cancellation;
on QuickJS it also checks replay of the errored module. The runtime-to-env
failed-job transfer was inspected, and the complete provider suites passed;
this follow-up does not claim a separate regression for every failed-job
variant from the original probe report. See P3 for the related
publicly retrieved termination value.

### M3 — V8 facade uses the current realm instead of the module's realm

**Original severity: medium. Status: implementation fix.**

A facade could instantiate a previously uninstantiated module using the
current N-API context. A module explicitly created in realm R could therefore
run against the environment's global, or the reverse, depending on which
realm was entered when the facade was requested.

The facade now scopes instantiation and evaluation to the wrapped module's
creation context. `FacadeRunsInModulesRealmAndCrossRealmLinksAreRefused`
checks the exported global marker when the facade is requested from another
realm. Cross-realm dependency links are handled separately under M13.

### M4 — Strong wrapper ownership prevents module and realm collection

**Original severity: medium. Status: implementation fix. Pre-existing.**

Each native module record strongly retained its JavaScript wrapper. EdgeJS
expects the wrapper's finalizer to destroy that native record, so the native
reference prevented the finalizer from running and retained the record and
its realm until environment release.

Both providers now hold the wrapper weakly. Dependency wrappers are retained
through private JavaScript references on the importing wrapper, making those
edges visible to the collector and allowing unreachable import cycles to be
collected. Explicit destruction also releases the importer's private
dependency references. Dependency-array construction defines own properties
so inherited array setters cannot intercept it.

The ownership contract still matters: the embedder owns native-record
destruction and should call `unofficial_napi_module_wrap_destroy` from its
wrapper finalizer. A raw native module handle alone does not retain the
wrapper. Callers must keep the wrapper reachable while using that handle.
This change does not introduce automatic native-record destruction for an
embedder that installs no finalizer.

Coverage includes collection of wrapper/dependency cycles, retention of a
dependency by a live importer, release of dependencies when the importer is
destroyed, and protection against inherited array setters. The complete
provider suites and ASan runs also cover the changed ownership paths.

### M5 — QuickJS relinking after instantiation can crash evaluation

**Original severity: medium. Status: implementation fix.**

Relinking replaced QuickJS's module request underneath an already instantiated
graph. Later evaluation could recurse into an uninstantiated replacement,
assert in Debug, or crash in Release. V8 kept its previously instantiated
graph, producing different behavior for the same API sequence.

Both providers now reject linking after instantiation with
`ERR_VM_MODULE_ALREADY_LINKED`. `RelinkingAnInstantiatedModuleIsRefused`
checks that the replacement is refused and evaluation still produces the
original dependency's value. Supplying links before instantiation remains
supported; this is a state-boundary check rather than a ban on all repeated
calls before instantiation.

### M6 — Destroyed V8 dependency records can resolve to unrelated modules

**Original severity: medium. Status: implementation fix.**

An importer kept raw pointers to dependency records. Explicitly destroying a
dependency freed its record, and a later allocation could reuse that address.
The importer could then resolve its old request to the unrelated new module.
Slab allocation made a plain ASan check insufficient to detect this logical
identity error.

Destruction now clears incoming native link pointers before the record can be
reused. V8 subsequently refuses instantiation through a missing dependency
instead of resolving it to a new record. The private wrapper edges from M4
also keep normally linked wrappers alive until their importer no longer
retains them.

`DestroyedDependencyCannotBecomeAnotherModule` checks that the unrelated
module never executes. Explicit destruction still differs between providers:
QuickJS retains its actual linked engine module and can continue using it;
V8 refuses later instantiation through the invalidated native link. This
boundary is documented. Embedders should avoid explicitly destroying a
dependency while an importer still needs it, and must never reuse a destroyed
native handle.

### M7 — QuickJS destruction during terminated evaluation reads freed memory

**Original severity: medium. Status: implementation fix.**

A native callback reached from a module body could destroy that module and
request termination. After `JS_EvaluateModule` returned, the termination path
still read the freed record's context while handling the evaluation promise.

A scoped active-call count now keeps the record and context alive during
linking, evaluation, facade execution, and synthetic evaluation steps.
Destruction requested while a call is active is deferred until its final
scope exits. The public lookup refuses a record already marked for
destruction.

`DestroyDuringTerminatedEvaluationKeepsRecordAliveUntilReturn` exercises both
ordinary evaluation and the facade. It checks that cancellation leaves the
environment usable. The complete QuickJS Debug+ASan suite passes with these
paths enabled.

### M8 — QuickJS pending top-level await reports incorrect state and settledness

**Original severity: medium. Status: implementation fix with a diagnostic limitation.**

QuickJS reported a module waiting on top-level await as “evaluating,” while
V8's public API reports it as “evaluated.” Its unsettled-await check always
returned settled. EdgeJS feeds those results into Node's module and exit
logic, so the mismatch could affect repeated evaluation and exit-time stalled
await detection.

QuickJS now maps asynchronous evaluation to the evaluated public state. The
unsettled-await check walks the linked record graph and detects asynchronous
evaluations that remain pending. With warnings enabled it prints the module
URL. `PendingTopLevelAwaitHasEvaluatedStatusAndIsUnsettled` checks state,
settledness, and another evaluation request while the await remains pending.

**Remaining limitation:** the QuickJS warning has no exact stalled source line
or column. An EdgeJS integration run confirming the final warning and exit
code has not been performed in this follow-up.

### M9 — V8 cached-data creation runs JS during termination and swallows errors

**Original severity: low. Status: implementation fix.**

Creating the output Buffer used raw V8 property reads and a call to
`Buffer.from`. A getter or that function could run after termination, and an
exception from `Buffer.from` could be swallowed while the helper fell back to
a plain `Uint8Array`.

Cached-data creation now refuses termination before the helper runs. The
shared Buffer helper also checks termination and captures JavaScript
exceptions, which are returned as pending exceptions instead of triggering a
silent fallback. Bytecode serialization, another caller of the same helper,
now preserves that pending-exception status too.

`CachedDataPropagatesBufferExceptionsAndRefusesTermination` checks the thrown
Buffer error and verifies that a Buffer getter does not run during
termination.

### M10 — QuickJS module creation and error paths run JS during termination

**Original severity: low. Status: implementation fix with an engine limitation.**

Module creation could call a JavaScript `internalBinding`, invoke wrapper
metadata setters, or run an inherited `Error.prototype.code` setter while the
environment was terminating. Raw engine calls bypassed the ordinary N-API
termination guards.

Source and synthetic module creation now refuse an existing termination
before those creation steps. Source creation checks termination after the
host-option lookup and metadata setters, and reports setter failures rather
than ignoring them. Module error codes are defined as own properties, which
bypasses inherited setters.

`ModuleCreationDuringTerminationRunsNoBindingOrSetters`,
`ModuleErrorCodeBypassesInheritedSetter`, and the termination suites cover
these paths.

**Remaining engine limitation:** QuickJS-ng can read `Error.prepareStackTrace`
while constructing an error, including while interruption unwinds. This
provider fix does not repair that engine-level behavior. It remains explicitly
documented in the public termination contract.

### M11 — A throwing QuickJS error-code setter leaves exception residue

**Original severity: low. Status: implementation fix.**

The module error helper ignored failure from setting `error.code`. If an
inherited setter threw, the environment held the intended module error while
the runtime retained another exception. After taking the module error,
“exception pending” could stay true and another take would fail.

The helper now defines an own `code` property and checks failures. No inherited
setter runs. `ModuleErrorCodeBypassesInheritedSetter` verifies that the intended
module error can be taken and that nothing remains pending afterward. This
shares the error-helper change with M10 but addresses a distinct reported
failure.

### M12 — QuickJS facade produces a spurious unhandled rejection

**Original severity: low. Status: implementation fix.**

When a module threw, the facade returned its error to the caller but left the
facade's rejected evaluation promise unhandled. An embedder could report an
unhandled rejection even though JavaScript caught the synchronous require
error.

The facade now marks that promise handled before extracting its rejection
reason. `FacadeRejectionIsHandled` verifies that rejection notifications are
balanced and the intended error is still returned.

### M13 — Cross-realm links execute differently across providers

**Original severity: low. Status: implementation fix through a stricter contract.**

V8 could instantiate a dependency in its importer's realm even when the
dependency had been created in another realm. QuickJS kept the dependency's
own realm. Node's vm layer rejects this cross-context arrangement before it
reaches the engine binding.

Both providers now reject cross-realm links with
`ERR_VM_MODULE_DIFFERENT_CONTEXT`. This makes the accepted API behavior
consistent and avoids silently executing against an unintended global. The
combined facade/realm regression checks the refusal.

Callers that previously supplied cross-realm graphs must instead create a
graph within one realm. This follow-up does not implement portable
cross-realm module execution.

### M14 — Smaller module API result differences

**Original severity: low. Status: implementation fixes.**

The report grouped several observable differences under one finding. The
follow-up aligns them as follows:

| Case | Result after the change |
|---|---|
| Link count mismatch | QuickJS uses V8's `ERR_VM_MODULE_LINK_FAILURE` and length-mismatch message. |
| NULL linked-module array with a matching nonzero count | QuickJS returns a pending link error, matching V8. |
| No module source object has been set | QuickJS returns `napi_ok` with a NULL result, matching V8. |
| Module-request array produced with a realm entered | QuickJS creates the array in the current N-API realm. |
| Async-module diagnostic from `evaluate_sync` | QuickJS includes the import hint and supplied filename/parent context used by V8's message. |

These alignments target provider consistency for the reported cases. They do
not claim that every module API error exactly reproduces Node's behavior;
Node sometimes rejects input at a higher JavaScript layer. Validation used
the complete module/realm suites and the added link/error regressions, rather
than a separate new test for every subcase in this grouped finding.

## Node-parity report: two implementation fixes, three retained behaviors

### P1 — V8 refuses error creation without a code during termination

**Original severity: medium. Earlier #15. Status: implementation fix.**

The new guard rejected `napi_create_error`, `napi_create_type_error`,
`napi_create_range_error`, and `node_api_create_syntax_error` even when no
error code was supplied. On V8, constructing that error runs no JavaScript;
only setting a code can run an inherited setter. Node permits the no-code
case. The reviewer identified the broad refusal as a regression from `main`.

V8's shared `CreateErrorCommon` now guards only the case where a code is
supplied. `ErrorCreationWithoutCodeUsesProviderTerminationPolicy` checks
successful no-code creation on V8 during termination.

QuickJS still refuses no-code creation because its error construction can
read `Error.prepareStackTrace`. The test pins that provider policy too, and
the public header explains the reason for the remaining difference.

### P2 — Refusal status and pending state differ from Node's shutdown preamble

**Original severity: low. Earlier #16. Status: documented divergence; exact Node parity remains unimplemented.**

The existing unofficial termination contract is retained: a refused call
returns `napi_pending_exception` for every `module_api_version` and leaves a
termination marker pending unless another exception already exists. Taking
that marker returns null on both native providers after P3.

The review's Node reference used `napi_cannot_run_js` for N-API version 10 or
later, `napi_pending_exception` for older versions, and no pending exception
for a refusal that never entered JavaScript. The old provider status already
differed; `next` broadened the observable difference through sticky
termination, additional guarded calls, and pending-state handling.

This follow-up **documents the divergence instead of changing that contract**.
An addon that specifically detects shutdown by comparing against
`napi_cannot_run_js` therefore still needs different handling here.

If exact Node parity is required, outstanding work includes defining how the
module API version affects refusals, separating refusal from actual engine
interruption, updating the public contract, and testing status and pending
state for versions below 10 and at least 10. That behavior change has not been
implemented or scheduled by this assessment.

### P3 — Retrieved QuickJS termination values stay uncatchable after cancel

**Original severity: low. Earlier #17. Status: implementation fix. Pre-existing.**

Native code could take QuickJS's uncatchable interruption error, cancel
termination, and later rethrow the saved object. Because taking the object
removed it from the environment's pending slot, cancellation did not find it;
its uncatchable flag survived and ordinary `catch`/`finally` could be bypassed.

[`napi_get_and_clear_last_exception`](../quickjs/src/js_native_api_quickjs.cc)
now returns null for the internal uncatchable termination marker, matching
V8's public value. It does not expose the engine's control-flow object. M2
separately handles evaluation errors still stored on module records.

The cancellation regression saves the taken value, cancels termination, then
throws that value from JavaScript and verifies that it is caught normally.

### P4 — QuickJS resumes leftover promise jobs after cancellation

**Original severity: low. Earlier #18. Status: documented divergence; queue-discard parity remains unimplemented. Pre-existing.**

When a QuickJS job terminates a checkpoint, `run_pending_jobs` stops and leaves
the remaining jobs queued. A checkpoint after cancellation can execute those
jobs, including work queued before shutdown. V8 discards remaining microtasks
when a running microtask terminates.

This follow-up **documents QuickJS's queue persistence without changing it**.
Embedders should run another checkpoint after cancellation only if they
intend the queued jobs to resume. The review identified this as a possible
shutdown/cleanup concern, but did not prove that EdgeJS's teardown runs the
checkpoint needed to expose it.

Matching V8 would require an engine-safe way to discard pending jobs without
executing their JavaScript or losing their context/resource cleanup. A future
change should test that subsequent jobs do not run after cancel, that their
contexts are released, and that newly queued work after cancellation still
runs normally. Neither queue discarding nor the embedder teardown validation
has been completed here.

### P5 — V8 builtins can continue native callbacks after termination

**Original severity: low. Earlier #19. Status: documented Node-compatible limitation; no behavior fix planned in this follow-up.**

A builtin invoked directly from C++, such as `Array.prototype.forEach`, can
call native callbacks with no user-JavaScript frame beneath them. V8's stack
trace does not count the builtin as a user frame. After a callback requests
termination, the builtin may finish its callbacks before reaching another
stack check, and `napi_call_function` can return `napi_ok`. QuickJS stops the
corresponding sequence immediately.

The reviewer measured the same behavior in Node. Termination remains
requested, so later calls into JavaScript are still refused. This is a
cross-engine limitation and an overbroad documentation promise, rather than
a demonstrated Node-compatibility regression.

The public contract now describes that boundary. No forced-unwind behavior
was added: running extra JavaScript merely to trigger an unwind could conflict
with an embedder's `DisallowJavascriptExecutionScope`. A dedicated builtin
callback-count regression would be useful if this boundary is changed later,
but this follow-up does not change it.

## Remaining work and limits

### Behavior work not implemented

| Item | Remaining work | Current treatment |
|---|---|---|
| P2 | Node-compatible refusal statuses and pending-state behavior by API version | Documented divergence; retain the existing unofficial API contract. |
| P4 | Discard interrupted QuickJS promise jobs safely instead of resuming them after cancel | Documented divergence; keep existing queue semantics. |
| P5 | No behavior change planned | Document the Node-compatible V8 stack-check limitation. |

P2 and P4 remain behavior differences if strict Node/V8 parity is the goal.
P5's documentation is the selected resolution, so it should not be counted as
an unimplemented Node bug. “All 19 addressed” must not be read as “all 19 now
have identical engine behavior.”

### Module and engine limitations that remain

- QuickJS unsettled-await warnings identify a module URL, without exact source
  line/column information (M8).
- QuickJS-ng's error stack construction can invoke `Error.prepareStackTrace`
  during interruption; closing that gap requires engine work (M10/P1).
- Native module-record lifetime remains an embedder responsibility. A weak
  wrapper does not automatically destroy a native record unless the embedder
  arranges destruction, for example through its wrapper finalizer (M4).
- Explicitly destroying a dependency while an importer still needs it has
  documented provider-specific outcomes. V8 refuses later instantiation;
  QuickJS may use its retained engine module (M6).
- Cross-realm module links are now rejected, rather than given portable
  cross-realm execution semantics (M13).

## Validation completed

The recorded runs tested implementation commit `ab41860` on macOS arm64.
These are native CTest results, not a claim that the original reviewers'
external probe files were all rerun unchanged.

| Configuration | Passed | Failed | Skipped |
|---|---:|---:|---:|
| V8 Release | 356 | 0 | 0 |
| QuickJS Release | 306 | 0 | 0 |
| V8 Release with AddressSanitizer | 355 | 0 | 1 |
| QuickJS Debug with AddressSanitizer | 306 | 0 | 0 |

The V8 ASan skip is
`Test66ForegroundTaskHandoff.UninitializedArrayBuffersAreFreedWithoutACheckpoint`,
an existing sanitizer-specific skip. ASan runs used
`ASAN_OPTIONS=detect_leaks=0`; QuickJS Debug retained its runtime leak
assertions. These runs therefore provide memory-access and Debug assertion
coverage, not a LeakSanitizer proof that every allocation is reclaimed.

Sixteen new test cases were added across the module-state and termination
runners. They cover the principal facade, destruction, realm, state,
exception, rejection, and wrapper/dependency-collection cases. The existing
termination matrices were also extended to include the require facade.
Individual coverage qualifications, including grouped cases without a new
standalone test, are recorded above.

Typical native rerun commands, after configuring the appropriate builds:

```sh
ctest --test-dir build-napi-v8 --output-on-failure -j8
ctest --test-dir build-napi-quickjs --output-on-failure -j8
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-review-v8-asan --output-on-failure -j8
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-review-qjs-asan --output-on-failure -j8
```

## Validation still pending

The following checks were not run as part of this implementation follow-up:

- **EdgeJS integration:** build against the updated napi revision and exercise
  module-wrapper finalization, vm modules, ESM require, pending top-level
  await exit handling, worker shutdown, and process-exit cleanup.
- **WASIX/guest ABI and host-JS integration:** verify the relevant bridge,
  guest, and host-JS paths against the updated native behavior. No new Rust,
  WASIX manifest, wasm-bindgen, or host-JS integration run is claimed here.
- **Performance:** benchmark module creation, linking, facade evaluation,
  graph destruction, and GC/realm retention with the new ownership edges.
  Passing functional tests does not establish a performance result.
- **Full original-probe replay:** the reports' external probe suites are not
  part of this napi commit; the added regressions and complete native suites
  do not establish that every original probe was rerun.

None of these pending checks is represented as passed by the native test
counts. Further engine changes for P2/P4 would also need their own focused
regressions and renewed integration validation.

## API and design references

The retained termination differences and module lifetime/linking contracts
are documented in [`include/unofficial_napi.h`](../include/unofficial_napi.h).
The realm and ownership overview is in [`docs/realms.md`](realms.md).
