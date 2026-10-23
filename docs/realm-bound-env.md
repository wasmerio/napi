# Realm-bound `napi_env`

Status: Phase 0 implemented (see §11). Phases 1–4 are not started.

**Proposal:** split each provider into an **agent**, which is the engine
instance and its thread, and **realm envs**. A realm env is a `napi_env` whose
realm is fixed when the env is created and never changes. Values and handle
scopes belong to the agent, so they work with every env of that agent. Host
hooks belong to the realm. An env lives exactly as long as its realm.

What gets simpler:

- **API.** No call takes a sandbox or context argument again.
  `contextify_make_context` becomes `create_realm`.
- **Hooks.** The context-token hooks go away.
- **Branch not needed.** The `contextify_enter`/`leave` work on
  `feat/contextify-enter-leave` is no longer needed.
- **QuickJS.** The provider stops switching a "current context".
- **host-JS.** The provider drops its fallback scopes and its active-context
  switch.

---

## 1. First principles: what an N-API call needs to know

Every call needs to know four things:

| # | What | Examples |
|---|---|---|
| A | **Agent**: which engine instance and thread. | Every call: handle allocation, throwing, GC. |
| R | **Realm**: which global object and intrinsics. | `create_object`, `create_error`, `create_function`, `create_promise`, `create_typedarray`, `get_global`, `run_script`, compiling, modules, and dispatching host hooks. |
| M | **Module instance**: which addon. | Instance data, cleanup hooks, the env passed to finalizers, API version. |
| T | **Per-call state** | Last error, pending exception, open handle scopes. |

Today `napi_env` bundles A, M and T with a *default* R. Each layer then
overrides R per call in its own way:

| Where | How the realm is overridden today |
|---|---|
| V8 | A `sandbox_or_null`, `parsing_context_or_undefined` or `context_or_undefined` argument, resolved by a linear scan of `g_context_records`. In about 9 places the provider uses the implicit current context instead of `env->context()`. |
| V8, `feat/contextify-enter-leave` | A selectable context stack (`context_scope`, `root_context()`) and LIFO tokens. |
| QuickJS | A mutable `current_context_`, switched at 23 `napi_env_context_scope__` sites. Every `napi_value__` carries its own `context_` tag. |
| host-JS | The module-global `wasmerNapiActiveGlobalContext`, switched per thread. vm sandboxes fall back to an anonymous global that belongs to no env. |

**Principle:** R becomes an immutable property of the env. An env is a
capability for exactly one realm: everything done through env X happens in
realm X. No call ever has a second source of realm information.

The rest of this document follows from that.

## 2. What the code does today

### V8 (`v8/src`)

- **One env, one isolate, one context.** `unofficial_napi_create_env` creates a
  fresh isolate, allocator and `v8::Context`, then calls
  `unofficial_napi_create_env_from_context` (`unofficial_napi.cc:1785-1797,
  1902-2003`).
- **The owner is the agent.** `unofficial_napi_env_owner` is the
  `UnofficialEnvScope`, which holds the isolate, the allocator, and
  permanently open isolate, handle and context scopes.
- **vm contexts are records inside the one env.** Each `ContextRecord` (key
  ref, context, own microtask queue) is pushed onto `g_context_records[env]`.
  Lookups scan the list with `napi_strict_equals`
  (`unofficial_napi_contextify.cc:848-861, 1333-1346`).
- **`module_wrap_create` resolves its context differently** from `run_script`
  and `compile_function`. It uses `GetCreationContextChecked()` on any object
  instead of looking up a contextify record.
- **Isolate-level state is spread over global maps** keyed by isolate or env:
  `g_env_by_isolate` (last writer wins), `g_promise_hooks`,
  `g_promise_reject_callbacks`, `g_prepare_stack_trace_callbacks`,
  `g_profiler_states`, `g_module_wrap_states`, and the platform runner's single
  `target_env_`.
- **The `Private::ForApi` keys are isolate-wide**, even though they are stored
  on the env.
- **Inconsistent context use.** Places that mix the implicit current context
  with `env->context()`:
  - the `run_script` timeout error;
  - `ImportModuleDynamicallyWithPhase`;
  - `HostInitializeImportMetaObject`;
  - `SyntheticModuleEvaluationSteps`;
  - `PromiseHookCallback` and `PromiseRejectCallback`;
  - `GetThrownAtString`;
  - the serdes constructors;
  - `make_context` called from a vm context.
