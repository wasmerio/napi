# Realms and envs

## The model

- **An env serves one engine instance and all its realms.** Its own realm is
  the engine instance's main realm. `unofficial_napi_create_realm` makes more
  (vm contexts, or a browser's documents) and returns an
  `unofficial_napi_realm`. A realm is not an env.
- **N-API acts in one realm at a time, the current realm**, as an ECMAScript
  engine runs code in the realm of its running execution context:
  - what N-API creates comes from the current realm, with its intrinsics, and
    `napi_get_global` returns its global. Errors, typed arrays and DataViews
    never come from the realm's global, which its scripts control. A buffer
    is the current realm's `Uint8Array` with the env realm's
    `Buffer.prototype`, since `Buffer` belongs to the env;
  - a native function belongs to the realm current when it was made (its
    `[[Realm]]`, as for an ECMAScript built-in function) and its callback
    runs in that realm, whichever realm calls it;
  - `unofficial_napi_enter_realm` makes a realm current until
    `unofficial_napi_leave_realm`;
  - otherwise the current realm is the env's own.
- **An embedder that never enters a realm acts only in the env's realm.**
  Every native function it makes belongs to the env's realm, so it runs there
  even when a vm context calls it. That is Node's model, and EdgeJS's.
- **The calls that run code take their realm explicitly:**
  `unofficial_napi_run_script`, `unofficial_napi_compile_function`, module
  creation and `unofficial_napi_bytecode_open`. A NULL realm there means the
  env's own, also while another realm is current.
- **Hooks, instance data and cleanup hooks stay on the env.** A hook fires
  for code running in any of its realms, as Node's hooks do for vm contexts.
  Hooks, finalizers, cleanup hooks and interrupt callbacks run in the env's
  realm.
- **Realms are collected.** A realm lives while its global (its sandbox, when
  it has one) or anything made in it is reachable. A realm handle is valid
  only while the handle scope it was obtained in is open, like a
  `napi_value`, and two handles for one realm may differ;
  `unofficial_napi_get_realm` finds the realm again from its global.
- **Don't keep realm handles.** Using one after its handle scope has closed
  is undefined behavior that the provider may not detect: once the realm is
  collected, a newer realm can get the same handle. An embedder that uses a
  realm later (a browser, per document) keeps its global in a `napi_ref` and
  calls `get_realm` on it each time.

## API

```c
typedef struct unofficial_napi_realm__* unofficial_napi_realm;

napi_status unofficial_napi_create_realm(napi_env env,
                                         const unofficial_napi_realm_options* options_or_null,
                                         unofficial_napi_realm* realm_out,
                                         napi_value* global_out);
napi_status unofficial_napi_get_realm(napi_env env, napi_value global_or_sandbox,
                                      unofficial_napi_realm* realm_out);

typedef uint64_t unofficial_napi_realm_scope;
napi_status unofficial_napi_enter_realm(napi_env env, unofficial_napi_realm realm_or_null,
                                        unofficial_napi_realm_scope* scope_out);
napi_status unofficial_napi_leave_realm(napi_env env, unofficial_napi_realm_scope scope);

napi_status unofficial_napi_run_script(napi_env env, unofficial_napi_realm realm_or_null, …);
napi_status unofficial_napi_compile_function(napi_env env, unofficial_napi_realm realm_or_null, …);
// unofficial_napi_module_create_options.realm_or_null      (options version 2)
// unofficial_napi_bytecode_open_options.realm_or_null      (options version 2)
```

- **`create_realm`:**
  - With a sandbox, global property access in the realm reads and writes the
    sandbox, as with `vm.createContext(sandbox)`.
  - A sandbox belongs to at most one realm. The env's own global may still be
    one.
  - Options: sandbox, name, origin, host-defined option id, the two
    code-generation flags and an own microtask queue. Each provider honors
    only some of them, and accepts and ignores the rest:

    | Option | V8 | QuickJS | host-JS |
    |---|---|---|---|
    | `sandbox_or_null` | yes | yes | yes |
    | `host_defined_option_id` | yes | yes | no |
    | `allow_code_gen_strings` | yes | no | no |
    | `own_microtask_queue` | yes | no | no |
    | `allow_code_gen_wasm` | no | no | no |
    | `name`, `origin` | no | no | no |

    With `allow_code_gen_strings` false, V8 makes `eval` and the `Function`
    constructors throw an `EvalError` in the realm. WebAssembly code
    generation stays allowed on V8 (QuickJS has no WebAssembly).
- **`get_realm`** returns NULL for any object that isn't a realm's global or
  sandbox, the env's own global included.
- **`enter_realm` / `leave_realm`:**
  - Scopes nest and close in LIFO order. A token is nonzero and never reused
    within its env. Leaving any token but the innermost scope's fails with
    `napi_invalid_arg` and changes nothing.
  - A native callback's frame sits on the same stack, so a callback can't
    leave a scope its caller opened, and scopes it leaves open close when it
    returns. Releasing the env closes every scope.
  - A scope keeps its realm alive until it is left. Entering a realm of
    another env fails with `napi_invalid_arg`.
  - A handle used after its handle scope has closed is undefined behavior.
    V8 and QuickJS only check that the handle is one of the env's realm
    records before reading it, so entering it does not crash. But the next
    `create_realm` frees the records of collected realms and can reuse one,
    and the stale handle then enters that newer realm.
  - No JavaScript runs, so both work while execution is terminating. A
    pending exception stays pending.
  - NULL enters the env's own realm, for example to reach its global from
    inside another realm.
- **Promises:** a promise from `napi_create_promise` belongs to the current
  realm, and `napi_resolve_deferred` and `napi_reject_deferred` settle it
  there.
- **Threadsafe functions and async work** are the embedder's, not napi's. To
  run their callbacks in the realm that made them, the embedder keeps a
  reference to the global `napi_get_global` returns when they are made, and
  enters `get_realm` of that global around each callback.
- **Changed from `main`:** the realm API replaces the contextify calls, the
  module options' `context_or_undefined` and the context-token hooks. They
  still work, deprecated, on top of realms: see
  [Deprecated compatibility API](#deprecated-compatibility-api).
- **Guest ABI:** a realm id is the value id of the realm's global or sandbox,
  and 0 is the env's realm. The bridge keeps no realm table: it resolves the
  id again with `get_realm` on each call, so the id lives exactly as long as
  its value id. The scope imports are `unofficial_napi_enter_realm(env,
  realm_id, scope_out_ptr) -> status` and `unofficial_napi_leave_realm(env,
  scope: i64) -> status`; the token is the provider's, passed through as a
  u64.

## Deprecated compatibility API

Code written against `main`, EdgeJS's included, builds and runs unchanged,
from source or as a binary. The API it uses is marked deprecated in
`unofficial_napi.h` (`UNOFFICIAL_NAPI_DEPRECATED`; define
`UNOFFICIAL_NAPI_NO_DEPRECATION_WARNINGS` to silence it) and is removed in a
future major release. It is a thin layer over realms, the same for V8 and
QuickJS (`lib/src/napi_compat.cc`), so contextified objects and realms mix:
`get_realm` finds the realm of a contextified object, and the deprecated
calls take a realm's global or sandbox.

| Deprecated | Use instead | How it maps |
|---|---|---|
| `unofficial_napi_contextify_make_context` | `create_realm` | An object or function sandbox becomes `sandbox_or_null`; a symbol (`vm.constants.DONT_CONTEXTIFY`) makes a fresh global. The other arguments become the realm options of the same names. Returns the realm's global: the sandbox, or the fresh global. |
| `unofficial_napi_contextify_run_script` | `run_script` | The sandbox names the realm, as below. |
| `unofficial_napi_contextify_compile_function` | `compile_function` | The parsing context names the realm, as below. |
| Bytecode-open options version 1 | Version 2, `realm_or_null` | Compiles for the current realm, as nothing means below. |
| Module-create options version 1, and `context_or_undefined` in version 2 | Version 2, `realm_or_null` | The context names the realm, as below. |
| The env hooks' `context_token_assign_callback` and `context_token_unassign_callback` | Per-realm state keyed by the realm's global | Tokens on realms, as below. |

- **A context argument** (sandbox, parsing context, `context_or_undefined`)
  is a contextified object, naming its realm, the env's own global, naming
  the env's realm, or nothing. Anything else fails with `napi_invalid_arg`.
  The env's own global can also be a realm's sandbox
  (`vm.createContext(globalThis)`). It is then a contextified object and
  names that realm, as on `main`'s V8.
- **Nothing** (NULL, `undefined` or `null`) means what the env's context
  meant on `main`: the current realm. That is the env's own realm unless the
  embedder enters a realm or a native function of another realm is running.
  Version 2 of the module options is the exception: there nothing means the
  env's realm, as a NULL `realm_or_null` does. The realm API's NULL is
  always the env's realm.
  - The current realm is found from the global `napi_get_global` returns,
    and the env's own global means the env's realm even when it is a realm's
    sandbox. So while the realm whose sandbox is the env's own global is
    current, nothing means that realm on V8, but the env's realm on QuickJS
    and with the host-JS guest imports: there `napi_get_global` returns the
    sandbox, the env's own global. That holds for version 1 of the
    bytecode-open and module-create options too.
- **Structs grow at the end.** Version 1 of the bytecode-open and
  module-create options keeps `main`'s layout, and version 2 appends
  `realm_or_null`. A provider reads a version 1 struct only up to
  `UNOFFICIAL_NAPI_*_V1_SIZE`. A version 2 module struct sets
  `realm_or_null` or the deprecated `context_or_undefined`, not both
  (`napi_invalid_arg`). The env hooks are version 1 again, `main`'s layout.
- **Context tokens.** V8 accepts the token callbacks, as on `main`. A realm's
  token is its record, which outlives the assignment. V8 assigns it when
  `create_realm` (and so `make_context`) makes the realm, and unassigns it
  exactly once, before the env is freed: when it frees the record of the
  collected realm (in the next `create_realm` or in `collect_garbage`), or,
  for a realm still alive, during env release. The release unassigns the
  tokens of the realms alive when it begins before the cleanup hooks run,
  and those of the realms made while the hooks run (by a hook, or by a
  finalizer that runs inside one) after them, before the release's
  finalizers run, as `main` did (its contexts' own cleanup hook unassigned
  them). A realm one of the release's finalizers (the instance-data
  finalizer too) makes is pending until the release's next pass unassigns
  its token, so the finalizers after it in its pass still see the token
  assigned, unless the realm is collected and its record freed first. One
  an unassign callback makes while a release sweep runs it is unassigned in
  the same sweep; one made by an unassign callback that `create_realm` or
  `collect_garbage` runs (for a collected realm, from a hook or finalizer of
  the release) waits, as any realm made while releasing, for the first
  sweep after it. What else an unassign callback makes is released as the
  rest (see `unofficial_napi_release_env` in the header). Realms made
  before the hooks were attached get no token. Both hooks run in the env's
  realm; at release what they throw is dropped, as for the other hooks and
  finalizers. On `main` a context lived until release, so tokens were only
  unassigned then. QuickJS does not accept the hooks and never calls them,
  as on `main`.
- **Statuses are `main`'s V8 ones.** `main`'s QuickJS differed: it took a
  NULL `name`, refused a symbol sandbox, returned an already contextified
  sandbox again, compiled in the env's context for a parsing context that
  was not contextified, and ignored `context_or_undefined`. It now behaves
  as V8 does. So do the host-JS guest imports, which are stricter than
  `main`'s host-JS was: it ignored `name`, made a fresh object for sandbox
  id 0 or an id that named no value, and ran with no sandbox for such an
  id. They now return `main`'s V8 and native-bridge statuses
  (`napi_invalid_arg`).
- **Zero-initialize the option structs** (`= {0}` in C, `{}` in C++)
  before setting their fields. Source written against `main` that sets
  `size` to `sizeof`, `version` to the version macro and only the fields it
  knew sends version 2 with an indeterminate `realm_or_null`.
- **Guest ABI.** The wasm32 layouts of version 1 are `main`'s, and version 2
  appends a u32 realm id. The guest imports
  `unofficial_napi_contextify_make_context`, `…_run_script` and
  `…_compile_function` keep `main`'s names and signatures, on the bridge's
  realm calls. A sandbox id is the value id of a realm's global or sandbox,
  as on `main`, and 0 is the current realm. With a host-JS native Worker
  realm (#125), `make_context` fails as `create_realm` does, and
  `run_script` with id 0 goes to `evaluate()`.
  - One layer (`src/guest/compat.rs`) maps a context id to a realm id for
    both guest backends, as `lib/src/napi_compat.cc` does natively. It
    makes the ids of its lookups (the current global, the env's global) in
    a handle scope of its own, as the native layer makes its values, so a
    deprecated call leaves in the guest's current handle scope only what
    the realm call it stands for leaves there. The one exception: with a
    realm other than the env's current, the call also needs an id of the
    current global, which names that realm, outside that scope. The WASIX
    native bridge stores a new one on each call. The host-JS backend gives
    an object the id it already has while that id is live, so it stores at
    most one: none while the guest holds an id of the realm's global, else
    one, which later calls reuse while it lives. A guest built against
    `main` can be in that case only inside a callback, whose scope frees
    the id when it returns.
  - Guest env hooks are version 1, `main`'s 32 bytes. As on `main`, the
    bridge gives the provider none of them, so a guest's token callbacks
    are reported as not accepted and never called.
  - `tests/programs/run_script_compat_test.c` is `main`'s
    `run_script_test.c`, run natively and as a WASIX guest, with version 1
    module and bytecode options and checks of the current realm. The WAT
    guest of the concurrency tests calls `…_contextify_run_script` and
    sends version 1 module options, and a host-JS test drives the three
    imports from a guest.

## How the current realm works

Each env keeps a stack of realm frames, innermost last: the scopes
`enter_realm` opened and the native callbacks that are running. The innermost
frame is the current realm; an empty stack means the env's own. A callback
pushes its function's realm on entry and pops back to where it started on
return, closing the scopes it left open. Finalizers, cleanup hooks, env hooks
and interrupt callbacks push the env's realm.

| Provider | Current realm | A native function's realm | Keeping the realm alive |
|---|---|---|---|
| V8 | `env->context()` returns the innermost frame's context. A scope also enters the context (`v8::Context::Enter`), so what V8 makes in the isolate's current context is made there too. | The function is made in the realm's context. V8 already makes a function's creation context current while it runs; the callback payload names the realm for N-API's frame. | A scope holds its context in a strong handle until it is left. A function holds its creation context, as any V8 function does. |
| QuickJS | `env->context()` returns the innermost frame's `JSContext`. | QuickJS calls C functions with data in the caller's context: it doesn't switch realms for them as it does for its own built-ins. A function of another realm carries that realm's context and anchor (a function of the realm) in its data, and its trampoline pushes them. | A scope owns a reference to the realm's anchor. A function's data holds it, so the collector sees the edge and frees the realm with its last function. |
| host-JS | The env's stack of virtual contexts. | The JS wrapper of a callback holds its context and passes it to the Rust dispatch. | The wrapper, not the Rust closure the env keeps, holds the context, so the realm goes with the function. |

## How each provider does it

| Provider | A realm is | `get_realm` | Collection |
|---|---|---|---|
| V8 | a record with a weak `v8::Global<v8::Context>` and its own microtask queue, owned by the env | creation context of the global, then context embedder-data slot 33 | The context is weak. The next `create_realm` frees records whose context is gone, and only then their microtask queues (from #109). |
| QuickJS | a record with a `JSContext` on the env's runtime; a sandbox realm copies the sandbox's properties in and out around each script | a map from global or sandbox to record | The key (the sandbox, or the realm's global) holds a function of the realm under a private symbol, and a sandbox realm's global holds the sandbox, so the collector frees the cycle with its key. The record holds the key weakly, and the next `create_realm` frees collected records (from #110). |
| host-JS | a virtual global context (a `with`/Proxy scope over one host realm) | a per-env `WeakMap` from global or sandbox to context | Nothing holds a realm strongly; the host GC collects it with its key. |

- **host-JS with a native Worker realm (#125).** The env's realm is the
  Worker's own global, and it serves that one env. `create_realm` fails with
  a pending TypeError, so `get_realm` finds nothing and NULL is the only realm
  to run code in. `unofficial_napi_run_script` goes to the bridge's
  `evaluate()`, which keeps the filename and offsets, and `napi_run_script` to
  its `binding()`.
- **Threads.** A realm call touches only its env's state and runs on the
  thread that owns the env (#107). The bridge takes no global lock for it.

## How EdgeJS uses it

- `vm.createContext` → `create_realm`.
- `runInContext`, `compileFunction`, `moveMessagePortToContext` and
  `ModuleWrap` → `get_realm` on the contextified object, then the call with
  that realm.
- Everything else, hooks included, goes through the env.

## Not done

- **QuickJS needs engine commit 870aa7b** (wasmerio/quickjs#10), which makes
  a pending job hold its context. Without it, a job queued in a realm that is
  then collected runs on a freed context. With it, a realm's pending jobs keep
  the realm alive until they run.
- **ShadowRealm.** A realm with its own bindings and hooks, and no fallback
  to the main realm, would be a separate concept, closer to Node's `Realm`.
- **host-JS realms have no intrinsics of their own.** A Worker can't create a
  JavaScript realm, so objects and errors made there come from the host
  realm. Entering a realm selects its global, and functions keep their
  realm.
- **The realm of a value** (`get_value_realm`). ECMAScript defines a realm
  only for functions (`GetFunctionRealm`), and QuickJS and host-JS record
  none for other objects. A callback already knows the realm it runs in:
  `napi_get_global` returns that realm's global.
- **Per-realm embedder data.** An embedder keeps its own map from a realm's
  global to its state.
- **Sandboxes work differently per engine.**
  - V8 turns the sandbox itself into the realm's global, which goes against
    V8's API contract and breaks `vm.createContext(globalThis)`.
  - QuickJS copies the sandbox's properties into the realm's global before
    each script and back after a script that completes:
    `unofficial_napi_run_script`, and `napi_run_script` while the realm is
    current. `compile_function` only copies them in. In between, the two can
    differ. Native code that runs during a script sees the sandbox as of the
    last copy, and a script it starts in the realm copies the sandbox in
    again, over the running script's global writes to the same names.
  - host-JS evaluates against the sandbox with `with`.
- **Code generation per realm.** No provider enforces
  `allow_code_gen_wasm`. V8 would need an isolate-wide
  `AllowWasmCodeGenerationCallback` that reads a per-realm flag, as Node
  does. There is also no call to change a realm's code-generation policy
  after it is created, which a browser needs because CSP arrives with the
  response.

## History

- **#111** moved what is shared by an isolate or runtime into an agent,
  apart from the env.
- **#112 and #114** made every vm context a full env, with fallback to its
  parent env for hooks and state and shims for older guests.
- **#115** made an env a realm that owns nothing, with all state on the
  engine instance.
- **#116** made vm contexts realm handles. Both earlier designs kept an env
  per vm context, so native code had to know which env it held, and every
  hook, scope and reference had to work across envs. With realm handles,
  only the four calls that run code take a realm, and the rest of N-API
  keeps a single env per engine instance. Native code acted only in the
  env's realm.
- **Entering realms** followed WebkitWasm's feedback on #116: a browser runs
  one realm per document, and its bindings must create objects in the
  document's realm and run there. Native code now acts in a current realm,
  with ECMAScript's rules.
- **The deprecated API** came back on top of realms, with its structs grown
  at the end instead of changed in place, so that code built against `main`
  keeps working until a major release removes it.
