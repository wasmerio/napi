# A napi_env is a realm

## The rule

A `napi_env` is a realm: a global object and its intrinsics. It owns nothing
else.

- **What an env decides:** where objects are created (`napi_create_object`,
  `napi_get_global`, errors) and where code runs (`unofficial_napi_run_script`,
  `unofficial_napi_compile_function`, modules). A native function runs in the
  realm of the env that created it, and its callback gets that env.
- **What belongs to the engine instance** (the V8 isolate, the QuickJS
  runtime or the host-JS worker): values, handle scopes, references, wraps,
  the pending exception, the last error, instance data, cleanup hooks,
  modules, bytecode and every hook. Any env of an engine instance may be used
  with any of them.

Everything else follows:

1. Nothing has to find out which env owns a value, reference, module or
   scope, because envs own none of them.
2. Setting a hook, instance data or a cleanup hook through any env sets the
   engine instance's one slot. There are no parent envs and no fallback.
3. `unofficial_napi_create_env` makes an engine instance and its first realm;
   `unofficial_napi_create_realm` adds another; `unofficial_napi_release_env`
   frees the engine instance with all its realms. The first realm isn't
   otherwise special.

## API

### Realms

```c
napi_status unofficial_napi_create_realm(napi_env env,
                                         const unofficial_napi_realm_options* options_or_null,
                                         napi_env* realm_out,
                                         napi_value* global_out);
napi_status unofficial_napi_get_realm(napi_env env, napi_value value, napi_env* realm_out);
```

- **`create_realm`** makes a realm in `env`'s engine instance. With a sandbox,
  global property access in the realm reads and writes the sandbox, as with
  `vm.createContext(sandbox)`. A sandbox belongs to at most one realm; the
  first realm's global may still be one (`vm.createContext(globalThis)`).
- **`get_realm`** returns the env whose realm's global is `value`: a realm's
  own global, or its sandbox. Any other object gives NULL.
- **Options** (`unofficial_napi_realm_options`, version 1): sandbox, name,
  origin, host-defined option id, code-generation flags and an own microtask
  queue. Only V8 honours the code-generation-from-strings flag and the own
  microtask queue; name and origin are labels.

### Code in a realm

| Call | Acts in |
|---|---|
| `unofficial_napi_run_script(env, source, …)` | env's realm. A realm with its own microtask queue drains it after the script. |
| `unofficial_napi_compile_function(env, source, …)` | env's realm, as `vm.compileFunction` does |
| `unofficial_napi_module_wrap_create(env, options, …)` | env's realm (options version 2) |

### Removed

- `unofficial_napi_contextify_make_context`: use `create_realm`.
- `unofficial_napi_contextify_run_script` and
  `unofficial_napi_contextify_compile_function`, which took a sandbox or
  parsing context.
- `context_or_undefined` in the module-create options (now version 2).
- The context-token callbacks in `unofficial_napi_env_hooks` (now version 2).
  Their hook bits are retired, not reused.

Providers reject version 1 of both structs. Wasm guests get the same API
through `napi_extension_wasmer_v0`; the guest ABI reads only the new layouts,
and the removed imports are gone. A guest built against older headers fails
instead of being shimmed.

## Where state lives

| Layer | An env holds | The engine instance holds |
|---|---|---|
| V8 | isolate, agent, `v8::Context`, own microtask queue | refs and finalizers, scopes, slab allocator, instance data, cleanup hooks, buffer records, hooks, module records, error-formatting state, pending exception |
| QuickJS | agent, `JSContext` | refs, deferreds, external buffers, scopes, pending exception, last error, instance data, cleanup hooks, promise hooks and reject callback, contextify and module state, every realm's `JSContext` |
| host-JS | agent, global context | values, frames, refs, callbacks, instance data, modules, bytecode |
| Bridge | `SnapiAgentState`, `napi_env` | value, scope, ref and handle tables, guest callback registry |
| Rust | a guest env id per realm | budget, owner scope and registry entry, on the first env |

**V8** finds a context's env through embedder-data slot 33, and an isolate's
agent through isolate data slot 0. Twelve N-API calls that create objects in
V8's current context enter the env's context first (`NapiV8RealmScope`).

**QuickJS** realms are `JSContext`s on one `JSRuntime`. A native function
gets its env from its function data, because QuickJS doesn't switch realms
for C functions. `get_realm` is a map lookup by global or sandbox.

**host-JS** realms are virtual: each has its own global scope object over one
host JavaScript realm, so intrinsics are shared.

## Lifetime

Realm envs live until their engine instance is released. Release runs the
cleanup hooks and finalizers first, while every env is alive, so a finalizer
gets the env it was registered through. It then frees the realm envs, the
first env and the engine instance.

## How EdgeJS uses it

- `vm.createContext` → `create_realm`.
- `runInContext`, `compileFunction`, `moveMessagePortToContext` and
  `ModuleWrap` → `get_realm` on the contextified object, then the call
  through that realm's env.
- Everything else, hooks included, goes through the first env.

## Not done

- **Collecting realms with their sandbox.** Realms live until their engine
  instance is released. #109 (V8) and #110 (QuickJS) did this for the old
  context records; their approach applies to realm envs now.
- **Separate envs per native addon.** Every addon shares the engine
  instance's instance data and cleanup hooks. Node gives each addon its own
  env; EdgeJS already passes one env to every addon.
- **Per-realm hooks** (for example for ShadowRealm) would be an override
  added later.
- **`vm.createContext(globalThis)` on V8** breaks the main realm afterwards,
  because the provider turns the sandbox itself into the new realm's global.
  This predates the change; `main` behaves the same.
- **Sandboxes work differently per engine.** V8 turns the sandbox into the
  realm's global; QuickJS copies sandbox properties in and out around each
  script; host-JS evaluates against the sandbox with `with`. On QuickJS a
  function compiled in a sandbox realm sees sandbox globals only after a
  script has run there.

## History

- **#111** split each provider into an engine instance (agent) and its env.
- **This change** makes the env a realm that owns nothing else. It replaces
  #112 and #114, which kept per-realm state, parent fallbacks and shims for
  older guests.
