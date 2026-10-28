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
function load(native, overrides = {}) {
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
  vm.runInContext(`${script}\nthis.providerAPI = {${apiNames.join(',')}};`, context);
  return { context, api: context.providerAPI, calls };
}

test('a native realm runs scripts in the Worker global with their source positions', () => {
  const { api, calls, context } = load(true);
  assert.equal(api.wasmer_napi_has_native_realm(), true);
  assert.equal(api.wasmer_napi_can_create_global_context(), true);
  const root = api.wasmer_napi_create_global_context();
  const scope = api.wasmer_napi_global_context_scope(root);
  assert.equal(scope, vm.runInContext('globalThis', context));
  api.wasmer_napi_activate_global_context(root);

  const source = "'use strict'; let lexical = 19; const base = 23; var globalValue = 11;";
  api.wasmer_napi_context_run_script(scope, source, 'https://fixture.test/original.js', 4, 2);
  assert.deepEqual(calls.scripts, [
    { source, filename: 'https://fixture.test/original.js', lineOffset: 4, columnOffset: 2 },
  ]);
  // Global lexical declarations persist across scripts, as in a real Worker.
  assert.equal(api.wasmer_napi_context_run_script(scope, 'lexical + base + globalValue', 'second.js', 0, 0), 53);
  assert.equal(api.wasmer_napi_context_eval(scope, '(() => lexical + base)()'), 42);
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
  assert.throws(() => api.wasmer_napi_context_eval({}, '42'), /Secondary contexts/);
  assert.throws(() => api.wasmer_napi_context_run_script({}, '42', '', 0, 0), /Secondary contexts/);
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

test('without the bridge, contexts stay isolated from the global and each other', () => {
  const { api, context } = load(false);
  assert.equal(api.wasmer_napi_has_native_realm(), false);
  const first = api.wasmer_napi_create_global_context();
  const second = api.wasmer_napi_create_global_context();
  api.wasmer_napi_activate_global_context(first);
  api.wasmer_napi_context_eval(first.scope, 'globalThis.fixtureValue = 17');
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
