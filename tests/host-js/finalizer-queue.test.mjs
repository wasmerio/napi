import assert from 'node:assert/strict';
import test from 'node:test';
import { setTimeout } from 'node:timers/promises';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

// Test the host-JS finalizer queue embedded in the wasm-bindgen module, first
// with collections the test reports itself, then with the host's own GC.
const source = await readFile(new URL('../../src/snapi_js.rs', import.meta.url), 'utf8');
const inline = source.match(/#\[wasm_bindgen\(inline_js = r#"([\s\S]*?)"#\)\]/)?.[1];
assert.ok(inline, 'host JavaScript implementation not found');
const start = inline.indexOf('// Objects with a guest finalizer.');
const end = inline.indexOf('export function wasmer_napi_has_jspi()', start);
assert.ok(start >= 0 && end > start, 'finalizer queue not found');
const script = inline.slice(start, end).replace(/^export /gm, '') + `
globalThis.api = {
  registry: wasmer_napi_finalizer_registry_create,
  queue: wasmer_napi_finalizer_queue_create,
  register: wasmer_napi_finalizer_register,
  take: wasmer_napi_finalizer_take,
  remove: wasmer_napi_finalizer_remove,
  retry: wasmer_napi_finalizer_retry,
};`;

// A FinalizationRegistry whose collections the test triggers.
function controlledQueue() {
  const registries = [];
  class ControlledRegistry {
    constructor(callback) {
      this.callback = callback;
      this.records = new Map();
      registries.push(this);
    }
    register(target, id, token) { this.records.set(token, {target, id}); }
    unregister(token) { return this.records.delete(token); }
    collect(target) {
      for (const [token, record] of this.records) {
        if (record.target === target) {
          this.records.delete(token);
          this.callback(record.id);
        }
      }
    }
  }
  const context = vm.createContext({FinalizationRegistry: ControlledRegistry});
  vm.runInContext(script, context);
  const {api} = context;
  // A queue of its own env, with its own registry.
  api.create = () => api.queue(api.registry());
  return {api, registries, context};
}

test('a drain takes only collected objects, oldest first', () => {
  const {api, registries} = controlledQueue();
  const queue = api.create();
  const targets = Array.from({length: 20000}, () => ({}));
  const ids = targets.map(target => api.register(queue, target));
  assert.equal(new Set(ids).size, targets.length);
  assert.ok(ids.every(id => id > 0));
  // Live objects make no work for a drain.
  for (let i = 0; i < 1000; i++) assert.equal(api.take(queue), 0);
  registries[0].collect(targets[7]);
  registries[0].collect(targets[3]);
  assert.equal(api.take(queue), ids[7]);
  assert.equal(api.take(queue), ids[3]);
  assert.equal(api.take(queue), 0);
});

test('envs are independent', () => {
  const {api, registries} = controlledQueue();
  const first = api.create(), second = api.create();
  const a = {}, b = {};
  const aID = api.register(first, a), bID = api.register(second, b);
  registries[0].collect(a);
  assert.equal(api.take(second), 0);
  assert.equal(api.take(first), aID);
  registries[1].collect(b);
  assert.equal(api.take(second), bID);
});

test("an env's two lists hear of a collection together", () => {
  const {api, registries} = controlledQueue();
  const shared = api.registry();
  const finalizers = api.queue(shared), buffers = api.queue(shared);
  const both = {}, one = {};
  const finalizerID = api.register(finalizers, both);
  const bufferID = api.register(buffers, both);
  const oneID = api.register(finalizers, one);
  assert.equal(new Set([finalizerID, bufferID, oneID]).size, 3);
  registries[0].collect(both);
  assert.equal(api.take(finalizers), finalizerID);
  assert.equal(api.take(buffers), bufferID);
  registries[0].collect(one);
  assert.equal(api.take(buffers), 0);
  assert.equal(api.take(finalizers), oneID);
});

test('a retried id is queued once', () => {
  const {api, registries} = controlledQueue();
  const queue = api.create(), target = {};
  const id = api.register(queue, target);
  registries[0].collect(target);
  assert.equal(api.take(queue), id);
  for (let i = 0; i < 10000; i++) api.retry(queue, id);
  assert.equal(api.take(queue), id);
  assert.equal(api.take(queue), 0);
});

test('remove cancels a queued or later collection', () => {
  const {api, registries} = controlledQueue();
  const queue = api.create();
  const queued = {}, live = {};
  const queuedID = api.register(queue, queued), liveID = api.register(queue, live);
  registries[0].collect(queued);
  api.remove(queue, queuedID);
  assert.equal(api.take(queue), 0, 'a queued id is dropped');
  api.remove(queue, liveID);
  registries[0].callback(liveID);
  assert.equal(api.take(queue), 0, 'a late report of a removed id is ignored');
});

test('removing from the middle, head and tail keeps the order', () => {
  const {api, registries} = controlledQueue();
  const queue = api.create();
  const targets = Array.from({length: 5}, () => ({}));
  const ids = targets.map(target => api.register(queue, target));
  for (const target of targets) registries[0].collect(target);
  api.remove(queue, ids[0]);
  api.remove(queue, ids[2]);
  api.remove(queue, ids[4]);
  assert.equal(api.take(queue), ids[1]);
  assert.equal(api.take(queue), ids[3]);
  assert.equal(api.take(queue), 0);
});

test('author code replacing intrinsics afterwards changes nothing', () => {
  const {api, registries, context} = controlledQueue();
  vm.runInContext(`
    Map.prototype.get = Map.prototype.set = Map.prototype.delete = () => { throw Error('author'); };
    FinalizationRegistry.prototype.register = () => { throw Error('author'); };
    FinalizationRegistry.prototype.unregister = () => { throw Error('author'); };`, context);
  const queue = api.create(), target = {};
  const id = api.register(queue, target);
  registries.at(-1).collect(target);
  assert.equal(api.take(queue), id);
  api.retry(queue, id);
  api.remove(queue, id);
  assert.equal(api.take(queue), 0);
});

test('the host GC reports a collected object and keeps a live one', async () => {
  const gc = globalThis.gc ?? (globalThis.Bun ? () => Bun.gc(true) : undefined);
  assert.equal(typeof gc, 'function', 'run node with --expose-gc');
  const context = vm.createContext({FinalizationRegistry: globalThis.FinalizationRegistry});
  vm.runInContext(script, context);
  const {api} = context;
  const queue = api.queue(api.registry());
  const kept = {};
  const keptID = api.register(queue, kept);
  let collectedID;
  (() => { collectedID = api.register(queue, {}); })();
  let taken = 0;
  for (let attempt = 0; attempt < 1000 && taken === 0; attempt++) {
    gc();
    await setTimeout(10);
    taken = api.take(queue);
  }
  assert.equal(taken, collectedID);
  assert.equal(api.take(queue), 0);
  api.remove(queue, keptID);
  assert.ok(kept);
});
