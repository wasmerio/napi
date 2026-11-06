import assert from 'node:assert/strict';
import test from 'node:test';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

// Test the ArrayBuffer inspection behind napi_is_detached_arraybuffer in the
// host JavaScript embedded in the wasm-bindgen module: with ES2024's
// ArrayBuffer.prototype.detached getter, and without it, as on engines that
// predate it, where a probe decides.
const rust = await readFile(new URL('../../src/snapi_js.rs', import.meta.url), 'utf8');
const inline = rust.match(/#\[wasm_bindgen\(inline_js = r#"([\s\S]*?)"#\)\]/)?.[1];
assert.ok(inline, 'host JavaScript implementation not found');
const script = inline
  .replace("import { parse as wasmerNapiParse } from 'acorn';",
    'const wasmerNapiParse = () => { throw new Error("Unexpected Acorn source preparation"); };')
  .replaceAll('export function ', 'function ');
assert.ok(!/^import /m.test(script), 'unexpected import in the host JavaScript');

// Loads the host JavaScript into a fresh realm, optionally one without the
// getter, and returns what the bridge sees: null for a value that is not an
// ArrayBuffer (napi_arraybuffer_expected), else whether it is detached.
function load(withGetter) {
  const context = vm.createContext({ setTimeout, queueMicrotask, console });
  if (!withGetter) vm.runInContext('delete ArrayBuffer.prototype.detached', context);
  vm.runInContext(script, context);
  const state = vm.runInContext('wasmer_napi_arraybuffer_detached_state', context);
  const inspect = value => {
    const result = state(value);
    assert.ok(result === -1 || result === 0 || result === 1, `state ${result}`);
    return result === -1 ? null : result === 1;
  };
  return { context, inspect };
}

function detach(buffer) {
  structuredClone(buffer, { transfer: [buffer] });
  return buffer;
}

// Buffers from the provider's realm and from this one.
function cases(context) {
  const local = source => vm.runInContext(source, context);
  const shrunk = new ArrayBuffer(8, { maxByteLength: 16 });
  shrunk.resize(0);
  return [
    ['live', local('new ArrayBuffer(16)'), false],
    ['empty', local('new ArrayBuffer(0)'), false],
    ['resizable', local('new ArrayBuffer(4, { maxByteLength: 16 })'), false],
    ['resized to empty', shrunk, false],
    ['live, another realm', new ArrayBuffer(8), false],
    ['detached', detach(local('new ArrayBuffer(16)')), true],
    ['detached empty', detach(local('new ArrayBuffer(0)')), true],
    ['detached resizable', detach(local('new ArrayBuffer(4, { maxByteLength: 16 })')), true],
    ['detached, another realm', detach(new ArrayBuffer(8)), true],
    ['SharedArrayBuffer', local('new SharedArrayBuffer(16)'), null],
    ['typed array', local('new Uint8Array(16)'), null],
    ['DataView', new DataView(new ArrayBuffer(4)), null],
    ['byteLength look-alike', { byteLength: 16 }, null],
    ['proxy of an ArrayBuffer', local('new Proxy(new ArrayBuffer(16), {})'), null],
    ['plain object', local('({})'), null],
    ['undefined', undefined, null],
    ['number', 16, null],
  ];
}

// Page code replacing, after the load, the intrinsics a naive check would
// call: with ones that throw, and with ones that accept anything.
const tamperings = {
  throw: `
    Object.defineProperty(ArrayBuffer.prototype, 'detached', { configurable: true, get() { throw new Error('detached'); } });
    Object.defineProperty(ArrayBuffer.prototype, 'byteLength', { configurable: true, get() { throw new Error('byteLength'); } });
    Function.prototype.call = function () { throw new Error('call'); };
    Function.prototype.apply = function () { throw new Error('apply'); };
    Function.prototype.bind = function () { throw new Error('bind'); };
    Reflect.apply = () => { throw new Error('Reflect.apply'); };
    globalThis.Uint8Array = function () { throw new Error('Uint8Array'); };
  `,
  'accept anything': `
    Object.defineProperty(ArrayBuffer.prototype, 'detached', { configurable: true, get() { return false; } });
    Object.defineProperty(ArrayBuffer.prototype, 'byteLength', { configurable: true, get() { return 16; } });
    Function.prototype.call = function () { return 0; };
    Function.prototype.apply = function () { return 0; };
    Function.prototype.bind = function () { return () => 0; };
    Reflect.apply = () => 0;
    globalThis.Uint8Array = function () { return {}; };
  `,
};

for (const withGetter of [true, false]) {
  const variant = withGetter ? 'with' : 'without';

  test(`detached ArrayBuffers are told apart ${variant} ArrayBuffer.prototype.detached`, () => {
    const { context, inspect } = load(withGetter);
    assert.equal(vm.runInContext("'detached' in ArrayBuffer.prototype", context),
      withGetter && 'detached' in ArrayBuffer.prototype);
    for (const [name, value, expected] of cases(context)) {
      assert.equal(inspect(value), expected, name);
    }
  });

  for (const [kind, tampering] of Object.entries(tamperings)) {
    test(`page code making intrinsics ${kind} changes nothing ${variant} ArrayBuffer.prototype.detached`, () => {
      const { context, inspect } = load(withGetter);
      const values = cases(context);
      vm.runInContext(tampering, context);
      for (const [name, value, expected] of values) {
        assert.equal(inspect(value), expected, name);
      }
    });
  }
}