- **Lifetimes.** Function payloads, external wrappers and serdes contexts live
  in the env's slab and are freed only with the env.

### QuickJS (`quickjs/src`)

- **One env, one runtime, one main context.** The owner holds the
  `JSRuntime` and the main `JSContext`.
- **Runtime hooks point at a single env.** The promise hook, rejection tracker
  and module loader hooks are installed per runtime with the env as opaque, so
  a second env would overwrite them.
- **Class ids are process-static.**
- **Native callbacks run in the caller's realm.** QuickJS does not switch
  realms for C-function-data calls (`quickjs.c:6287`, a TODO). The trampoline
  takes `JS_GetContextOpaque(caller_ctx)` and switches `current_context_` to
  it (`napi_function.cc:67-77`). An addon function made in main and called
  from a vm script sees the vm global. That differs from V8 and Node.
- **`module_wrap` ignores `context_or_undefined`.** Every module lives in main.
- **`compile_function` silently falls back to main** when the parsing context
  is unknown.
- **vm contexts are never freed individually.** `dispose_context` is
  unreachable; `fix/quickjs-contextify-collect` addresses this.
- **`last_exception_context_` is redundant.** QuickJS exceptions are
  runtime-level.

### host-JS (`src/snapi_js.rs`)

- **One shared host realm.** Each env gets a virtual global: a `Proxy` over a
  descriptor snapshot of `globalThis`.
- **vm sandboxes are emulated.** They are evaluated with `with (sandbox)`.
  Names they don't define fall back to an anonymous global context, cached in
  the module-global `wasmerNapiGlobalScopes` WeakMap and owned by no env.
- **`compile_function` with a parsing context uses the raw sandbox**, so
  missing names fall through to the real host global.
- **Env behaviour depends on a module-global active-context pointer**, switched
  in `env_mut`/`activate()` (`snapi_js.rs:1712-1762`).

### Bridge and Rust (`src/`)

- **The guest's `napi_env` is a u32 env id**, resolved on every import. The
  "scope id" is the guest's handle to the `env_owner`, i.e. the teardown
  capability.
- **`SnapiEnvState` (`napi_bridge_init.cc:119-172`) holds isolate-level
  state:** value slots, scope frames, refs, deferreds, handle tables and
  `active_callback_ctx`.
- **The budget treats env and isolate as the same:** `reserve_isolate` and
  `commit_isolate`, `env_heap_charges[env_id]`, and the `env_registry` used by
  `terminate_all`.
- **Callbacks capture `guest_env` at registration.** They always receive the
  env that created them.
- **`attach_env` hooks are inert on native.** The bridge passes empty hooks.

### EdgeJS (consumer)

- **One env per isolate.** The env is both the principal realm and the
  `edge::Environment` key (`g_environments`).
- **vm contexts are passed as sandbox `napi_value`s** together with the
  principal env.
- **Hooks are set once, on the principal env:** dynamic import,
  `import.meta`, promise reject, promise hooks and prepare-stack-trace. vm
  modules depend on the provider routing them back to that env.
- **EdgeJS never asks for "the current env"**, never compares envs, and never
  calls `napi_set_instance_data`.
- **Bindings rely on callbacks receiving their creating env:**
  `napi_get_global(env)` → `process`/`internalBinding`, 86 call sites.
- **Context tokens are recorded in `attached_contexts_` but never read.**
- **Native addons share Edge's env** (`edge_process.cc:4477`), so their
  instance data collides.

### Node (reference)

Node's `napi_env__` is already realm-bound: `isolate` plus `context_persistent`,
created per (addon, context). `js_native_api_v8.cc` uses `env->context()`
everywhere and never the current context.

Node has three layers:

- **`Environment`:** thread, loop and cleanup.
- **`Realm`:** principal or ShadowRealm. It holds the per-realm callbacks:
  `prepare_stack_trace_callback`, `promise_reject_callback`,
  `promise_hook_handler`, and `host_import_module_dynamically_callback` /
  `host_initialize_import_meta_object_callback` (`env_properties.h:474-536`).
- **Plain contexts.** A vm context is assigned to the Environment with
  `realm = nullptr` (`node_contextify.cc:330`). Its hooks fall back to the
  principal realm: "Fallback to the principal realm if it's in a vm context"
  (`module_wrap.cc:998-1001`).

## 3. The object model

