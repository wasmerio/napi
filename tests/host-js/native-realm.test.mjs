import assert from 'node:assert/strict';
import test from 'node:test';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

// Test the native Worker realm adapter embedded in the wasm-bindgen module:
// guest code runs in the Worker's own global when the embedder opts in through
// globalThis.__wasmerNapiNativeRealm, and in isolated scopes otherwise.
const rust = await readFile(new URL('../../src/snapi_js.rs', import.meta.url), 'utf8');
const inline = rust.match(/#\[wasm_bindgen\(inline_js = r#"([\s\S]*?)"#\)\]/)?.[1];
assert.ok(inline, 'host JavaScript implementation not found');
// These paths never parse or lower code with Acorn; fail loudly if they try.
const script = inline
  .replace("import { parse as wasmerNapiParse } from 'acorn';",
    'const wasmerNapiParse = () => { throw new Error("Unexpected Acorn source preparation"); };')
  .replaceAll('export function ', 'function ');
assert.ok(!/^import /m.test(script), 'unexpected import in the host JavaScript');
const apiNames = [...inline.matchAll(/export function (wasmer_napi_\w+)\(/g)].map(match => match[1]);

// Loads the host JavaScript into a fresh realm, with or without a native realm
// bridge, and records what the bridge is asked to do.
function load(native, overrides = {}, setup = '') {
  const context = vm.createContext({ setTimeout, queueMicrotask, console });
  const calls = { scripts: [], depth: 0, enters: 0, leaves: 0, released: 0, checkpoints: [] };
  if (native) {
    context.__wasmerNapiNativeRealm = {
      evaluate(source, filename, lineOffset, columnOffset) {
        calls.scripts.push({ source, filename, lineOffset, columnOffset });
        return vm.runInContext(source, context, { filename });
      },
      binding(source) { return vm.runInContext(source, context); },
      enterCallback() { ++calls.depth; ++calls.enters; },
      leaveCallback() { --calls.depth; ++calls.leaves; },
      release() { ++calls.released; },
      checkpoint(...args) { calls.checkpoints.push(args); return Promise.resolve(); },
      ...overrides,
    };
  }
  vm.runInContext(`'use strict';\n${setup}\n${script}\nthis.providerAPI = {${apiNames.join(',')}};`, context);
  return { context, api: context.providerAPI, calls };
}

test('a native realm runs scripts in the Worker global with their source positions', () => {
  const { api, calls, context } = load(true);
  assert.equal(api.wasmer_napi_has_native_realm(), true);
  assert.equal(api.wasmer_napi_can_create_global_context(), true);
  const root = api.wasmer_napi_create_global_context();
  const scope = api.wasmer_napi_global_context_scope(root);
  assert.equal(scope, vm.runInContext('globalThis', context));

  const source = "'use strict'; let lexical = 19; const base = 23; var globalValue = 11;";
  api.wasmer_napi_context_run_script(root, source, 'https://fixture.test/original.js', 4, 2);
  assert.deepEqual(calls.scripts, [
    { source, filename: 'https://fixture.test/original.js', lineOffset: 4, columnOffset: 2 },
  ]);
  // Global lexical declarations persist across scripts, as in a real Worker.
  assert.equal(api.wasmer_napi_context_run_script(root, 'lexical + base + globalValue', 'second.js', 0, 0), 53);
  assert.equal(api.wasmer_napi_context_eval(root, '(() => lexical + base)()'), 42);
  assert.equal(Object.hasOwn(scope, 'lexical'), false);
  assert.equal(scope.globalValue, 11);
  assert.equal(api.wasmer_napi_compile_function(scope, [], ['value'], 'return value + lexical;', '')(4), 23);
});

test('a native realm serves one environment and no secondary contexts', () => {
  const { api } = load(true);
  const root = api.wasmer_napi_create_global_context();
  const scope = api.wasmer_napi_global_context_scope(root);
  assert.equal(api.wasmer_napi_can_create_global_context(), false);
  assert.throws(() => api.wasmer_napi_create_global_context(), /lease already consumed/);
  const registry = api.wasmer_napi_create_realm_registry();
  assert.throws(() => api.wasmer_napi_create_realm_context(registry, null), /Secondary contexts/);
  assert.throws(() => api.wasmer_napi_create_realm_context(registry, {}), /Secondary contexts/);
  assert.throws(() => api.wasmer_napi_compile_function(scope, [{}], [], 'return 42;', ''), /Context extensions/);
  assert.throws(() => api.wasmer_napi_compile_module(scope, 'export default 42;', ''), /module/);
});

test('a native realm brackets callbacks, routes checkpoints and is never recycled', async () => {
  const { api, calls } = load(true);
  const root = api.wasmer_napi_create_global_context();
  const callback = api.wasmer_napi_make_callback(root, (receiver, args) => {
    assert.equal(calls.depth, 1);
    assert.equal(receiver.marker, 41);
    return args[0] + 1;
  });
  assert.equal(callback.call({ marker: 41 }, 5), 6);
  const throwing = api.wasmer_napi_make_callback(root, () => { throw Error('callback test'); });
  assert.throws(throwing, /callback test/);
  assert.deepEqual([calls.depth, calls.enters, calls.leaves], [0, 2, 2]);

  await api.wasmer_napi_event_loop_checkpoint(false, true);
  assert.deepEqual(calls.checkpoints, [[false, true]]);

  api.wasmer_napi_release_global_context(root);
  assert.equal(calls.released, 1);
  assert.equal(callback.call({ marker: 41 }, 5), undefined, 'released callbacks cannot dispatch into Rust');
  assert.equal(api.wasmer_napi_can_create_global_context(), false, 'a released realm is not recycled');
});

test('an invokeCallback hook makes calls into the guest, inside the bracket', () => {
  const standIn = { name: 'stand-in' };
  const real = { name: 'real' };
  const hooked = [];
  const { api, calls } = load(true, {
    // As an embedder virtualizing globals would: map the receiver to the real
    // one, and change the result on the way back.
    invokeCallback(dispatch, receiver, args, newTarget) {
      hooked.push([calls.depth, newTarget]);
      return dispatch(receiver === standIn ? real : receiver, args, newTarget) * 10;
    },
  });
  const root = api.wasmer_napi_create_global_context();
  const callback = api.wasmer_napi_make_callback(root, (receiver, args) => {
    assert.equal(receiver, real);
    return args[0] + 1;
  });
  assert.equal(callback.call(standIn, 4), 50);
  assert.deepEqual(hooked, [[1, undefined]]);
  assert.deepEqual([calls.depth, calls.enters, calls.leaves], [0, 1, 1]);
  // A throwing hook still leaves the bracket.
  const throwing = load(true, { invokeCallback() { throw new Error('invoke test'); } });
  const throwingRoot = throwing.api.wasmer_napi_create_global_context();
  assert.throws(() => throwing.api.wasmer_napi_make_callback(throwingRoot, () => 1)(), /invoke test/);
  assert.deepEqual([throwing.calls.depth, throwing.calls.enters, throwing.calls.leaves], [0, 1, 1]);
});

test('without the bridge, contexts stay isolated from the global and each other', () => {
  const { api, context } = load(false);
  assert.equal(api.wasmer_napi_has_native_realm(), false);
  const first = api.wasmer_napi_create_global_context();
  const second = api.wasmer_napi_create_global_context();
  api.wasmer_napi_context_eval(first, 'globalThis.fixtureValue = 17');
  assert.equal(first.scope.fixtureValue, 17);
  assert.equal(second.scope.fixtureValue, undefined);
  assert.equal(vm.runInContext('globalThis.fixtureValue', context), undefined);
  assert.equal(api.wasmer_napi_can_create_global_context(), true);
});

test('the module captures the bridge and removes it from the global', () => {
  const { context, api } = load(true);
  assert.equal(api.wasmer_napi_has_native_realm(), true);
  assert.equal(vm.runInContext('typeof __wasmerNapiNativeRealm', context), 'undefined');
});

test('the module refuses a bridge it cannot remove from the global', () => {
  const context = vm.createContext({ setTimeout, queueMicrotask, console });
  vm.runInContext(`Object.defineProperty(globalThis, '__wasmerNapiNativeRealm', {
    value: { evaluate() {}, binding() {}, enterCallback() {}, leaveCallback() {} },
  });`, context);
  assert.throws(() => vm.runInContext(script, context), /must be a configurable global property/);
});

test('checkpoint, release and drain hook failures stay inside the bridge', async () => {
  const errors = [];
  const { api, context } = load(true, {
    checkpoint() { throw new Error('checkpoint test'); },
    release() { throw new Error('release test'); },
    enterCallback() { throw new Error('enter test'); },
    leaveCallback() { throw new Error('leave test'); },
  });
  context.console = { error: (...args) => errors.push(args[1].message) };
  const root = api.wasmer_napi_create_global_context();
  // A throwing checkpoint becomes a rejected Promise, not a synchronous throw,
  // and is logged, since the guest only sees a status.
  const checkpoint = api.wasmer_napi_event_loop_checkpoint(false, true);
  await assert.rejects(checkpoint, /checkpoint test/);
  // The drain has already taken its record: the hooks must not skip it.
  api.wasmer_napi_native_realm_enter();
  api.wasmer_napi_native_realm_leave();
  api.wasmer_napi_release_global_context(root);
  assert.deepEqual(errors, ['checkpoint test', 'enter test', 'leave test', 'release test']);
});

test('a checkpoint that rejects later is logged too', async () => {
  const errors = [];
  const { api, context } = load(true, {
    checkpoint() { return Promise.reject(new Error('async checkpoint test')); },
  });
  context.console = { error: (...args) => errors.push(args[1].message) };
  await assert.rejects(api.wasmer_napi_event_loop_checkpoint(true, false), /async checkpoint test/);
  assert.deepEqual(errors, ['async checkpoint test']);
});

test('without a checkpoint hook the default checkpoint runs', async () => {
  const { api } = load(true, { checkpoint: undefined });
  await api.wasmer_napi_event_loop_checkpoint(false, true);
});

test('callbacks see new.target, compiled functions accept a hashbang', () => {
  const { api } = load(true);
  const root = api.wasmer_napi_create_global_context();
  const scope = api.wasmer_napi_global_context_scope(root);
  const seen = [];
  const ctor = api.wasmer_napi_make_callback(root, (receiver, args, newTarget) => {
    seen.push(newTarget);
  });
  ctor();
  new ctor();
  assert.deepEqual(seen, [undefined, ctor]);
  const entry = api.wasmer_napi_compile_function(scope, [], ['x'], '#!/usr/bin/env node\nreturn x * 2;', 'main.js');
  assert.equal(entry(21), 42);
});

test('guest code run from a drain is bracketed in a native realm only', () => {
  const native = load(true);
  native.api.wasmer_napi_native_realm_enter();
  assert.equal(native.calls.depth, 1);
  native.api.wasmer_napi_native_realm_leave();
  assert.deepEqual([native.calls.depth, native.calls.enters, native.calls.leaves], [0, 1, 1]);
  const ordinary = load(false);
  ordinary.api.wasmer_napi_native_realm_enter();
  ordinary.api.wasmer_napi_native_realm_leave();
});

test('deferred callbacks do not require an activation hook', () => {
  const { api } = load(true);
  assert.equal(api.wasmer_napi_native_realm_enter_deferred({}), undefined);
  const expected = {};
  const native = load(true, { enterDeferredCallback(document) {
    assert.equal(document, expected);
    return { mapResult(value) { return value + 1; }, leave() {} };
  }});
  const frame = native.api.wasmer_napi_native_realm_enter_deferred(expected);
  assert.equal(native.api.wasmer_napi_native_realm_leave_deferred(frame, 41, true), 42);
});

test('binding factories build native class and method shapes under poisoned intrinsics', () => {
  const { context, api } = load(false);
  const root = api.wasmer_napi_create_global_context();
  const registry = api.wasmer_napi_create_realm_registry();
  const key = api.wasmer_napi_create_realm_context(registry, null, 27);
  const realm = api.wasmer_napi_realm_context(registry, key);
  assert.equal(api.wasmer_napi_realm_data(realm), 27);
  const calls = [];
  api.wasmer_napi_set_binding_dispatch(root, (reg, data, receiver, args, target, current) => {
    calls.push({ reg, data, receiver, args: [...args], target, current });
    return reg;
  });
  vm.runInContext(
    "const define = Object.defineProperty; Object.defineProperty = Object.setPrototypeOf = () => { throw Error('poison'); };"
    + "for (const key of ['get', 'set', 'value']) define(Object.prototype, key, {__proto__:null,get(){throw Error(key);}});",
    context,
  );
  const base = api.wasmer_napi_binding_function(root, realm, 1, 'Base', 2, 1, 0);
  const basePrototype = api.wasmer_napi_binding_prototype(base, undefined, undefined);
  const derived = api.wasmer_napi_binding_function(root, realm, 1, 'Derived', 3, 2, 19);
  const derivedPrototype = api.wasmer_napi_binding_prototype(derived, base, basePrototype);
  api.wasmer_napi_binding_member(root, realm, derivedPrototype, 'name with spaces', 5, undefined, 3, 4, 0, 0, 23);
  api.wasmer_napi_binding_member(root, realm, derivedPrototype, 'x', 6, undefined, 0, 0, 4, 5, 29);
  const fn = derivedPrototype['name with spaces'];
  const accessor = Object.getOwnPropertyDescriptor(derivedPrototype, 'x');
  assert.equal(Object.getPrototypeOf(derived), base);
  assert.equal(Object.getPrototypeOf(derivedPrototype), basePrototype);
  assert.deepEqual(Object.getOwnPropertyDescriptor(derived, 'prototype'), {
    value: derivedPrototype, writable: false, enumerable: false, configurable: false,
  });
  assert.deepEqual(Object.getOwnPropertyDescriptor(derivedPrototype, 'constructor'), {
    value: derived, writable: true, enumerable: false, configurable: true,
  });
  const obj = new derived();
  assert.equal(obj instanceof base, true);
  assert.equal(calls.at(-1).target, derived);
  assert.equal(calls.at(-1).data, 19);
  assert.equal(calls.at(-1).current, realm);
  assert.equal(derived.call(base), 2);
  assert.equal(calls.at(-1).receiver, base);
  assert.equal(calls.at(-1).target, undefined);
  assert.deepEqual(Object.getOwnPropertyDescriptor(fn, 'name'), {
    value: 'name with spaces', writable: false, enumerable: false, configurable: true,
  });
  assert.equal(fn.length, 4);
  assert.equal(Object.hasOwn(fn, 'prototype'), false);
  assert.throws(() => new fn(), { name: 'TypeError' });
  assert.equal(fn.call(null), 3);
  assert.equal(calls.at(-1).data, 23);
  assert.equal(calls.at(-1).receiver, null);
  assert.equal(fn.call(17), 3);
  assert.equal(calls.at(-1).receiver, 17);
  assert.equal(fn(), 3);
  assert.equal(calls.at(-1).receiver, undefined);
  assert.equal(accessor.get.name, 'get x');
  assert.equal(accessor.get.length, 0);
  assert.equal(accessor.set.name, 'set x');
  assert.equal(accessor.set.length, 1);
  assert.equal(accessor.get.call(obj, 42), 4);
  assert.deepEqual(calls.at(-1).args, [42]);
  assert.equal(accessor.set.call(obj), 5);
  assert.equal(calls.at(-1).reg, 5);
  assert.equal(calls.at(-1).data, 29);
  assert.equal(accessor.get, Object.getOwnPropertyDescriptor(derivedPrototype, 'x').get);
  assert.equal(accessor.set, Object.getOwnPropertyDescriptor(derivedPrototype, 'x').set);
  assert.throws(() => new accessor.get(), { name: 'TypeError' });
  api.wasmer_napi_release_global_context(root);
  assert.equal(fn(), undefined);
});


test('fresh realm setup uses captured reflection and realm registry methods', () => {
  const { context, api } = load(false);
  const registry = api.wasmer_napi_create_realm_registry();
  const key = api.wasmer_napi_create_realm_context(registry, null, 41);
  const realm = api.wasmer_napi_realm_context(registry, key);
  api.wasmer_napi_context_eval(realm, `
    const rootError = Object.getPrototypeOf(Error);
    rootError.captureStackTrace = () => { throw Error('poisoned captureStackTrace'); };
    Object.getPrototypeOf([][Symbol.iterator]()).next = () => { throw Error('poisoned iterator'); };
  `);
  vm.runInContext(
    "const poisoned = () => { throw Error('poison'); };"
    + "Object.defineProperty = Object.getOwnPropertyDescriptors = Object.setPrototypeOf = poisoned;"
    + "Reflect.ownKeys = Reflect.get = WeakMap.prototype.get = WeakMap.prototype.set = poisoned;",
    context,
  );
  for (const owner of [73, 97]) {
    const fresh = api.wasmer_napi_create_realm_context(registry, null, owner);
    const realm = api.wasmer_napi_realm_context(registry, fresh);
    assert.equal(api.wasmer_napi_realm_data(realm), owner);
    assert.match(api.wasmer_napi_context_eval(realm, 'new Error("intact").stack'), /intact/);
  }
});

test('an unsupported stack probe is attempted only once', () => {
  const { context, api } = load(false, {}, `
    let probes = 0;
    Error.captureStackTrace = () => { ++probes; };
  `);
  api.wasmer_napi_create_global_context();
  api.wasmer_napi_create_global_context();
  assert.equal(vm.runInContext('probes', context), 1);
});

test('an embedder can install a stack shim before loading the provider', () => {
  const { api } = load(false, {}, `
    Error.captureStackTrace = target => {
      Object.defineProperty(target, 'stack', {
        configurable: true, get() {
          return Error.prepareStackTrace(target, [{getFileName: () => '/stack-shim.js'}]);
        },
      });
    };
  `);
  const context = api.wasmer_napi_create_global_context();
  const error = api.wasmer_napi_context_eval(context, `
    Error.prepareStackTrace = (error, frames) => ({ error, frames });
    new Error('shim');
  `);
  assert.equal(error.stack.error, error);
  assert.equal(error.stack.frames[0].getFileName(), '/stack-shim.js');
});
