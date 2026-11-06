# Trusted script sources and execution-state queries

PR #106 adds two env-based helpers:

```c
napi_status unofficial_napi_create_trusted_script(
    napi_env env, napi_value source, napi_value* result);
napi_status unofficial_napi_is_executing_js(napi_env env, bool* result);
```

They follow the [realm model](realms.md): one env serves an engine instance,
object creation uses the current N-API realm, and a native callback acts in the
realm where its function was created. Neither helper needs a new realm argument.
Both must be called on the engine's owning thread.

## Support

| Provider | create_trusted_script | is_executing_js |
|---|---|---|
| V8 | Native code-like object and code-generation hook | Native stack and microtask-checkpoint state |
| QuickJS | Unsupported; napi_generic_failure and NULL output | Unsupported; napi_generic_failure and false output |
| Host JS | Unsupported; napi_generic_failure and zero value id | Unsupported; napi_generic_failure and false output |

Unsupported providers do not emulate direct eval or infer VM execution state
from adapter bookkeeping. Guest stubs still validate output pointers and zero
valid outputs. A value id occupies four bytes; a bool occupies one byte.

## Trusted-script contract on V8

The source must be a primitive string. Objects, including boxed strings, are
rejected with `napi_string_expected` without coercion. Null arguments are
invalid. The result is an object allocated in the current realm, with a native
default `toString` from that realm. It is not itself a full WebIDL TrustedScript
implementation; the embedder supplies any WebIDL prototype and policy layer.

Direct `eval(trusted)` uses the stored primitive source and preserves the
caller's lexical scope. It does not call author `toString` or `Symbol.toPrimitive`.
Indirect eval keeps ordinary global-eval semantics. The native brand cannot be
copied, inherited, obtained by name through `create_private_symbol`, or forwarded
by a Proxy. Prototype replacement and freezing do not remove the object's brand.

A trusted source can be passed between realms of the same agent. Eval uses the
receiving caller's scope and code-generation policy, not the source object's
creation realm. A receiving realm with string code generation disabled rejects
it with `EvalError`; the object is not a policy bypass.

**Function constructors have a narrower guarantee.** `Function(trusted)` works
through the default native stringifier, but V8 converts its arguments before
its code-generation hook. An author's replacement `toString` or
`Symbol.toPrimitive` can therefore change the function source. This API does
not provide hardened Trusted Types integration for Function, AsyncFunction,
GeneratorFunction, or AsyncGeneratorFunction. V8's
[dynamic-function implementation](https://github.com/v8/v8/blob/13.6.233/src/builtins/builtins-function.cc)
shows that argument coercion precedes compilation. The regression suite pins
ordinary Function coercion so it cannot be confused with the direct-eval guarantee.

Creation follows the provider's sticky termination contract and returns
`napi_pending_exception` while execution is terminating. It remains usable in
cleanup hooks while the live env and its agent support JavaScript, consistent
with `release_env`. The read-only execution query also remains usable during
termination and cleanup and preserves a pending exception.

## Agent state and hook ownership

The private brand and reusable code-like ObjectTemplate belong to `napi_agent__`.
They are shared by the env's realms and reset before the isolate is disposed.
There is no process-global map or lock for trusted-script brands. A single
reusable template avoids V8 retaining a fresh template instantiation for every
created object. The template does not retain its first creation realm after
that realm and its values become unreachable; a regression checks collection.

On the first trusted-script creation in an agent, the provider installs its
isolate-wide `ModifyCodeGenerationFromStrings` callback. V8 supplies no getter
for the previous callback, so an earlier embedder callback is replaced and
cannot be chained. An embedder callback installed later is preserved: further
trusted-script creation does not reinstall the provider hook. Such a callback
then determines how code-like objects are treated.

For values not bearing the provider's brand, the provider hook preserves V8's
behavior: eval returns unbranded non-string objects unchanged, while ordinary
strings follow the receiving context's code-generation policy. This hook is
not a general CSP/Trusted Types policy API. In particular, when string code
generation is allowed V8 does not send ordinary strings through this hook, so
it cannot enforce a “trusted objects only” policy in that configuration.

## Execution-state semantics

`is_executing_js` describes the whole engine instance, across its realms and
security tokens. It returns true while a user-JavaScript frame exists or a
microtask checkpoint is running. Provider-driven checkpoints, including a
realm's own queue, use an agent-wide scoped depth so the answer stays true
when a callback temporarily runs in a different N-API realm. The query does
not scan the realm registry or take bridge/provider locks.

Entering a realm from C++ does not mean JavaScript is executing. A native
function called directly from C, or through a builtin with no user-JavaScript
frame below it, returns false outside a checkpoint. A native callback invoked
by a promise checkpoint returns true even when no user-JavaScript frame is
visible. This contract measures observable VM frames and checkpoints, not all
possible internal engine activity.

The bridge calls use the existing owner-thread validation. They do not hold
`g_mu` while invoking the provider; restoring the old recursive lock or
changing it to a plain-mutex guard would violate the bridge's concurrency model.

## QuickJS follow-up

Native QuickJS support would be useful for embedders using this API across
providers, but it needs an engine change. At the pinned QuickJS revision,
`JS_EvalObject` returns non-string arguments unchanged before entering the
parser. The Function constructor separately stringifies its arguments.
Replacing the global eval function or coercing the object in the provider
cannot preserve native direct-eval scope and the unforgeable-source guarantee.

A separate engine/provider follow-up should cover:

1. A native branded object with a traced primitive-string payload, correct
   finalization, and no inherited or Proxy-forwarded brand lookup.
2. Unwrapping only that brand inside the direct/indirect eval engine path,
   preserving its caller frame, lexical scope, strictness, and eval mode.
3. Receiving-realm dynamic-code-generation policy. QuickJS currently ignores
   `create_realm`'s code-generation flags, so equivalent policy enforcement
   needs engine support too; it must not silently claim V8's restriction.
4. A deliberate Function-constructor contract, including its ordinary argument
   coercion, with no stronger guarantee than is implemented.
5. Forgery, prototype mutation, proxies, cross-realm scope/policy, termination,
   GC, teardown, Unicode, and native/WASIX regression coverage.

`is_executing_js` is a separate capability: implementing trusted scripts should
not make it claim support. An execution query would also need native engine
stack/job state and tests for C-only callbacks versus JavaScript callers.

This PR keeps both QuickJS capabilities explicitly unsupported. It does not
modify the QuickJS fork or change its pinned revision.

## Validation of the realm adaptation

The primary runner is `tests/runners/test_79_unofficial_trusted_script.cc`:
20 V8 cases and one QuickJS unsupported case. Original direct-eval and brand
checks are retained. The new realm coverage includes callback-creation realm,
receiving-realm lexical scope and policy, realm collection, security-token
boundaries, entered-but-idle realms, termination, cleanup hooks, and Function's
coercion limitation. The deprecated context-token hook callback is still
checked through `create_realm`; other tests use the explicit realm APIs.

`tests/programs/test_trusted_script.c` drives both helpers and the realm
imports natively and through WASIX, including pointer bounds, code generation,
and cross-realm direct eval. A wasm-bindgen test instantiates a WAT guest to
check real host-JS import dispatch, unsupported statuses, output zeroing,
and the different u32/bool pointer bounds.

Recorded macOS arm64 results and remaining limitations are reported in the
PR description. The V8 package is named 11.9.9; its headers report V8
13.6.233.17. No EdgeJS or WebkitWasm application build is claimed by these
provider/guest tests, and no execution-query performance improvement is claimed:
the V8 query still captures a stack trace on each call.