```
Platform (process)      configure_runtime, V8 worker threads
└─ Agent                isolate / JSRuntime / host worker; one thread at a time
   │                    the owner handle (unofficial_napi_env_owner) names it
   ├─ values and handle scopes, pending exception, finalizer queue
   ├─ default microtask queue, GC, heap limits and budget, near-heap-limit
   ├─ terminate, interrupt, profiler, snapshots, hash seed, datetime
   ├─ embedder hooks: foreground task, fatal, OOM
   ├─ Private keys, class ids, context-free compiled code (UnboundScript)
   └─ Realm env (napi_env) × N: one per ECMAScript realm
      │                 principal, vm context, ShadowRealm, frame...
      ├─ context (immutable), global, cached intrinsics
      │                 (Buffer.prototype, typed array constructors)
      ├─ parent env; host hooks fall back to the parent's when unset
      │                 promise reject, promise hooks, prepare stack trace,
      │                 dynamic import, import.meta, source maps
      ├─ module records, sandbox link, own microtask queue, codegen policy
      └─ addon state    API version, instance data, cleanup hooks,
                        refs/wraps/finalizers, payload slabs, last error
```

### Invariants

1. **An env's realm never changes.** No context arguments, no selection, no
   tokens.
2. **Everything an env creates lives in its realm.**
   - Objects, functions and errors made through env X are made in realm X.
     When the current context differs, the provider enters X for the call;
     otherwise it adds no work.
   - A native callback receives the env that created it. V8 already enters the
     function's creation context. QuickJS must take the env from the
     function's data rather than from the caller's context.
3. **Values belong to the agent.** A `napi_value` (or a ref) from any env of
   an agent is valid with every other env of the same agent, within its
   handle scope.
   - V8 gets this for free.
   - QuickJS, the bridge and host-JS must move their value and scope tables
     from the env to the agent.
   - EdgeJS already relies on it: `run_script` results, `compileFunction`
     with a parsing context, and `moveMessagePortToContext`.
4. **Host hooks belong to the realm, with a fallback to the parent.**
   - An event is dispatched to the env of the realm where it happened. That
     env's hook runs if set; otherwise the provider walks up the parent chain.
   - This is Node's "vm context → principal realm" fallback, made explicit.
   - A ShadowRealm or frame sets its own hooks.
5. **An env lives exactly as long as its realm.** See §4.5.

### Addon instances

Node gives each addon its own env per realm. The model allows that later as a
sibling env on the same realm that shares the realm state and has its own
addon state. That would fix the instance-data collision from Edge's
`process.dlopen`. This proposal starts with **one env per realm**, but keeps
the realm part of the provider struct separate from the addon part so that
per-addon envs need no ABI change later.

## 4. API changes

### 4.1 New

```c
enum { UNOFFICIAL_NAPI_REALM_OPTIONS_VERSION = 1 };

typedef struct {
  uint32_t size;
  uint32_t version;
  napi_value sandbox_or_null;       // vm semantics: global property access goes to this object
  napi_value name;                  // or undefined
  napi_value origin;                // or undefined
  napi_value host_defined_option_id;
  bool allow_code_gen_strings;
  bool allow_code_gen_wasm;
  bool own_microtask_queue;
} unofficial_napi_realm_options;

// Creates a realm in env's agent. The new env falls back to env's host hooks
// until it sets its own. global_out receives the realm's global, or the
// sandbox when one is given.
napi_status unofficial_napi_create_realm(
    napi_env parent,
    const unofficial_napi_realm_options* options,
    napi_env* realm_out,
    napi_value* global_out);

// Returns the env of the realm that owns `value`. `value` may be a realm
// global, a vm sandbox, or a function; V8 also accepts any object (by its
// creation context). The result is NULL when that realm has no env.
napi_status unofficial_napi_get_realm(
    napi_env env, napi_value value, napi_env* realm_out);
```

`get_realm` replaces every sandbox-to-context lookup, including the linear
scans in V8. It is O(1):

- V8: a context embedder-data slot.
- QuickJS: `JS_GetContextOpaque`.
- host-JS: a WeakMap.

### 4.2 Lose their context argument

| Today | After |
|---|---|
| `contextify_run_script(env, sandbox_or_null, source, …)` | `run_script(env, source, …)`, run in env's realm |
| `contextify_compile_function(env, …, parsing_context_or_undefined, …)` | Parameter removed; env is the parsing realm |
| `module_wrap_create`: `options.context_or_undefined` | Field removed (options v2); the module lives in env's realm |

