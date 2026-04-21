#ifndef NAPI_QUICKJS_UPSTREAM_JS_TEST_H_
#define NAPI_QUICKJS_UPSTREAM_JS_TEST_H_

#include <fstream>
#include <sstream>
#include <string>
#include "test_env.h"

inline std::string ReadTextFile(const std::string &path)
{
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline bool RunScript(EnvScope &s, const std::string &source_text, const char *label)
{
  // 1. Evaluate the script
  JSValue val = JS_Eval(s.ctx, source_text.c_str(), source_text.size(), label, JS_EVAL_TYPE_GLOBAL);

  // 2. Handle exceptions
  if (JS_IsException(val))
  {
    JSValue exc = JS_GetException(s.ctx);
    const char *msg = JS_ToCString(s.ctx, exc);
    ADD_FAILURE() << "JS exception (" << label << "): " << (msg ? msg : "<empty>");
    JS_FreeCString(s.ctx, msg);
    JS_FreeValue(s.ctx, exc);
    return false;
  }
  JS_FreeValue(s.ctx, val);

  // 3. Perform Microtask Checkpoint (execute Promises/jobs)
  JSContext *ctx1;
  while (JS_ExecutePendingJob(JS_GetRuntime(s.ctx), &ctx1) > 0)
    ;

  return true;
}

// Global GC callback for QuickJS
inline JSValue ForceGcCallback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  JS_RunGC(JS_GetRuntime(ctx));
  return JS_UNDEFINED;
}

