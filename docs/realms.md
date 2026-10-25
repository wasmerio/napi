# Realms and envs

## The model

- **An env is one engine instance's view from its main realm.** Every N-API
  call through it creates objects in that realm, and the native functions it
  makes run there. It never switches realms. This is Node's model: a
  `napi_env` belongs to the principal realm, and vm contexts get none.
- **A vm context is another realm of the same engine instance, but not an
  env.** `unofficial_napi_create_realm` makes one and returns an
  `unofficial_napi_realm`. Only the calls that run code take a realm:
  `unofficial_napi_run_script`, `unofficial_napi_compile_function`, module
  creation and `unofficial_napi_bytecode_open`. A NULL realm means the env's
  own.
- **Hooks, instance data and cleanup hooks stay on the env.** A hook fires
  for code running in any of its realms, as Node's hooks do for vm contexts.
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

- **QuickJS needs engine commit 7ddf9e3**, which makes a pending job hold its
  context. Without it, a job queued in a realm that is then collected runs on
  a freed context. With it, a realm's pending jobs keep the realm alive until
  they run.
- **ShadowRealm.** A realm with its own bindings and hooks, and no fallback
  to the main realm, would be a separate concept, closer to Node's `Realm`.
- **Native objects inside a vm realm.** Native functions always belong to the
  env's realm. EdgeJS creates the objects `moveMessagePortToContext` needs by
  running a script in the realm.
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

Both kept an env per vm context. That means native code has to know which
env it holds, and every hook, scope and reference has to work across envs.
Here, vm contexts are realm handles and only the four calls that run code
take one, so the rest of N-API keeps a single env per engine instance.
