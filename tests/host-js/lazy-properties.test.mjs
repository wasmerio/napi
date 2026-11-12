import assert from 'node:assert/strict';
import test from 'node:test';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

// Exercise the actual wasm-bindgen host helpers in independent realms, without
// requiring a Wasm build. Bridge/status tests separately cover the Rust ABI.
const rust = await readFile(new URL('../../src/snapi_js.rs', import.meta.url), 'utf8');
const inline = rust.match(/#\[wasm_bindgen\(inline_js = r#"([\s\S]*?)"#\)\]/)?.[1];
assert.ok(inline, 'host JavaScript implementation not found');
const script = inline
  .replace("import { parse as wasmerNapiParse } from 'acorn';",
    'const wasmerNapiParse = () => { throw new Error("Unexpected Acorn source preparation"); };')
  .replaceAll('export function ', 'function ');
assert.ok(!/^import /m.test(script), 'unexpected import in the host JavaScript');

function load() {
  const context = vm.createContext({ setTimeout, queueMicrotask, console });
  vm.runInContext(script, context);
  return {
    context,
    define: vm.runInContext('wasmer_napi_define_lazy_data_properties', context),
    lazy: vm.runInContext('wasmer_napi_is_lazy_data_property', context),
  };
}
const errorNamed = name => error => error?.name === name;

test('configurable properties initialize once, in place, and only on a value read', () => {
  const { define, lazy } = load();
  const holder = { first: 1 };
  const symbol = Symbol('lazy');
  let calls = 0;
  assert.equal(define(holder, ['value', symbol], function (key) {
    assert.equal(this, holder);
    calls++;
    return key;
  }, 7), 0);
  holder.last = 2;
  assert.equal(lazy(holder, 'value'), true);
  assert.deepEqual(Object.keys(holder), ['first', 'value', 'last']);
  assert.equal('value' in holder, true);
  const descriptor = Object.getOwnPropertyDescriptor(holder, 'value');
  assert.equal(typeof descriptor.get, 'function'); // Portable descriptor limit.
  assert.equal(calls, 0);
  assert.equal(holder.value, 'value');
  assert.equal(holder.value, 'value');
  assert.equal(lazy(holder, 'value'), false);
  assert.deepEqual(Object.getOwnPropertyDescriptor(holder, 'value'), {
    value: 'value', writable: true, enumerable: true, configurable: true,
  });
  assert.equal(holder[symbol], symbol);
  assert.equal(calls, 2);
  assert.deepEqual(Reflect.ownKeys(holder), ['first', 'value', 'last', symbol]);
});

test('every attribute combination preserves flags and memoizes locked accessors', () => {
  const { define, lazy } = load();
  for (let attributes = 0; attributes < 8; attributes++) {
    const holder = {};
    let calls = 0;
    assert.equal(define(holder, ['value'], () => ++calls, attributes), 0);
    let descriptor = Object.getOwnPropertyDescriptor(holder, 'value');
    assert.equal(descriptor.enumerable, Boolean(attributes & 2));
    assert.equal(descriptor.configurable, Boolean(attributes & 4));
    assert.equal(typeof descriptor.set, attributes & 1 ? 'function' : 'undefined');
    assert.equal(calls, 0);
    assert.equal(holder.value, 1);
    assert.equal(holder.value, 1);
    assert.equal(calls, 1);
    assert.equal(lazy(holder, 'value'), false);
    descriptor = Object.getOwnPropertyDescriptor(holder, 'value');
    if (attributes & 4) assert.equal(descriptor.writable, Boolean(attributes & 1));
    else assert.equal(typeof descriptor.get, 'function');
  }
});

test('writes and deletes avoid initialization; inherited writes shadow the holder', () => {
  const { define, lazy } = load();
  let calls = 0;
  const holder = {};
  assert.equal(define(holder, ['write', 'remove', 'inherited'], () => ++calls, 7), 0);
  holder.write = 42;
  delete holder.remove;
  const child = Object.create(holder);
  child.inherited = 24;
  assert.equal(calls, 0);
  assert.equal(lazy(holder, 'write'), false);
  assert.equal(lazy(holder, 'remove'), false);
  assert.equal(lazy(holder, 'inherited'), true);
  assert.deepEqual(Object.getOwnPropertyDescriptor(child, 'inherited'), {
    value: 24, writable: true, enumerable: true, configurable: true,
  });
  assert.equal(lazy(child, 'inherited'), false);
  assert.equal(holder.inherited, 1);
});

test('inherited Reflect.set preserves receiver data flags and refuses accessor or readonly receivers', () => {
  const { define, lazy } = load();
  const holder = {};
  let calls = 0;
  define(holder, ['value'], () => ++calls, 7);
  for (const configurable of [false, true]) {
    const receiver = Object.defineProperty({}, 'value', {
      value: 1, writable: true, enumerable: false, configurable,
    });
    assert.equal(Reflect.set(holder, 'value', 42, receiver), true);
    assert.deepEqual(Object.getOwnPropertyDescriptor(receiver, 'value'), {
      value: 42, writable: true, enumerable: false, configurable,
    });
  }
  const absent = {};
  assert.equal(Reflect.set(holder, 'value', 24, absent), true);
  assert.deepEqual(Object.getOwnPropertyDescriptor(absent, 'value'), {
    value: 24, writable: true, enumerable: true, configurable: true,
  });
  let setterCalls = 0;
  const accessor = Object.defineProperty({}, 'value', { set() { setterCalls++; } });
  const readonly = Object.defineProperty({}, 'value', { value: 9, writable: false });
  // The portable accessor reports refused writes by throwing, even when
  // Reflect.set would return false for an ordinary inherited data property.
  for (const receiver of [accessor, readonly]) {
    assert.throws(() => Reflect.set(holder, 'value', 42, receiver), errorNamed('TypeError'));
  }
  assert.equal(setterCalls, 0);
  assert.equal(readonly.value, 9);
  assert.equal(calls, 0);
  assert.equal(lazy(holder, 'value'), true);
});

test('locked writable setters cache values; freeze cannot disable an accessor setter', () => {
  const { define, lazy } = load();
  for (const lock of [Object.seal, Object.freeze]) {
    const holder = {};
    let calls = 0;
    assert.equal(define(holder, ['value'], () => ++calls, 7), 0);
    lock(holder);
    holder.value = 42; // Documented portable freeze difference.
    assert.equal(holder.value, 42);
    assert.equal(calls, 0);
    assert.equal(lazy(holder, 'value'), false);
    assert.equal(typeof Object.getOwnPropertyDescriptor(holder, 'value').get, 'function');
  }
  const readonly = {};
  assert.equal(define(readonly, ['value'], () => 9, 0), 0);
  assert.throws(() => { readonly.value = 4; }, TypeError);
  assert.equal(readonly.value, 9);
});

test('sealed and frozen uninitialized accessors still initialize only once', () => {
  const { define, lazy } = load();
  for (const lock of [Object.seal, Object.freeze]) {
    const holder = {};
    let calls = 0;
    define(holder, ['value'], () => ++calls, 7);
    lock(holder);
    assert.equal(lazy(holder, 'value'), true);
    assert.equal(calls, 0);
    assert.equal(holder.value, 1);
    assert.equal(holder.value, 1);
    assert.equal(calls, 1);
    assert.equal(lazy(holder, 'value'), false);
  }
});

test('initializer exceptions retry; recursion is rejected without poisoning a property', () => {
  const { define, lazy } = load();
  const holder = {};
  const failure = new Error('retry');
  let calls = 0;
  define(holder, ['value'], () => { if (++calls === 1) throw failure; return 12; }, 7);
  assert.throws(() => holder.value, error => error === failure);
  assert.equal(lazy(holder, 'value'), true);
  assert.equal(holder.value, 12);
  define(holder, ['recursive'], () => holder.recursive, 7);
  assert.throws(() => holder.recursive, errorNamed('RangeError'));
  assert.equal(lazy(holder, 'recursive'), true);
});

test('initialization detects replacement, deletion and writes while preserving mutations', () => {
  const { define, lazy } = load();
  for (const mutate of [
    holder => { holder.value = 'assigned'; },
    holder => { delete holder.value; },
    holder => { Object.defineProperty(holder, 'value', { value: 'replaced' }); },
  ]) {
    const holder = {};
    define(holder, ['value'], () => { mutate(holder); return 'initializer'; }, 7);
    assert.throws(() => holder.value, errorNamed('TypeError'));
    assert.equal(lazy(holder, 'value'), false);
    assert.notEqual(holder.value, 'initializer');
  }
});

test('assignment inside a locked initializer preserves the write and rejects its result', () => {
  const { define, lazy } = load();
  const holder = {};
  define(holder, ['value'], () => { holder.value = 'assigned'; return 'initializer'; }, 1);
  const before = Object.getOwnPropertyDescriptor(holder, 'value');
  assert.throws(() => holder.value, errorNamed('TypeError'));
  assert.deepEqual(Object.getOwnPropertyDescriptor(holder, 'value'), before);
  assert.equal(holder.value, 'assigned');
  assert.equal(lazy(holder, 'value'), false);
});

test('filtered names use lazy attributes without reading values, including locked caches', () => {
  const { context, define, lazy } = load();
  const names = vm.runInContext('wasmer_napi_get_all_property_names', context);
  const holder = {};
  let calls = 0;
  for (let flags = 0; flags < 8; flags++) define(holder, [`p${flags}`], () => ++calls, flags);
  const expected = { 1: ['p1', 'p3', 'p5', 'p7'], 2: ['p2', 'p3', 'p6', 'p7'], 4: ['p4', 'p5', 'p6', 'p7'] };
  for (const filter of [1, 2, 4]) assert.deepEqual(Array.from(names(holder, 1, filter, 1)), expected[filter]);
  assert.equal(calls, 0);
  assert.equal(holder.p3, 1);
  assert.equal(lazy(holder, 'p3'), false);
  for (const filter of [1, 2, 4]) assert.deepEqual(Array.from(names(holder, 1, filter, 1)), expected[filter]);
  assert.equal(calls, 1);
});

test('redefining a running property cannot bypass holder/name recursion checks', () => {
  const { define, lazy } = load();
  const holder = {};
  define(holder, ['value'], () => {
    assert.equal(define(holder, ['value'], () => 23, 7), 0);
    return holder.value;
  }, 7);
  assert.throws(() => holder.value, errorNamed('RangeError'));
  assert.equal(lazy(holder, 'value'), true);
  assert.equal(holder.value, 23);
});

test('validation checks every name before definitions and reports refused definitions', () => {
  const { define, lazy } = load();
  const holder = {};
  assert.equal(define(holder, [], () => 1, 8), 1);
  assert.equal(define(null, [], () => 1, 7), 2);
  assert.equal(define(holder, {}, () => 1, 7), 8);
  assert.equal(define(holder, [], null, 7), 5);
  for (const name of [undefined, null, 4, {}, '0', '4294967294']) {
    assert.equal(define(holder, ['untouched', name], () => 1, 7), 4);
    assert.equal(Object.hasOwn(holder, 'untouched'), false);
  }
  assert.equal(define(holder, ['4294967295', '01', '-0'], () => 1, 7), 0);
  Object.defineProperty(holder, 'blocked', { value: 1 });
  assert.equal(define(holder, ['partial', 'blocked'], () => 1, 7), 9);
  assert.equal(lazy(holder, 'partial'), true);
  assert.equal(define(Object.preventExtensions({}), ['value'], () => 1, 7), 9);
  for (const key of ['-0', 'NaN', 'Infinity', '-1', '0.5', '4294967295']) {
    assert.equal(define(new Uint8Array(1), [key], () => 1, 7), 4, key);
  }
});

test('name access and definition proxy exceptions propagate unchanged', () => {
  const { define } = load();
  const failure = new Error('trap');
  const names = new Proxy(['value'], { get() { throw failure; } });
  assert.throws(() => define({}, names, () => 1, 7), error => error === failure);
  const holder = new Proxy({}, { defineProperty() { throw failure; } });
  assert.throws(() => define(holder, ['value'], () => 1, 7), error => error === failure);
  assert.equal(define(new Proxy({}, { defineProperty() { return false; } }), ['value'], () => 1, 7), 9);
});

test('query ignores inherited and unrelated accessors and untracked proxies without traps', () => {
  const { define, lazy } = load();
  const holder = {};
  define(holder, ['value'], () => 1, 7);
  assert.equal(lazy(Object.create(holder), 'value'), false);
  const getter = Object.getOwnPropertyDescriptor(holder, 'value').get;
  const copied = Object.defineProperty({}, 'value', { get: getter });
  assert.equal(lazy(copied, 'value'), false);
  const proxy = new Proxy(holder, {
    get() { throw new Error('get trap'); },
    getOwnPropertyDescriptor() { throw new Error('descriptor trap'); },
  });
  assert.equal(lazy(proxy, 'value'), false);
  const revoked = Proxy.revocable({}, {});
  revoked.revoke();
  assert.equal(lazy(revoked.proxy, 'value'), false);
});

test('tracked proxy queries observe descriptor traps without invoking initializers', () => {
  const { define, lazy } = load();
  let calls = 0, descriptors = 0;
  const proxy = new Proxy({}, {
    getOwnPropertyDescriptor(target, key) { descriptors++; return Reflect.getOwnPropertyDescriptor(target, key); },
  });
  assert.equal(define(proxy, ['value'], () => ++calls, 7), 0);
  const before = descriptors;
  assert.equal(lazy(proxy, 'value'), true);
  assert.ok(descriptors > before);
  assert.equal(calls, 0);
  assert.equal(proxy.value, 1);
  assert.equal(lazy(proxy, 'value'), false);
});

test('inherited reads call the initializer in its own realm with the holder', () => {
  const { define } = load();
  const initializer = vm.runInNewContext('(function (key) { return [this, key, Array.prototype]; })');
  const holder = {};
  define(holder, ['value'], initializer, 7);
  const child = Object.create(holder);
  const result = child.value;
  assert.equal(result[0], holder);
  assert.equal(result[1], 'value');
  assert.equal(Object.getPrototypeOf(result), result[2]);
  assert.notEqual(result[2], Array.prototype);
  assert.equal(holder.value, result);
  assert.equal(Object.hasOwn(child, 'value'), false);
});

test('captured intrinsics resist page tampering after module initialization', () => {
  const { context, define, lazy } = load();
  vm.runInContext(`
    const fail = () => { throw new Error('tampered intrinsic'); };
    Object.defineProperty = Object.getOwnPropertyDescriptor = Object.getPrototypeOf = fail;
    Reflect.defineProperty = Reflect.getOwnPropertyDescriptor = Reflect.apply = Reflect.ownKeys = fail;
    WeakMap.prototype.get = WeakMap.prototype.set = WeakMap.prototype.has = WeakMap.prototype.delete = fail;
    WeakSet.prototype.has = WeakSet.prototype.add = fail;
    globalThis.Number = globalThis.String = fail;
    Array.isArray = fail;
    Function.prototype.call = Function.prototype.apply = Function.prototype.bind = fail;
  `, context);
  const holder = {};
  Object.defineProperty(holder, Symbol.toStringTag, { get() { throw new Error('tag getter'); } });
  let calls = 0;
  assert.equal(define(holder, ['0'], () => 0, 7), 4);
  assert.equal(define(holder, ['value'], () => ++calls, 7), 0);
  assert.equal(lazy(holder, 'value'), true);
  assert.equal(holder.value, 1);
  assert.equal(lazy(holder, 'value'), false);
  assert.equal(calls, 1);
});

test('deletion and materialization release initializer realms while holders and saved getters live', async t => {
  const gc = globalThis.gc ?? (globalThis.Bun ? () => Bun.gc(true) : undefined);
  if (!gc) return t.skip('run Node with --expose-gc or Bun with GC support');
  const { define } = load();
  const references = [], kept = [];
  for (const action of ['delete', 'initialize', 'assign']) {
    (() => {
      const holder = {};
      const initializer = vm.runInNewContext('(function (key) { return key === "probe" ? globalThis : 17; })');
      references.push(new WeakRef(initializer), new WeakRef(initializer('probe')));
      define(holder, ['value'], initializer, 7);
      if (action === 'delete') delete holder.value;
      else {
        kept.push(Object.getOwnPropertyDescriptor(holder, 'value').get);
        if (action === 'initialize') assert.equal(holder.value, 17);
        else holder.value = 17;
      }
      kept.push(holder);
    })();
  }
  // Bun's vm implementation retains the most recently evaluated realm even
  // without any lazy properties. Replace that cache with an unrelated realm.
  vm.runInNewContext('0');
  // WeakRef targets survive their creation job; allow bounded task turns for
  // the collector and any compilation work to release temporary references.
  for (let attempt = 0; attempt < 80; attempt++) {
    await new Promise(resolve => setTimeout(resolve, 0));
    gc();
    if (references.every(reference => reference.deref() === undefined)) break;
  }
  assert.deepEqual(references.flatMap((reference, index) => reference.deref() === undefined ? [] : [index]), []);
  assert.equal(kept.length, 5);
  assert.equal(kept.filter(value => typeof value === 'function').every(getter => getter() === 17), true);
});
