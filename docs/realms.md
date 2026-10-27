# Realms and envs

## The model

- **An env serves one engine instance and all its realms.** Its own realm is
  the engine instance's main realm. `unofficial_napi_create_realm` makes more
  (vm contexts, or a browser's documents) and returns an
  `unofficial_napi_realm`. A realm is not an env.
- **N-API acts in one realm at a time, the current realm**, as an ECMAScript
  engine runs code in the realm of its running execution context:
  - what N-API creates comes from the current realm, with its intrinsics, and
    `napi_get_global` returns its global;
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
  - Options: sandbox, name, origin, host-defined option id, the
    code-generation flags and an own microtask queue (V8 only).
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
    another env, or a collected one, fails with `napi_invalid_arg`.
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
- **Changed from `main`:**
  - `unofficial_napi_contextify_make_context` is replaced by `create_realm`.
  - `unofficial_napi_contextify_run_script` and
    `unofficial_napi_contextify_compile_function` become `run_script` and
    `compile_function`. They take a realm instead of a sandbox or parsing
    context.
  - The module options' `context_or_undefined` becomes `realm_or_null`.
  - The context-token hooks are gone (env hooks are version 2).
- **Guest ABI:** a realm id is the value id of the realm's global or sandbox,
  and 0 is the env's realm. The bridge keeps no realm table: it resolves the
  id again with `get_realm` on each call, so the id lives exactly as long as
  its value id. Version 1 structs are rejected and the old imports are gone.
  The scope imports are `unofficial_napi_enter_realm(env, realm_id,
  scope_out_ptr) -> status` and `unofficial_napi_leave_realm(env, scope: i64)
  -> status`; the token is the provider's, passed through as a u64.

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
| QuickJS | a record with a `JSContext` on the env's runtime; a sandbox realm copies the sandbox's properties in and out around each call | a map from global or sandbox to record | The key (the sandbox, or the realm's global) holds a function of the realm under a private symbol, and a sandbox realm's global holds the sandbox, so the collector frees the cycle with its key. The record holds the key weakly, and the next `create_realm` frees collected records (from #110). |
| host-JS | a virtual global context (a `with`/Proxy scope over one host realm) | a per-env `WeakMap` from global or sandbox to context | Nothing holds a realm strongly; the host GC collects it with its key. |

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
  - QuickJS copies sandbox properties in and out around scripts.
  - host-JS evaluates against the sandbox with `with`.

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
