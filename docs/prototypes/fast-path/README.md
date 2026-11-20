# N-API call path: how fast it can get

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟠 | Research prototype only, measured on macOS arm64. Not proposed for merging; this branch exists to keep the code. |
| **Severity** | Low | Performance research; no runtime behaviour changes in the repository. |

Follows the import-overhead analysis behind PR #174. The
question on 2026-11-20 was how fast the whole path can get: Wasmer's host
call and stack switch, the Rust import body, the C++ bridge, the V8 provider
and the JavaScript-to-guest callback.

## Method

- Reference: N-API PR [wasmerio/napi#174](https://github.com/wasmerio/napi/pull/174)
  on Wasmer PR 7198 (`afa25e62`), N-API imports on the native stack.
- Six layers were profiled and prototyped separately in scratch copies. Each
  prototype was then reviewed by a second pass whose job was to refute its
  soundness and its timings. A last pass merged what survived, ran the
  tests, and timed the result.
- Timing: `tests/perf_calls.rs`, release, one thread, 7 rounds, median ns
  per call, three alternating runs with no other build running.
- I re-ran the saved reference and combined binaries once afterwards
  (5 rounds) and re-ran the four-server smoke test; both match the tables.

## Result

| Case | Reference | Combined, default mode | Combined, thread-stack mode | Unsound upper bound |
| --- | ---: | ---: | ---: | ---: |
| `napi_get_value_int32` | 17.3 | 5.6 | 5.6 | 5.6 |
| `napi_get_undefined` | 16.6 | 5.2 | 5.2 | 4.9 |
| JS -> guest `g()` | 53.3 | 29.4 | 26.9 | 24.4 |
| JS -> guest `f(x)`, three imports | 159.5 | 69.9 | 66.8 | 62.9 |
| `napi_get_named_property` | 60.6 | 39.2 | 34.6 | 29.5 |
| `napi_call_function`, no-op | 51.8 | 43.4 | 38.4 | 33.8 |

"Thread-stack mode" runs the guest on the thread's own stack (see Wasmer
below). The upper bound removes checks that soundness needs (TryCatch,
termination and type checks, owner-thread check, depth bound); it is only
2 to 4 ns below the sound build.

Extra cases, one run each:

| Case | Reference | Default | Thread-stack |
| --- | ---: | ---: | ---: |
| Nested JS -> guest -> JS -> guest | 335.7 | 106.9 | 92.3 |
| Getter `o.p` | 84.5 | 40.7 | 41.0 |
| `napi_create_string_utf8`, 11 bytes | 43.2 | 25.3 | 20.5 |
| `napi_get_value_string_utf8`, 11 bytes | 51.6 | 18.1 | 13.8 |
| `napi_call_function`, 2 arguments | 96.6 | 45.8 | 40.2 |
| `napi_get_buffer_info` | 41.2 | 30.0 | 29.1 |
| `napi_create_int32`, scoped | 21.2 | 11.6 | 11.6 |