Once these calls no longer take a context, the `contextify_` prefix no longer
describes them. Consider renaming them to `unofficial_napi_run_script` and
`unofficial_napi_compile_function`.

### 4.3 Removed

- **`unofficial_napi_contextify_make_context`** → `create_realm`.
- **`context_token_assign` / `context_token_unassign` hooks.** The embedder
  creates realms itself, and a realm's death runs that env's cleanup hooks.
  Edge's `attached_contexts_` is already dead code.
- **`unofficial_napi_contextify_enter` / `leave`** (unmerged branch). They
  are never needed: use the realm's env.

### 4.4 Re-scoped, same signature

**Per realm, with fallback to the parent:**

- `set_promise_reject_callback`
  - V8 dispatches by `promise->GetCreationContext()`.
  - QuickJS dispatches by the tracker's `ctx`.
- `set_promise_hooks`
  - V8's `Context::SetPromiseHooks` is already per context.
  - This removes `NapiV8ApplyPromiseHooksToContextifyContexts`.
- `set_prepare_stack_trace_callback`
- `module_wrap_set_hooks`
- `configure_source_maps`
- `enqueue_microtask`: uses the realm's own queue if it has one, otherwise the
  agent's.

**Per agent; any env of the agent selects it:**

- `collect_garbage`
- heap, space and code statistics
- `profile_*`, `take_heap_snapshot`
- `terminate_execution`, `cancel_terminate_execution`, `request_interrupt`
- `configure_near_heap_limit_callback`
- `get_hash_seed`
- `notify_datetime_configuration_change`
- continuation-preserved embedder data
- `create_private_symbol`
- `event_loop_checkpoint`
- bytecode handles: compiled code is not tied to a context (V8
  `UnboundScript`; QuickJS bytecode belongs to the runtime), so these become
  usable by any realm of the agent instead of only "the creating
  environment".

**`attach_env`:** agent-level. It is accepted once, on the principal env, and
refused on any other env.

**Optional: thread-safe agent handle.** Terminate and interrupt could take the
owner handle instead of an env when called from a foreign thread. That would
remove the "env used from another thread" special case that
`feat/concurrent-stores` has to carve out.

### 4.5 Realm lifetime

**The principal env** lives until `release_env(owner)`.

**A created realm env** lives while its realm is reachable: its global or
sandbox, or anything made in it.

- This is the lifetime model that `fix/contextify-weak-records` (V8) and
  `fix/quickjs-contextify-collect` (QuickJS) already implement for vm
  records.
- When the realm is collected, the provider does what Node does when an env
  goes away before its isolate:
  1. runs the env's cleanup hooks;
  2. finalizes its refs, wraps and finalizers;
  3. neutralizes surviving externals (`hint->env = nullptr`);
  4. tells the bridge to drop the guest id.
- Env ids are never reused, so a stale id fails with `napi_invalid_arg`.

**Why this is safe by construction.** Payloads, externals and wrappers created
through an env belong to objects in that env's realm. In V8, a function, and in
practice any object, keeps its native context alive. So the slabs cannot be
reached once the realm is gone.

Two cases need explicit handling:

- **Foreign objects.** Records an env attached to *another* realm's objects
  (`napi_wrap`, `add_finalizer`, `create_reference` on a foreign object) are
  finalized when the env is torn down.
- **QuickJS plain objects.** They don't keep their `JSContext` alive, so
  externals must be neutralized at teardown.

**Explicit detach (for example a frame being removed)** is a possible later
addition. It would need tombstoned payloads, because functions of a detached
realm can still be reachable.

## 5. Changes inside each layer

### V8

- **Agent.** `UnofficialEnvScope` grows into `napi_agent__` and absorbs:
  - the isolate-keyed maps: hash seed, promise reject, promise hooks,
    fatal/OOM, near-heap-limit, profiler;
  - the principal env (replacing `g_env_by_isolate`);
  - the foreground target;
  - the `Private` keys;
  - the isolate-level V8 callbacks (prepare stack trace, dynamic import,
    import.meta, promise reject), as dispatchers that resolve
    context → env → hook.
- **Context → env** goes through an embedder-data slot. It replaces
  `FindRecordByKey`, `ResolveContextFromKey` and `GetModuleWrapEnvForIsolate`.
