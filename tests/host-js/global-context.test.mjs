import assert from 'node:assert/strict';
import test from 'node:test';
import { setImmediate } from 'node:timers/promises';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

// Test the context implementation embedded in the wasm-bindgen module without
// requiring a full Wasmer build or loading the unrelated Acorn-based loader.
const source = await readFile(new URL('../../src/snapi_js.rs', import.meta.url), 'utf8');
const inline = source.match(/#\[wasm_bindgen\(inline_js = r#"([\s\S]*?)"#\)\]/)?.[1];
assert.ok(inline, 'host JavaScript implementation not found');
const start = inline.indexOf('const wasmerNapiHostSchedulingGlobals =');
const end = inline.indexOf('function wasmerNapiLowerDynamicImports(', start);
assert.ok(start >= 0 && end > start, 'context implementation not found');
const directory = await mkdtemp(join(tmpdir(), 'napi-context-test-'));
let runtime;
try {
  const modulePath = join(directory, 'context.mjs');
  await writeFile(modulePath, 'let wasmerNapiActiveGlobalContext;\n' + inline.slice(start, end));
  runtime = await import(pathToFileURL(modulePath));
} finally {
  await rm(directory, {recursive:true,force:true});
}

const create = runtime.wasmer_napi_create_global_context;
const evaluate = runtime.wasmer_napi_context_eval;
const callback = runtime.wasmer_napi_make_callback;
const stackFormattingSupported = (() => {
  const saved=Object.getOwnPropertyDescriptor(Error,'prepareStackTrace'),marker={};
  try {
    Object.defineProperty(Error,'prepareStackTrace',{value:()=>marker,writable:true,configurable:true});
    const holder={}; Error.captureStackTrace(holder); return holder.stack===marker;
  } finally {
    if(saved) Object.defineProperty(Error,'prepareStackTrace',saved);
    else delete Error.prepareStackTrace;
  }
})();

test('script this and globalThis refer to the virtual global', () => {
  const context = create();
  const values = evaluate(context.scope,'[this, globalThis, global]');
  assert.ok(values.every(value => value === context.scope));
});

test('global accessors retain lazy reads and context-owned writes', () => {
  let reads = 0, writes = 0;
  const original = { host:true };
  Object.defineProperty(globalThis,'__issue547HostAccessor',{
    configurable:true,get() { reads++; return original; },set() { writes++; },
  });
  try {
    const a=create(), b=create();
    assert.equal(reads,0);
    assert.equal(a.scope.__issue547HostAccessor,original);
    assert.equal(reads,1);
    assert.equal(a.scope.__issue547HostAccessor,original);
    assert.equal(reads,1);
    a.scope.__issue547HostAccessor={guest:'a'};
    assert.equal(writes,0);
    assert.equal(b.scope.__issue547HostAccessor,original);
    assert.deepEqual(a.scope.__issue547HostAccessor,{guest:'a'});
  } finally { delete globalThis.__issue547HostAccessor; }
});

test('a guest process assignment does not replace the worker process', () => {
  const original=globalThis.process;
  const a=create(), b=create();
  evaluate(a.scope,'globalThis.process={guest:"a"}');
  assert.equal(globalThis.process,original);
  assert.equal(b.scope.process,original);
  assert.deepEqual(a.scope.process,{guest:'a'});
});

test('closing an inactive context disables only its callbacks', () => {
  const a=create(), b=create();
  const fnA=callback(a,(receiver,args)=>({receiver,args}));
  const fnA2=callback(a,()=>42);
  const fnB=callback(b,()=>{throw new Error('active callback');});
  const receiver={guest:'a'};
  assert.deepEqual(fnA.call(receiver,1,2),{receiver,args:[1,2]});
  assert.equal(fnA2(),42);
  assert.throws(()=>fnB(),/active callback/);
  runtime.wasmer_napi_activate_global_context(a);
  runtime.wasmer_napi_release_global_context(b);
  assert.equal(fnB(),undefined);
  assert.equal(evaluate(undefined,'globalThis'),a.scope);
  assert.deepEqual(fnA.call(receiver,3),{receiver,args:[3]});
  runtime.wasmer_napi_release_global_context(a);
  assert.equal(fnA(),undefined);
  assert.equal(fnA2(),undefined);
});

test('retained callback wrappers do not retain closed contexts or dispatch captures', async () => {
  const gc=globalThis.gc ?? (globalThis.Bun ? ()=>Bun.gc(true) : undefined);
  const references=[], callbacks=[];
  for(let i=0;i<12;i++) {
    (() => {
      const context=create();
      const buffer=new ArrayBuffer(1024*1024);
      callbacks.push(callback(context,()=>buffer));
      runtime.wasmer_napi_release_global_context(context);
      references.push(new WeakRef(context),new WeakRef(buffer));
    })();
  }
  for(let i=0;i<8;i++) { await setImmediate(); gc(); }
  assert.ok(callbacks.every(fn=>fn()===undefined));
  assert.equal(references.filter(reference=>reference.deref()).length,0);
});

test('Error constructors, static hooks and prototype writes stay local', () => {
  const a=create(), b=create();
  const hook=Error.prepareStackTrace;
  evaluate(a.scope,'Error.prepareStackTrace=()=>"guest-a"; Error.prototype.guest="a"; Error.stackTraceLimit=3;');
  assert.equal(Error.prepareStackTrace,hook);
  assert.equal(Error.prototype.guest,undefined);
  assert.notEqual(a.scope.Error,b.scope.Error);
  assert.notEqual(a.scope.Error,Error);
  assert.equal(b.scope.Error.prototype.guest,undefined);
  assert.notEqual(b.scope.Error.stackTraceLimit,3);
  const ownMethods=Object.getOwnPropertyDescriptors(a.scope.Error.prototype);
  assert.equal(ownMethods.toString.value,Error.prototype.toString);
  assert.equal(ownMethods.name.value,'Error');
  assert.equal(ownMethods.message.value,'');
  assert.equal(evaluate(a.scope,'Error.prototype.toString.call(new Error("details"))'),'Error: details');
  assert.equal(Object.getPrototypeOf(a.scope.TypeError.prototype),a.scope.Error.prototype);
  assert.equal(evaluate(a.scope,'new TypeError("typed").guest'),'a');
  assert.equal(evaluate(b.scope,'new TypeError("typed").guest'),undefined);
});

test('callable errors, subclasses, causes and native N-API errors work', () => {
  const c=create();
  const values=evaluate(c.scope,`class CustomError extends Error {}; [Error('called'),new TypeError('typed',{cause:42}),new AggregateError([1,2],'many'),new CustomError('custom')]`);
  for (const value of values) assert.ok(value instanceof c.scope.Error);
  assert.equal(values[0].message,'called');
  assert.ok(values[1] instanceof c.scope.TypeError);
  assert.equal(values[1].constructor,c.scope.TypeError);
  assert.equal(values[1].cause,42);
  assert.deepEqual(values[2].errors,[1,2]);
  assert.equal(values[3].constructor.name,'CustomError');
  assert.equal(values[0] instanceof c.scope.TypeError,false);
  assert.equal(values[0] instanceof values[3].constructor,false);
  assert.equal(values[3] instanceof values[3].constructor,true);
  assert.equal(c.scope.Error.prototype instanceof c.scope.Error,false);
  assert.ok(new Error('native') instanceof c.scope.Error);
});

test('custom stack formatting is lazy, receives the actual error and restores the host hook', {skip:!stackFormattingSupported}, () => {
  const c=create(), host=Object.getOwnPropertyDescriptor(Error,'prepareStackTrace');
  c.scope.state={calls:0};
  const error=evaluate(c.scope,`Error.prepareStackTrace=(error,frames)=>{state.calls++;return {error,frames}}; new Error('custom',{cause:42})`);
  assert.equal(c.scope.state.calls,0);
  const stack=error.stack;
  assert.equal(stack.error,error);
  assert.ok(stack.frames.length>0);
  assert.equal(c.scope.state.calls,1);
  assert.equal(error.stack,stack);
  assert.deepEqual(Object.getOwnPropertyDescriptor(Error,'prepareStackTrace'),host);
  evaluate(c.scope,'Error.prepareStackTrace=()=>{throw new RangeError("formatter failed")}');
  const bad=evaluate(c.scope,'new Error("bad")');
  assert.throws(()=>bad.stack,/formatter failed/);
  assert.deepEqual(Object.getOwnPropertyDescriptor(Error,'prepareStackTrace'),host);
});

test('Error.captureStackTrace targets use the local formatter', {skip:!stackFormattingSupported}, () => {
  const c=create();
  const object=evaluate(c.scope,`Error.prepareStackTrace=(object,frames)=>({object,frames}); const target={}; Error.captureStackTrace(target); target`);
  assert.equal(object.stack.object,object);
  assert.ok(object.stack.frames.length>0);
  object.stack='overridden';
  assert.equal(object.stack,'overridden');
});

test('a stack shim installed after module loading keeps its target and local formatter', () => {
  const saved=Object.getOwnPropertyDescriptor(Error,'captureStackTrace');
  const frame={getFileName:()=>'/stack-shim.js'};
  Object.defineProperty(Error,'captureStackTrace',{
    configurable:true,writable:true,value(target) {
      let formatted=false, value;
      Object.defineProperty(target,'stack',{
        configurable:true,get() {
          if (!formatted) {
            value=typeof Error.prepareStackTrace==='function'
              ? Error.prepareStackTrace(target,[frame]) : 'shim stack';
            formatted=true;
          }
          return value;
        },
      });
    },
  });
  try {
    const context=create();
    context.scope.Error.prepareStackTrace=(error,frames)=>({error,frames});
    const error=new context.scope.Error('shim');
    assert.equal(error.stack.error,error);
    assert.equal(error.stack.frames[0].getFileName(),'/stack-shim.js');
    const target={}; context.scope.Error.captureStackTrace(target);
    assert.equal(target.stack.error,target);
    assert.equal(target.stack.frames[0].getFileName(),'/stack-shim.js');
  } finally {
    if(saved) Object.defineProperty(Error,'captureStackTrace',saved);
    else delete Error.captureStackTrace;
  }
});

test('delayed stacks keep their owner and capture its frame limit', {skip:!stackFormattingSupported}, () => {
  const a=create(), b=create();
  const host=Object.getOwnPropertyDescriptor(Error,'stackTraceLimit');
  a.scope.Error.stackTraceLimit=2;
  b.scope.Error.stackTraceLimit=4;
  a.scope.Error.prepareStackTrace=(error,frames)=>({owner:'a',error,frames});
  b.scope.Error.prepareStackTrace=(error,frames)=>({owner:'b',error,frames});
  const source='function first(){return second()} function second(){return third()} function third(){return new TypeError("delayed")} first()';
  const errorA=evaluate(a.scope,source), errorB=evaluate(b.scope,source);
  const stackB=errorB.stack, stackA=errorA.stack;
  assert.equal(stackA.owner,'a');
  assert.equal(stackB.owner,'b');
  assert.equal(stackA.error,errorA);
  assert.equal(stackB.error,errorB);
  assert.equal(stackA.frames.length,2);
  assert.equal(stackB.frames.length,4);
  assert.deepEqual(Object.getOwnPropertyDescriptor(Error,'stackTraceLimit'),host);
});

test('closed virtual contexts and their guest buffers are collectible', async () => {
  const gc=globalThis.gc ?? (globalThis.Bun ? ()=>Bun.gc(true) : undefined);
  assert.equal(typeof gc,'function','run with --expose-gc');
  const references=[];
  for(let i=0;i<12;i++) {
    (() => {
      const context=create();
      const memory=new WebAssembly.Memory({initial:32,maximum:64,shared:true});
      context.scope.buffer=new Int32Array(memory.buffer);
      evaluate(context.scope,'process={buffer}; Error.prepareStackTrace=()=>process.buffer;');
      runtime.wasmer_napi_activate_global_context(context);
      runtime.wasmer_napi_release_global_context(context);
      references.push(new WeakRef(memory.buffer));
    })();
  }
  for(let i=0;i<8;i++) { await setImmediate(); gc(); }
  assert.equal(references.filter(reference=>reference.deref()).length,0);
});
