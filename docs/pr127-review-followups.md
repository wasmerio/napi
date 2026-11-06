# PR #127 review follow-ups

The two local reviews of `fin/next` at `80b591e` reported 19 findings:
14 in `review-module-state.json` and five in `review-node-parity.json`.
The changes here start from published `next` at `e45b116`.

The numbering below follows the module report first, then the parity report.
Sixteen items have implementation fixes; three existing termination differences
have explicit API documentation rather than behavior changes.

| # | Finding | Disposition |
|---|---|---|
| 1 | QuickJS require facade bypasses termination | Check before running code and after evaluation; include the facade in both termination-call matrices. |
| 2 | Stored QuickJS module errors remain uncatchable after cancel; checkpoint exceptions cannot be taken | Clear stored errors' uncatchable flag on cancellation, and transfer failed-job exceptions into the env. |
| 3 | V8 facade instantiates in the current realm | Use the wrapped module's creation context. |
| 4 | Strong module wrapper references prevent collection | Use weak wrapper references and private JS edges for dependency wrappers; unreachable import cycles can be collected. The embedder still owns destruction of each native record. |
| 5 | QuickJS relinking after instantiation crashes | Refuse relinking after instantiation on both providers. |
| 6 | Destroyed V8 dependency record can resolve to a new module at the same address | Clear incoming record pointers at destruction. V8 refuses subsequent instantiation through them. |
| 7 | QuickJS destruction during terminated evaluation reads freed memory | Defer record destruction while linking, evaluating, or running synthetic steps. |
| 8 | QuickJS pending top-level await reports the wrong state and settledness | Map async evaluation to evaluated, inspect the linked graph for pending async evaluation, and warn with the module URL. Exact source positions remain unavailable. |
| 9 | V8 cached-data Buffer construction runs JS during termination and swallows exceptions | Guard termination and report Buffer getter/from exceptions; apply the shared helper's error status to bytecode serialization too. |
| 10 | QuickJS module creation/error construction runs JS during termination | Guard creation before binding lookups/setters, check source metadata setter failures and termination, and define module error codes as own properties. The engine's documented prepareStackTrace gap remains. |
| 11 | Throwing QuickJS error-code setter leaves an untakeable exception | Bypass the inherited setter when defining a module error's code. |
| 12 | QuickJS facade produces a spurious unhandled rejection | Mark the rejected facade promise handled before returning its reason. |
| 13 | Cross-realm links have different execution realms across engines | Refuse cross-realm links on both providers with ERR_VM_MODULE_DIFFERENT_CONTEXT. |
| 14 | Smaller module API result differences | Align link error codes/statuses, the absent source-object result, request-array realm, and evaluate_sync's async-module diagnostic. |
| 15 | V8 refuses error creation without a code during termination | Guard only the case with a code; document QuickJS's broader refusal. |
| 16 | Refusal status/pending state differs from Node's versioned shutdown preamble | Document the existing unofficial termination contract: napi_pending_exception and a pending marker for every module API version. |
| 17 | A retrieved QuickJS termination value remains uncatchable after cancel | Expose null from get_and_clear_last_exception for an internal termination marker, matching V8. |
| 18 | QuickJS resumes leftover promise jobs after cancel | Document existing queue persistence; embedders decide whether to run another checkpoint. |
| 19 | V8 builtins without user JS frames may finish after termination | Document V8's stack-check boundary and possible napi_ok result, preserving Node's behavior and DisallowJavascriptExecutionScope compatibility. |

## Validation

On macOS arm64, the complete native CTest suites pass:

| Configuration | Result |
|---|---|
| V8 Release | 356 passed |
| QuickJS Release | 306 passed |
| V8 Release with AddressSanitizer | 355 passed, one sanitizer-specific skip |
| QuickJS Debug with AddressSanitizer | 306 passed |

The ASan runs use `ASAN_OPTIONS=detect_leaks=0`; QuickJS Debug retains its
runtime leak assertions. Sixteen new test cases cover the reported failure
modes and wrapper/dependency GC behavior. The existing termination matrices
also exercise the facade before and during termination.

This pass does not include an EdgeJS build, WASIX/host-JS integration tests,
or performance benchmarking.