Framework servers, informal (short runs, 32 connections, two rounds, zero
errors; reference is the #174 runner of note 006):

| Server | Reference req/s | Default mode | Thread-stack mode |
| --- | ---: | ---: | ---: |
| node:http | 52,600 to 53,000 | 66,500 to 67,200 (+26%) | 71,100 to 71,300 (+35%) |
| Express | 31,400 to 31,600 | 40,500 to 40,800 (+29%) | 42,600 to 42,800 (+36%) |
| Hono | 45,100 | 56,700 to 58,000 (+27%) | 60,600 to 60,900 (+35%) |
| Next.js | 3,970 to 4,000 | 5,130 to 5,140 (+29%) | 5,380 (+35%) |

## What the combined prototype contains

**Wasmer (PR 7198 copy)**

- Thread-stack mode, `WASMER_WASM_ON_THREAD_STACK=1`, aarch64 Apple only,
  off by default. A call into Wasm from the thread's own stack is a plain
  call below the caller's frame, with trap recovery by a recorded
  continuation. Every host call then runs in place, so V8 sees one
  contiguous stack and no stack switch happens in either direction.
- A guarded in-place wrapper: a function flagged for the native stack runs
  in place only when the guest is on the thread's stack. N-API keeps
  flagging its imports.
- Four entry coroutines instead of two. With two, the third nested
  activation costs about 160 ns more.
- Wasmer-only numbers: Wasm-to-Wasm call 0.66 ns; host call in place 2.0;
  host call with the stack switch 4.5 to 4.9; host-to-Wasm entry 10.7
  (9.6 in thread-stack mode).

**Imports that run in place (default mode)**

- An allow-list of 33 imports that never allocate on the V8 heap, run
  JavaScript, throw or call the guest runs on the Wasm stack. The four
  number-creating imports run in place for small integers and switch to the
  native stack otherwise.
- The in-place bridge scope skips the heap-lease reconcile and the lane
  binding.

**Rust import body**

- A per-env window on guest memory for scalar reads and writes.
- One-pass property-name copy; strings written straight into the guest
  buffer; call arguments read without allocation.
- `HotEnv`: imports are closures that hold the env's hot record, so they
  skip the env-data lookup.
- Lean `napi_get_cb_info` (lock-free refill flag, one env read).

**C++ bridge**

- `BridgeEnvScope` with one thread-local block and one lane swap.
- Guest callbacks without the provider's escapable handle scope; V8 already
  scopes API callbacks.
- Flat slot stack for scope frames, vector callback registry, one id per
  agent for undefined, null, true and false.

**V8 provider**

- A name cache keyed by guest address and checked against the bytes, so the
  named-property imports do not create a V8 string per call.
- Call arguments passed without a copy, `context()` without a handle, Smi
  fast path for number getters, inline last-error clear.

**Callback path**

- Rust trampoline without env-data lookups, and a 64-entry guest-function
  cache per callback context.

Dropped: a frameless dispatcher in Wasmer (superseded), ThinLTO for the C++
objects (no gain on the merged tree), and every unsound variant.

## Where the time is now

- Trivial import, 5.6 ns: Wasmer in-place wrapper about 1.2, Rust 0.7,
  bridge 1.5, one thread-local access 0.6, provider and V8 1.3.
- Named get, 34.6 ns: V8 `Object::Get` 15.7, Rust 4.9, bridge 4.5, Wasmer
  3.0, provider 2.5. Default mode adds 4.6 ns for the stack switch.
- `napi_call_function`, 38.4 ns: V8's `Function::Call` path is 19.6.
- `g()`, 26.9 ns: Wasmer host-to-Wasm entry 8.2 (now the largest item),
  bridge 6.5, Rust trampoline 2.1 plus 2.9 for the lend cell, V8 2.9,
  provider 2.0.
- `f(x)`, 66.8 ns: the three imports cost about 40 ns, of which
  `napi_get_cb_info` is about 21.

## Estimated floor with further work

These are estimates, not measurements.

| Case | Measured sound | Estimated reachable | What it needs |
| --- | ---: | ---: | --- |
| Trivial import | 5.2 to 5.6 | about 4; 2.5 to 3 for singletons | Fixed ids answered in Rust with no C++ call. Wasmer's in-place host call (2.0 ns) is the hard floor. |
| `g()` | 26.9 | 17 to 20 | Leaner Wasmer entry (cached vmconfig, direct typed call, trap recovery by unwinding), lend cell reached without a thread-local. |
| `f(x)` | 66.8 | 40 to 45 | The host writes argc, `this`, data and argument ids into guest memory before the call, so `napi_get_cb_info` needs no import. |
| Named get | 34.6 | 28 to 30 | V8's `Object::Get` at 14 to 16 ns is the floor through the public API. |
| `napi_call_function` | 38.4 | about 33 | V8's call path is about 20 of it. |

## Verification

- Debug build, both modes: `--lib` (86), the `guest_*` suites,
  `concurrent_envs`, `concurrent_stores`, `message_memory`, `v8_heap_limit`,
  and the new suites (`guest_in_place_imports`, `guest_fast_paths`,
  `name_cache`, `thread_stack_mode`, `thread_stack_trycatch`) pass. One
  in-place test needed a mode-aware assertion (see caveats).
- Release with `--features cli`, both modes: `boundary_charges`,
  `budgeted_host_variables`, `managed_host_variables`,
  `resource_limit_rules`, `v8_host_control`, `v8_lanes`,
  `v8_lanes_after_standalone` pass.
- `node:http`, Express, Hono and Next.js each answered 2,000 requests with
  200 and the expected body in both modes.
- Not run: Wasmer's own tests on the patched copy, Miri, the wasm32 host-JS
  build, QuickJS (shares `lib/src`), the js-native-api suites, the Edge.js
  test suite, anything on Linux.

## Caveats

- Thread-stack mode is a prototype for aarch64 Apple; production Edge.js
  runs on Linux. It changes behaviour: a guest more than about 1 MiB deep
  cannot call JavaScript, the configured Wasm stack size is ignored, and a
  host call with under 256 KiB of thread stack left traps with
  StackOverflow. WASIX syscalls then also run in place; that was exercised
  only by the four-server smoke test.
- The in-place allow-list was audited against the unmodified bridge and
  provider. The merge changed both under it; the merged code passed the
  in-place suite but was not re-audited import by import.
- The name cache was timed at a 100% hit rate. A miss costs 1 to 4 ns more
  than the reference, and a worst-case eviction pattern 6 to 9 ns more. Its
  hit rate in Edge.js is not measured.
- No run used a memory budget. With one, every lending import makes a
  second bridge call for the heap settle.
- `HotEnv` relies on Wasmer's sys backend calling capturing closures as
  host functions, which works but whose doc comment says is unsupported. A
  1.2 ns slowdown of `g()` at that stage was not explained.
- Guest-visible changes: stable ids for undefined, null, true and false;
  heap-lease reconciliation deferred past in-place imports.
- The combined tree is a hand merge of six prototypes, about 11,400 patch
  lines across N-API and Wasmer. It is evidence of what is reachable, not
  a reviewable change.
- The reference's unscoped `napi_get_undefined` case mostly measures a
  table-full path where `StoreValue` returns id 0 with `napi_ok` once the
  1M-slot table fills. That is an existing bug in the reference.

## Suggested order if this is taken further

1. N-API only, no Wasmer change, default mode: bridge scope, callback
   without the provider scope, flat slot stack, singleton ids, Rust window
   and one-pass names, lean `napi_get_cb_info`, call arguments without
   allocation.
2. The in-place allow-list (N-API, needs PR 7198's in-place host calls).
3. Name cache, after measuring its hit rate on a framework request.
4. Wasmer: four entry coroutines or a one-call coroutine, then the leaner
   entry path.
5. Thread-stack mode as a real Wasmer feature, starting with Linux.

## My read

Written by Claude on 2026-11-20, after reading all six layer reports, their
reviews and the merge report, and re-running the timings once.

**What I trust**

- The six headline timings. Three passes measured them independently (layer
  author, reviewer, merge) and my own re-run of the saved binaries matched
  within 1 to 2 ns.
- The direction of the server numbers. Four servers moved by a similar
  amount in both rounds with zero errors. The exact percentages come from
  short runs and I would re-measure before quoting them.
- The N-API-side changes that only remove work: the bridge scope, the
  callback without the provider scope, the flat slot stack, the guest-memory
  window, the one-pass name copy and the allocation-free call arguments.
  Reviewers tried to break each and could not, and the test suites pass.

**What I trust less**

- The merged tree as a whole. Each layer was reviewed alone; the merge
  changed code under the in-place allow-list and was only tested, not
  re-reviewed.
- The in-place allow-list over time. It is sound today because 33 specific
  functions never allocate, throw or run JavaScript. Nothing enforces that
  when the provider changes. It needs a debug-build guard before it ships.
- The name cache. Its win depends on a hit rate nobody measured in Edge.js,
  and it loses on a miss.
- `HotEnv`. It depends on Wasmer behaviour that is implemented but
  documented as unsupported, and it came with an unexplained 1.2 ns
  slowdown of `g()`.
- Thread-stack mode outside macOS arm64. The design argument is good (V8
  wants one contiguous stack, and this gives it one), but the entry is
  hand-written assembly with a setjmp-style trap recovery, and Linux is
  where it matters.

**What surprised me**

- The stack switch was not the main cost. On a trivial import Wasmer was
  3.5 of 17 ns; the Rust body, the bridge scope and thread-local access
  were the rest. Most of the 3x comes from N-API, not Wasmer.
- Thread-local access on macOS (`_tlv_get_addr`) shows up in every layer.
  Linux TLS is cheaper, so the macOS numbers probably overstate both the
  cost and the gain there.
- The unsound upper bound is only 2 to 4 ns below the sound build. Further
  gains must come from doing less work per call (fewer imports, fewer V8
  API calls), not from removing checks.
- After the changes V8 itself is the largest part of every case that runs
  JavaScript. `Object::Get` and `Function::Call` are about half of the
  named get and of `napi_call_function`.

**What I would do next**

1. Land the N-API-only changes as a few small PRs on top of #174, each with
   its own timing. They need no Wasmer change and carry most of the gain in
   default mode.
2. Count imports per request on a real framework request before tuning
   further. The six micro cases were chosen by me, not by profile; string
   creation, `napi_get_property` with a key value, wrap and unwrap and
   references may matter more.
3. Remove `napi_get_cb_info` as an import by writing the callback record
   into guest memory. It is the single largest remaining item in a callback
   with arguments (about 21 of 67 ns).
4. Fix the two-coroutine nesting cliff in Wasmer properly. It is a 3x cost
   on nested callbacks and independent of everything else.
5. Treat thread-stack mode as its own Wasmer project, Linux first.

## Building this branch

- This branch does not build against the Wasmer revision pinned in
  `Cargo.standalone.toml`. It needs Wasmer PR 7198 at `afa25e62` with
  `wasmer-pr7198-afa25e62.patch` (in this folder) applied with `patch -p1`.
- Point the manifest's Wasmer crates at that checkout with a
  `[patch."https://github.com/wasmerio/wasmer.git"]` section listing each
  crate by path.
- Timings: `tests/perf_calls.rs` and the `perf_calls_*` and `perf_hostcall`
  files, release, `-- --ignored --nocapture`, with `PERF_THREADS=1
  PERF_ROUNDS=7`.
- Thread-stack mode: set `WASMER_WASM_ON_THREAD_STACK=1` (aarch64 Apple
  only).