- **`napi_env__`** keeps the immutable context, the parent, per-realm hooks,
  module records, the own microtask queue and the sandbox key.
- **Entering the realm.** The prologue of each N-API call that creates objects
  enters `env->context()` only when it differs from the current context. That
  keeps the same-realm path free; the enter/leave branch measured
  `Context::Enter`/`Exit` at about 3 ns.
- **Deleted:**
  - `g_context_records` and its scans;
  - `NapiV8ApplyPromiseHooksToContextifyContexts`;
  - the per-context prepare-stack-trace table;
  - the context-token plumbing.

### QuickJS

- **Agent.** `napi_agent__` owns:
  - the `JSRuntime`;
  - class ids, made per runtime instead of static;
  - the rejection tracker, promise hook and module loader as dispatchers via
    `JS_GetContextOpaque(ctx)` → env;
  - the job queue;
  - the scope stack and value slots;
  - the pending exception;
  - `external_memory_` and the embedder hooks.
- **`napi_env__`** owns one `JSContext`.
- **Deleted:** `current_context_`, `napi_env_context_scope__` (23 sites),
  `napi_value__::context_` tags and `last_exception_context_`.
- **The trampoline takes its env from `func_data`** (the creating env). This
  is a deliberate behaviour change to match V8 and Node.
- **`module_wrap` and `compile_function`** honour the env's realm.

### host-JS

- **Split `HostJsEnv`** into:
  - `HostJsAgent`, per worker: values, frames, refs, `object_ids`,
    finalizers;
  - `HostJsEnv`: `global_context`, instance data, modules.
- **`create_realm`** builds a new `global_context`, optionally over a sandbox.
  This deletes the anonymous `wasmerNapiGlobalScopes` fallback and the
  `evaluation_scope(parsing_context)` branch.
- **Pass the env's context explicitly** to `context_eval`, `is_buffer` and
  `buffer_view`. That lets `wasmerNapiActiveGlobalContext` and the
  `activate()` switch go.
- **Limit.** These realms stay virtual (`with`/`Proxy` over one host realm),
  so intrinsics are shared. The API becomes uniform; real isolation would need
  host ShadowRealm, iframes or `node:vm`, which is out of scope.

### Bridge (`napi_bridge_init.cc`)

- **Split `SnapiEnvState`:**
  - `SnapiAgentState`: owner, value slots, scope frames, refs, deferreds,
    handle tables (module_wrap, bytecode, profile, buffer lease),
    `active_callback_ctx`, `value_limit`, the list of envs;
  - `SnapiEnvState`: agent pointer, `napi_env`, callback registry.
- **Guest value ids become agent-scoped**, so they stay valid across that
  agent's realms.
- **`g_envs` validation** maps env → agent.
- **Lock scope (side benefit).** With an agent object it is natural to replace
  the process-global `g_mu`, taken on every `unofficial_*` call, with a
  per-agent lock.

### Rust (`src/env.rs`, `src/guest/napi.rs`)

- **Agents get their own table.** `napi_scopes: scope_id → env_id` becomes
  `agents: agent_id → { principal env, heap charge, registry entry }`.
- **Budget is per agent.** Isolate reservation and heap charges stay on
  `create_env` only; `create_realm` charges no isolate, because a context
  counts against its isolate's heap.
- **`env_registry` and `terminate_all`** hold agents instead of envs.
- **The bridge reports realm death** so the env id and its leases can be
  dropped.
- **New guest imports** `unofficial_napi_create_realm` and
  `unofficial_napi_get_realm`.
- **Changed signatures get new import names.** The old ones stay as shims for
  one release; see §8.

## 6. EdgeJS migration

| Area | Change |
|---|---|
| `makeContext` | `create_realm`. Keep the `contextify_context_private_symbol` tag on the returned object. |
| `runInContext` | `get_realm(sandbox)`, then run on that env. |
| `compileFunction` | `get_realm(parsing_context)`, then compile on that env. |
| `ModuleWrap` | Create on `get_realm(context)`. Drop the "context given → skip bytecode cache" special case; the cache is per agent now. |
| Hooks | No change for vm contexts, because realm envs fall back to the principal env. A ShadowRealm later gets its own Edge `Realm` object holding the module loader state, binding registry and hooks, sharing the `edge::Environment`. |
| Isolate operations, workers | No change. |
| `attach_env` | No change: it is already called once, on the principal env. |
| Context-token handlers | Delete. |
| `process.dlopen` | Optional later: one addon env per addon. |