inline bool InstallUpstreamJsShim(EnvScope &s, napi_value addon_exports)
{
  // 1. Set the addon exports on the global object for 'require' to find
  napi_value global = nullptr;
  if (napi_get_global(s.env, &global) != napi_ok)
    return false;
  if (napi_set_named_property(s.env, global, "__napi_test_addon", addon_exports) != napi_ok)
  {
    return false;
  }

  // 2. Register the manual GC function
  JSValue global_obj = JS_GetGlobalObject(s.ctx);
  JS_SetPropertyStr(s.ctx, global_obj, "__napi_force_gc",
                    JS_NewCFunction(s.ctx, ForceGcCallback, "__napi_force_gc", 0));
  JS_FreeValue(s.ctx, global_obj);

  // 3. Define the shim script (already provided in your snippet)
  const char *shim = R"JS(
(() => {
  'use strict';
  const __mustCallRecords = [];
  function __deepEqual(a, b) {
    if (Object.is(a, b)) return true;
    if (typeof a !== typeof b) return false;
    if (a === null || b === null) return a === b;
    if (typeof a !== 'object') return false;
    if (Array.isArray(a) !== Array.isArray(b)) return false;
    const aKeys = Reflect.ownKeys(a);
    const bKeys = Reflect.ownKeys(b);
    if (aKeys.length !== bKeys.length) return false;
    for (const k of aKeys) {
      if (!bKeys.includes(k)) return false;
      if (!__deepEqual(a[k], b[k])) return false;
    }
    return true;
  }

  globalThis.common = {
    buildType: 'Debug',
    mustCall(fn, expected = 1) {
      if (typeof fn !== 'function') fn = () => {};
      const rec = { called: 0, expected };
      __mustCallRecords.push(rec);
      return function(...args) {
        rec.called++;
        return fn.apply(this, args);
      };
    },
    mustNotCall(message) {
      return function() {
        throw new Error(message || 'mustNotCall');
      };
    }
  };

  function __assert(value, message) {
    if (!value) throw new Error(message || 'assert failed');
  }
  __assert.strictEqual = function(actual, expected, message) {
    if (!Object.is(actual, expected)) throw new Error(message || `strictEqual: ${actual} !== ${expected}`);
  };
  __assert.deepStrictEqual = function(actual, expected, message) {
    if (!__deepEqual(actual, expected)) {
      throw new Error(message || 'deepStrictEqual failed');
    }
  };
  __assert.notStrictEqual = function(actual, expected, message) {
    if (Object.is(actual, expected)) throw new Error(message || `notStrictEqual: ${actual} === ${expected}`);
  };
  __assert.throws = function(fn, expected) {
    let threw = false;
    let err;
    try {
      fn();
    } catch (e) {
      threw = true;
      err = e;
    }
    if (!threw) throw new Error('assert.throws failed: no throw');
    if (expected === undefined) return;
    if (expected instanceof RegExp) {
      if (!expected.test(String(err))) {
        throw new Error(`assert.throws regex mismatch: ${String(err)}`);
      }
      return;
    }
    if (typeof expected === 'function') {
      // Covers both predicate functions and error constructors.
      if (expected.prototype && err instanceof expected) return;
      const ok = expected(err);
      if (ok !== true) throw new Error('assert.throws predicate mismatch');
      return;
    }
    if (typeof expected === 'object' && expected !== null) {
      for (const key of Object.keys(expected)) {
        if (!Object.is(err?.[key], expected[key])) {
          throw new Error(`assert.throws object mismatch on ${key}`);
        }
      }
      return;
    }
  };
  __assert.ok = function(value, message) {
    if (!value) throw new Error(message || 'assert.ok failed');
  };
  globalThis.assert = __assert;

  globalThis.require = function(spec) {
    if (spec === '../../common') return globalThis.common;
    if (spec === '../../common/gc') {
      return {
        gcUntil: async function(name, predicate) {
          for (let i = 0; i < 256; i++) {
            if (predicate()) return;
            __napi_force_gc();
          }
          throw new Error(`gcUntil timeout: ${name}`);
        }
      };
    }
    if (spec === 'assert') return globalThis.assert;
    if (spec.startsWith('./build/')) {
      if (globalThis.__napi_test_require_exception) {
        const err = globalThis.__napi_test_require_exception;
        globalThis.__napi_test_require_exception = null;
        throw err;
      }
      return globalThis.__napi_test_addon;
    }
    throw new Error(`Unsupported require: ${spec}`);
  };
  globalThis.require.main = {};
  globalThis.module = {};
  globalThis.global = globalThis;
  globalThis.gc = globalThis.__napi_force_gc;

  globalThis.__napi_verify_must_call = function() {
    for (const rec of __mustCallRecords) {
      if (rec.called !== rec.expected) {
        throw new Error(`mustCall mismatch: called=${rec.called}, expected=${rec.expected}`);
      }
    }
  };
})();
)JS";

  return RunScript(s, shim, "shim");
}

inline bool SetUpstreamRequireException(EnvScope &s, napi_value exception_value)
{
  napi_value global = nullptr;
  if (napi_get_global(s.env, &global) != napi_ok)
    return false;
  return napi_set_named_property(s.env, global, "__napi_test_require_exception", exception_value) == napi_ok;
}

inline bool RunUpstreamJsFile(EnvScope &s, const std::string &path)
{
  const std::string source = ReadTextFile(path);
  if (source.empty())
  {
    ADD_FAILURE() << "Unable to read upstream JS file: " << path;
    return false;
  }
  if (!RunScript(s, source, path.c_str()))
    return false;
  if (!RunScript(s, "for (let i = 0; i < 32; i++) __napi_force_gc();", "pre-verify-gc"))
    return false;
  return RunScript(s, "__napi_verify_must_call();", "must-call-verification");
}

inline bool RunUpstreamJsFileNoMustCallVerification(EnvScope &s, const std::string &path)
{
  const std::string source = ReadTextFile(path);
  if (source.empty())
  {
    ADD_FAILURE() << "Unable to read upstream JS file: " << path;
    return false;
  }
  if (!RunScript(s, source, path.c_str()))
    return false;
  return RunScript(s, "for (let i = 0; i < 32; i++) __napi_force_gc();", "post-run-gc");
}

#endif // NAPI_QUICKJS_UPSTREAM_JS_TEST_H_
