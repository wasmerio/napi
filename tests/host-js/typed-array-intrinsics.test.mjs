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


// napi_get_typedarray_info, napi_get_dataview_info and napi_get_buffer_info
// read a view's layout from its internal slots: a view's own `length` (a
// Buffer may have one) or a replaced prototype getter must not change what
// native code is told, or run.
test('view layouts come from internal slots', () => {
  const context = load();
  const local = source => vm.runInContext(source, context);
  local(`
    globalThis.calls = 0;
    globalThis.buffer = new ArrayBuffer(16);
    new Uint8Array(buffer).forEach((_, i, a) => { a[i] = i; });
    globalThis.bytes = new Uint8Array(buffer, 4, 8);
    globalThis.view = new DataView(buffer, 2, 6);
    for (const target of [bytes, view]) {
      for (const key of ['length', 'byteOffset', 'byteLength', 'buffer']) {
        Object.defineProperty(target, key, { get() { calls++; return 999; } });
      }
    }
    for (const proto of [Object.getPrototypeOf(Uint8Array.prototype), DataView.prototype]) {
      for (const key of ['length', 'byteOffset', 'byteLength', 'buffer']) {
        if (Object.getOwnPropertyDescriptor(proto, key)) {
          Object.defineProperty(proto, key, { get() { calls++; return 999; } });
        }
      }
    }
    Object.defineProperty(Object.getPrototypeOf(Uint8Array.prototype), Symbol.toStringTag,
      { get() { calls++; return 'Float64Array'; } });
    Function.prototype.call = function () { calls++; throw new Error('call'); };
  `);
  const buffer = local('buffer');
  const typedInfo = local('wasmer_napi_typed_array_info');
  const viewInfo = local('wasmer_napi_dataview_info');
  const byteView = local('wasmer_napi_byte_view');

  const [kind, ownBuffer, length, byteOffset, byteLength] = typedInfo(local('bytes'));
  assert.deepEqual([kind, ownBuffer === buffer, length, byteOffset, byteLength],
    [NAPI_UINT8_ARRAY, true, 8, 4, 8]);
  const [viewBuffer, viewOffset, viewLength] = viewInfo(local('view'));
  assert.deepEqual([viewBuffer === buffer, viewOffset, viewLength], [true, 2, 6]);
  assert.deepEqual([...byteView(local('bytes'))], [4, 5, 6, 7, 8, 9, 10, 11]);
  assert.deepEqual([...byteView(local('view'))], [2, 3, 4, 5, 6, 7]);
  // This realm's getter: the context's own is the script's now.
  const lengthOf = view => Reflect.apply(
    Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), 'length').get,
    view, []);
  assert.equal(lengthOf(byteView(buffer)), 16);
  assert.equal(local('calls'), 0, 'a script-provided getter or call ran');
});

test('views of another realm, detached views and non-views', () => {
  const context = load();
  const local = source => vm.runInContext(source, context);
  const typedInfo = local('wasmer_napi_typed_array_info');
  const viewInfo = local('wasmer_napi_dataview_info');
  const kindOf = local('wasmer_napi_typed_array_kind');
  const isDataView = local('wasmer_napi_is_dataview');
  const byteView = local('wasmer_napi_byte_view');

  // This test's own realm is another one for the glue.
  const doubles = new Float64Array(new ArrayBuffer(32), 8, 2);
  assert.deepEqual(Array.from(typedInfo(doubles).slice(2)), [2, 8, 16]);
  assert.equal(typedInfo(doubles)[0], NAPI_FLOAT64_ARRAY);
  const foreignView = new DataView(new ArrayBuffer(8), 1, 3);
  assert.deepEqual(Array.from(viewInfo(foreignView).slice(1)), [1, 3]);

  // A DataView whose buffer is detached is still one, of no bytes.
  const detachedBuffer = new ArrayBuffer(8);
  const detached = new DataView(detachedBuffer, 2, 4);
  const detachedBytes = new Uint8Array(detachedBuffer, 2, 4);
  if (typeof detachedBuffer.transfer === 'function') {
    detachedBuffer.transfer();
    assert.equal(isDataView(detached), true);
    assert.deepEqual(Array.from(viewInfo(detached).slice(1)), [0, 0]);
    assert.deepEqual(Array.from(typedInfo(detachedBytes).slice(2)), [0, 0, 0]);
    assert.equal(byteView(detached).length, 0);
    assert.equal(byteView(detachedBytes).length, 0);
  }

  for (const other of [{}, [], null, 1, 'x', new ArrayBuffer(4),
    new Proxy(new Uint8Array(4), {})]) {
    assert.equal(kindOf(other), -1);
    assert.equal(typedInfo(other), null);
    assert.equal(viewInfo(other), null);
  }
  assert.equal(viewInfo(new Uint8Array(4)), null);
  assert.equal(typedInfo(new DataView(new ArrayBuffer(4))), null);
  assert.equal(byteView({}), null);
  assert.equal(byteView(new SharedArrayBuffer(4)).length, 4);
});