What must not change: callbacks keep receiving their creating env, and values
stay usable across envs. Both are invariants in §3.

## 7. Pitfalls

- **Envs held past their realm.** An embedder that keeps a realm env must
  also keep its realm reachable, for example by holding a ref to the global or
  sandbox, or by listening on that env's cleanup hook. EdgeJS stores what it
  needs on the sandbox, which satisfies this.
- **Cross-agent values remain invalid.** Workers still exchange values through
  `unofficial_napi_message`. A cheap agent check on value-taking calls would
  catch misuse in debug builds.
- **QuickJS behaviour change.** Callbacks called from vm code will see their
  creating realm, as on V8. Any QuickJS-only test that expects the vm global
  inside a callback will change.
- **Hook fallback is a policy.** It makes vm contexts behave like Node. Code
  that wants isolation (ShadowRealm) must set hooks on its realm env.
- **Teardown order.** The agent tears down after all of its realm envs. The
  principal env tears down last, then the isolate or runtime.

## 8. Phased plan

Every phase leaves the tests passing on V8, QuickJS and host-JS.

**Phase 0: internal split, no ABI change.**
- Introduce agent structs in V8, QuickJS, host-JS and the bridge, and move the
  isolate-level state into them.
- Make `env->context()` immutable everywhere.
- Fix the QuickJS trampoline.

**Phase 1: realm envs, additive ABI.**
- Add `create_realm` and `get_realm`.
- `contextify_make_context` becomes a shim over `create_realm`. The old
  context arguments resolve through `get_realm` and forward to the realm env.
- Dispatch hooks per realm with fallback to the parent.
- Collect realm envs.
- Make bridge value tables agent-scoped.
- Add the guest imports.

**Phase 2: EdgeJS moves to realm envs** (§6).

**Phase 3: remove the old surface.**
- Drop the context arguments, `make_context` and the context-token hooks.
- Bump the options structs (module create v2).
- Delete the records, scans and fallbacks listed in §5.

**Phase 4, optional:** per-addon envs, and ShadowRealm in EdgeJS.

## 9. Unmerged branches

| Branch | Recommendation |
|---|---|
| `feat/contextify-enter-leave` | Superseded: "one realm per document or frame" becomes `create_realm` plus that env. Its commits are a list of the costs of a selectable context, which this design avoids: root vs selected context, the microtask queue split, finalizers and interrupts running in the wrong realm, the handle leak, and callback cost going from 5.9 to 11 ns. |
| `feat/concurrent-stores`, `perf/concurrent-envs` | Land first. "Ids never reused" and "scope id = env id" become "agent id = principal env id". Terminate and interrupt then target the agent. |
| `fix/contextify-weak-records`, `fix/quickjs-contextify-collect` | Land first. Their lifetime model is the realm-env lifetime in §4.5. |
| `feat/trusted-script` | No conflict. Its hook is installed once per isolate, which is agent-level. |

## 10. Decisions to make

1. **Hook fallback to the parent** (recommended, matches Node) or explicit
   hooks per realm?
2. **Realm teardown:** GC only at first (recommended), or explicit detach from
   the start?
3. **Per-addon envs:** now, or after realm envs (recommended)?
4. **Guest ABI:** additive imports plus shims, then removal (recommended), or
   a `napi_extension_wasmer_v1` module?

## 11. Phase 0: what landed

Every engine now has an agent next to its env. Each agent still has exactly one
env, its principal env. There is no ABI change: public headers, guest imports
and the Rust↔C++ signatures are untouched. The one deliberate behaviour change
is the QuickJS trampoline.

### V8 (`v8/src`)

- **New `napi_agent__`** (`internal/napi_v8_agent.h`). It is owned by the owner
  handle (`UnofficialEnvScope`) and found from an isolate through isolate data
  slot 0.
- **Eight process-global maps are gone**, replaced by agent fields that need no
  lock: `g_env_by_isolate`, `g_hash_seeds`, `g_tracking_allocators`,
  `g_fatal_error_callbacks`, `g_near_heap_limit_callbacks`,
  `g_profiler_states`, `g_promise_reject_callbacks` and `g_promise_hooks`.
