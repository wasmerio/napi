import assert from 'node:assert/strict';
import test from 'node:test';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

// napi_create_typedarray and the guest-backed views behind
// unofficial_napi_create_uint8array make typed arrays through
// wasmer_napi_typed_array in the host JavaScript embedded in the wasm-bindgen
// module. It must use the intrinsic constructors: a script that replaces
// globalThis.Uint8Array, or makes it a getter, must not run there or choose
// the object returned.
const rust = await readFile(new URL('../../src/snapi_js.rs', import.meta.url), 'utf8');
const inline = rust.match(/#\[wasm_bindgen\(inline_js = r#"([\s\S]*?)"#\)\]/)?.[1];
assert.ok(inline, 'host JavaScript implementation not found');
const script = inline
  .replace("import { parse as wasmerNapiParse } from 'acorn';",
    'const wasmerNapiParse = () => { throw new Error("Unexpected Acorn source preparation"); };')
  .replaceAll('export function ', 'function ');
assert.ok(!/^import /m.test(script), 'unexpected import in the host JavaScript');

const NAPI_UINT8_ARRAY = 1;
const NAPI_FLOAT64_ARRAY = 8;

function load() {
  const context = vm.createContext({ setTimeout, queueMicrotask, console });
  vm.runInContext(script, context);
  return context;
}

test('typed arrays come from the intrinsic constructors, not the global ones', () => {
  const context = load();
  const local = source => vm.runInContext(source, context);
  const intrinsicUint8Array = local('Uint8Array');
  const intrinsicFloat64Array = local('Float64Array');
  local(`
    globalThis.calls = 0;
    globalThis.Uint8Array = function Hijacked() { calls++; return { hijacked: true }; };
    Object.defineProperty(globalThis, 'Float64Array', {
      configurable: true,
      get() { calls++; throw new Error('Float64Array read'); },
    });
  `);
  const make = local('wasmer_napi_typed_array');
  const buffer = local('new ArrayBuffer(16)');

  const bytes = make(NAPI_UINT8_ARRAY, buffer, 4, 8);
  assert.equal(Object.getPrototypeOf(bytes), intrinsicUint8Array.prototype);
  assert.equal(bytes.length, 8);
  assert.equal(bytes.byteOffset, 4);
  assert.equal(bytes.buffer, buffer);

  const doubles = make(NAPI_FLOAT64_ARRAY, buffer, 0, 2);
  assert.equal(Object.getPrototypeOf(doubles), intrinsicFloat64Array.prototype);
  assert.equal(doubles.length, 2);

  assert.equal(local('calls'), 0, 'a replaced global constructor ran');
  assert.throws(() => make(99, buffer, 0, 1), /unsupported typed array kind/);
});

test('copies own their bytes and use the intrinsics', () => {
  const context = load();
  const local = source => vm.runInContext(source, context);
  const intrinsicUint8Array = local('Uint8Array');
  local(`
    globalThis.calls = 0;
    const set = Object.getPrototypeOf(Uint8Array.prototype).set;
    Object.getPrototypeOf(Uint8Array.prototype).set = function () {
      calls++;
      return Reflect.apply(set, this, arguments);
    };
    globalThis.Uint8Array = function Hijacked() { calls++; return {}; };
  `);
  const copyOf = local('wasmer_napi_uint8array_copy');
  const memory = new intrinsicUint8Array(16).map((_, i) => i);
  const copy = copyOf(memory.buffer, 4, 6);
  assert.equal(Object.getPrototypeOf(copy), intrinsicUint8Array.prototype);
  assert.equal(copy.buffer.byteLength, 6, 'the copy has a buffer of its own');
  assert.notEqual(copy.buffer, memory.buffer);
  assert.deepEqual([...copy], [4, 5, 6, 7, 8, 9]);
  memory[4] = 99;
  assert.equal(copy[0], 4);
  assert.equal(local('calls'), 0, 'a replaced constructor or set ran');
});