- **Also moved to the agent:**
  - the three `Private` keys;
  - the embedder hooks (context tokens, foreground task, fatal and OOM);
  - the microtask policy;
  - the default dynamic-import hook.
- **Moved to the env (realm):** the promise-reject callback and the four
  promise hooks. Dispatch goes isolate → agent → principal env; Phase 1
  resolves context → env instead.
- **`GetModuleWrapEnvForIsolate`** is an O(1) agent lookup instead of a scan.
- **`napi_agent__::Shutdown()`** now also removes the near-heap-limit
  trampoline. Before, it stayed registered with the env pointer as data after
  the env was freed.
- **Unchanged:** the env-keyed realm maps (`g_prepare_stack_trace_callbacks`,
  `g_error_formatting_states`, `g_context_records`, `g_module_wrap_states`) and
  `g_runtime_mu` around the platform.

### QuickJS (`quickjs/src`)

- **New `napi_agent__`** (`internal/napi_agent.{h,cc}`) holds:
  - the runtime;
  - the handle-scope stack;
  - the pending exception;
  - external memory and the embedder hooks;
  - continuation data and promise context frames;
  - the runtime-wide promise, rejection, import.meta and dynamic-import hooks,
    installed with the agent as opaque.
- **The env has one immutable `JSContext`.**
- **Deleted:**
  - `current_context_` and `napi_env_context_scope__` (16 sites);
  - `main_context()` / `current_context()`;
  - `last_exception_context_`;
  - the per-value, per-ref and per-deferred context tags. Values hold only the
    runtime.
- **Trampoline:** the env now comes from the function's own data, not the
  caller's context. A native function called from a vm script now sees its
  creating realm, as on V8 and Node.
- **New shared test:** `NativeFunctionCalledFromContextKeepsItsRealm`.
- **Teardown:** native callbacks are refused once teardown starts. Before, only
  calls from the main context were refused.
- **Not done:** class ids are still process-static.

### Bridge (`src/napi_bridge_init.cc`)

- **`SnapiAgentState` holds:** the owner, value slots, scope frames, refs,
  deferreds, handle tables, cbinfo invocations, `active_callback_ctx` and
  `value_limit`.
- **`SnapiEnvState` holds:** the agent pointer, the `napi_env`, and the
  callback registry and bindings.
- **Scope frames remember the env that opened them.**
- **The value cap counts across the agent's envs.**
- **Unchanged:** `g_mu` and `g_envs`.

### host-JS (`src/snapi_js.rs`)

- **`HostJsAgent` holds:** the handle tables and frames, `object_ids`, wraps,
  references, deferreds, finalizers, pending callbacks and
  `active_callback_ctx`.
- **`HostJsEnv` keeps:** its global context, `last_exception`, callbacks,
  closures, instance data and modules.
- **No implicit active context any more.** The module-global
  `wasmerNapiActiveGlobalContext`, `activate()` and the `ACTIVE_HOST_JS_ENV`
  thread-local are gone. The JS helpers take the env's context explicitly.
- **Bug fix:** buffer views made by `create_bytes_value` and
  `create_guest_buffer_view` used whichever env had been active last.
- **Unchanged:** vm sandbox semantics, including the fallback scope WeakMap.

### Tests

All counts are compared with `main`.

| Suite | Before | After |
|---|---|---|
| V8 (native) | 97 | 99 |
| QuickJS (native, at the recorded submodule commit) | 96 | 97 |
| Rust lib / `v8_host_control` | 48 / 3 | 48 / 3 |
| Manifest, native + WASIX | 36 | 36 |
| host-JS node / bun | 15 / — | 16 / 13 passed, 3 skipped |
| host-JS wasm-bindgen | 15 | 15 |

The new tests:

- V8 `NearHeapLimitCallbackReceivesItsEnvironment`;
- shared `NativeFunctionCalledFromContextKeepsItsRealm`;
- a host-JS test that two contexts each evaluate in their own global.

`make test-wasix-napi` does not work in standalone mode: the harness looks for
`./cargo-standalone.sh` under the temporary crate root. The manifest was run
with an equivalent script instead.

### Notes for Phase 1

- Callback, finalizer and interrupt paths in the bridge and host-JS still
  carry their env. Pending callbacks and finalizer drains read the exception of
  the env passed to the drain.
- The per-env callback registry and the opener env on frames exist, but
  nothing exercises them yet.
- `Box<HostJsAgent>` needs a shared handle once several envs share an agent.
