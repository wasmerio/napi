#include "compat/global_shims.h"

#include <cstring>

namespace quickjs::detail
{
    // Brief: EnsureSymbolProperty belongs to the global shim compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void EnsureSymbolProperty(JSContext *ctx,
                              JSValueConst symbol_ctor,
                              const char *name,
                              const char *description)
    {
        JSValue existing = JS_GetPropertyStr(ctx, symbol_ctor, name);
        if (JS_IsException(existing))
            return;
        bool missing = JS_IsUndefined(existing);
        JS_FreeValue(ctx, existing);
        if (!missing)
            return;

        JS_DefinePropertyValueStr(
            ctx, symbol_ctor, name, JS_NewSymbol(ctx, description, false), 0);
    }

    // Brief: EnsureNodeWellKnownSymbols belongs to the global shim compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void EnsureNodeWellKnownSymbols(JSContext *ctx)
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue symbol_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
        JS_FreeValue(ctx, global);
        if (JS_IsException(symbol_ctor) || !JS_IsObject(symbol_ctor))
        {
            JS_FreeValue(ctx, symbol_ctor);
            return;
        }

        EnsureSymbolProperty(ctx, symbol_ctor, "dispose", "Symbol.dispose");
        EnsureSymbolProperty(ctx, symbol_ctor, "asyncDispose", "Symbol.asyncDispose");
        JS_FreeValue(ctx, symbol_ctor);
    }

    // Undici is Node's fetch/HTTP client. It loads llhttp, its HTTP/1 parser,
    // through a small WebAssembly module; QuickJS does not expose a general
    // WebAssembly engine here, so provide only the parser-shaped surface Undici
    // asks for.
    // Brief: EnsureUndiciLlhttpWebAssemblyShim belongs to the global shim compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void EnsureUndiciLlhttpWebAssemblyShim(JSContext *ctx)
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue existing = JS_GetPropertyStr(ctx, global, "WebAssembly");
        const bool missing = !JS_IsException(existing) && JS_IsUndefined(existing);
        JS_FreeValue(ctx, existing);
        JS_FreeValue(ctx, global);
        if (!missing)
            return;

        static constexpr const char *kShim = R"JS(
(() => {
  "use strict";
  const OK = 0;
  const PAUSED = 21;
  const PAUSED_UPGRADE = 22;
  const TYPE_RESPONSE = 2;
  const textDecoder = typeof TextDecoder === "function" ? new TextDecoder("latin1") : null;
  let memoryBuffer = new ArrayBuffer(1024 * 1024);
  let heapTop = 1024;
  let nextParser = 1;
  let env = null;
  const parsers = new Map();

  function ensureMemory(size) {
    if (size <= memoryBuffer.byteLength) return;
    let next = memoryBuffer.byteLength;
    while (next < size) next *= 2;
    const grown = new ArrayBuffer(next);
    new Uint8Array(grown).set(new Uint8Array(memoryBuffer));
    memoryBuffer = grown;
  }

  function decode(bytes) {
    if (textDecoder) return textDecoder.decode(bytes);
    let out = "";
    for (let i = 0; i < bytes.length; i += 0x8000) {
      out += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    }
    return out;
  }

  function callback(name, ...args) {
    const fn = env && env[name];
    return typeof fn === "function" ? fn(...args) : OK;
  }

  function resetParser(p, type = p.type) {
    p.type = type;
    p.headerText = "";
    p.state = "headers";
    p.contentLength = null;
    p.bodyRemaining = 0;
    p.chunked = false;
    p.chunkBuffer = "";
    p.statusCode = 0;
    p.shouldKeepAlive = true;
    p.upgrade = false;
    p.errorPos = 0;
    p.errorReason = 0;
  }

  function findHeaderOffsets(chunkText, headerText, lines) {
    const offsets = [];
    let searchFrom = 0;
    for (const line of lines) {
      const index = chunkText.indexOf(line, searchFrom);
      offsets.push(index);
      if (index >= 0) searchFrom = index + line.length + 2;
    }
    return offsets;
  }

  function parseHeaders(p, chunkText, basePtr) {
    const end = p.headerText.indexOf("\r\n\r\n");
    if (end < 0) return false;

    const head = p.headerText.slice(0, end);
    const lines = head.split("\r\n");
    const first = lines.shift() || "";
    callback("wasm_on_message_begin", p.ptr);

    if (p.type === TYPE_RESPONSE) {
      const match = /^HTTP\/(\d+)\.(\d+)\s+(\d+)\s*(.*)$/.exec(first);
      if (match) {
        p.statusCode = Number(match[3]) || 0;
        const statusText = match[4] || "";
        if (statusText) {
          const statusAt = chunkText.indexOf(statusText);
          if (statusAt >= 0) callback("wasm_on_status", p.ptr, basePtr + statusAt, statusText.length);
        }
      }
    }

    const headerOffsets = findHeaderOffsets(chunkText, head, lines);
    const headers = Object.create(null);
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];
      const colon = line.indexOf(":");
      if (colon <= 0) continue;
      const name = line.slice(0, colon);
      const rawValue = line.slice(colon + 1);
      const leading = rawValue.match(/^\s*/)[0].length;
      const value = rawValue.slice(leading);
      const lineAt = headerOffsets[i];
      if (lineAt >= 0) {
        callback("wasm_on_header_field", p.ptr, basePtr + lineAt, name.length);
        callback("wasm_on_header_value", p.ptr, basePtr + lineAt + colon + 1 + leading, value.length);
      }
      headers[name.toLowerCase()] = value;
    }

    const connection = (headers.connection || "").toLowerCase();
    const transferEncoding = (headers["transfer-encoding"] || "").toLowerCase();
    p.contentLength = headers["content-length"] != null ? Number(headers["content-length"]) : null;
    p.chunked = transferEncoding.split(",").map((v) => v.trim()).includes("chunked");
    p.shouldKeepAlive = connection !== "close";
    p.upgrade = connection.includes("upgrade");

    const complete = callback(
      "wasm_on_headers_complete",
      p.ptr,
      p.statusCode,
      p.upgrade ? 1 : 0,
      p.shouldKeepAlive ? 1 : 0
    );
    p.state = "body";
    p.headerText = "";
    if (complete === 1 || p.statusCode === 204 || p.statusCode === 304) {
      callback("wasm_on_message_complete", p.ptr);
      resetParser(p);
      return true;
    }
    if (complete === 2) return PAUSED_UPGRADE;
    if (complete === PAUSED) return PAUSED;
    if (Number.isFinite(p.contentLength)) p.bodyRemaining = Math.max(0, p.contentLength);
    return true;
  }

  function consumeBody(p, chunkText, basePtr, start, end) {
    if (p.chunked) {
      let cursor = start;
      while (cursor < end) {
        const lineEnd = chunkText.indexOf("\r\n", cursor);
        if (lineEnd < 0) break;
        const size = Number.parseInt(chunkText.slice(cursor, lineEnd).split(";", 1)[0], 16);
        if (!Number.isFinite(size)) break;
        const bodyStart = lineEnd + 2;
        const bodyEnd = bodyStart + size;
        if (bodyEnd + 2 > end) break;
        if (size === 0) {
          callback("wasm_on_message_complete", p.ptr);
          resetParser(p);
          return OK;
        }
        const ret = callback("wasm_on_body", p.ptr, basePtr + bodyStart, size);
        if (ret !== OK) return ret;
        cursor = bodyEnd + 2;
      }
      return OK;
    }

    if (Number.isFinite(p.bodyRemaining)) {
      const len = Math.min(p.bodyRemaining, end - start);
      if (len > 0) {
        const ret = callback("wasm_on_body", p.ptr, basePtr + start, len);
        if (ret !== OK) return ret;
        p.bodyRemaining -= len;
      }
      if (p.bodyRemaining === 0) {
        callback("wasm_on_message_complete", p.ptr);
        resetParser(p);
      }
    } else if (end > start) {
      const ret = callback("wasm_on_body", p.ptr, basePtr + start, end - start);
      if (ret !== OK) return ret;
    }
    return OK;
  }

  const exports = {
    get memory() {
      return { buffer: memoryBuffer };
    },
    malloc(size) {
      const ptr = (heapTop + 7) & ~7;
      heapTop = ptr + Math.max(0, size | 0);
      ensureMemory(heapTop + 8);
      return ptr;
    },
    free() {},
    llhttp_alloc(type) {
      const ptr = nextParser++;
      const parser = { ptr };
      resetParser(parser, type);
      parsers.set(ptr, parser);
      return ptr;
    },
    llhttp_free(ptr) {
      parsers.delete(ptr);
    },
    llhttp_init(ptr, type) {
      const parser = parsers.get(ptr);
      if (parser) resetParser(parser, type);
    },
    _initialize() {},
    __indirect_function_table: {},
    llhttp_execute(ptr, dataPtr, len) {
      const parser = parsers.get(ptr);
      if (!parser) return 1;
      const chunkText = decode(new Uint8Array(memoryBuffer, dataPtr, len));
      let bodyStart = 0;
      if (parser.state === "headers") {
        parser.headerText += chunkText;
        const result = parseHeaders(parser, chunkText, dataPtr);
        if (result !== true) return result === false ? OK : result;
        const headerEnd = chunkText.indexOf("\r\n\r\n");
        bodyStart = headerEnd >= 0 ? headerEnd + 4 : len;
      }
      return consumeBody(parser, chunkText, dataPtr, bodyStart, len);
    },
    llhttp_should_keep_alive(ptr) {
      const parser = parsers.get(ptr);
      return parser && parser.shouldKeepAlive ? 1 : 0;
    },
    llhttp_get_type(ptr) {
      const parser = parsers.get(ptr);
      return parser ? parser.type : 0;
    },
    llhttp_get_http_major() { return 1; },
    llhttp_get_http_minor() { return 1; },
    llhttp_get_method() { return 0; },
    llhttp_get_status_code(ptr) {
      const parser = parsers.get(ptr);
      return parser ? parser.statusCode : 0;
    },
    llhttp_get_upgrade(ptr) {
      const parser = parsers.get(ptr);
      return parser && parser.upgrade ? 1 : 0;
    },
    llhttp_reset(ptr) {
      const parser = parsers.get(ptr);
      if (parser) resetParser(parser);
    },
    llhttp_finish(ptr) {
      const parser = parsers.get(ptr);
      if (parser && parser.state === "body" && !Number.isFinite(parser.bodyRemaining)) {
        callback("wasm_on_message_complete", ptr);
        resetParser(parser);
      }
      return OK;
    },
    llhttp_pause() {},
    llhttp_resume() {},
    llhttp_resume_after_upgrade() {},
    llhttp_get_errno() { return OK; },
    llhttp_get_error_reason(ptr) {
      const parser = parsers.get(ptr);
      return parser ? parser.errorReason : 0;
    },
    llhttp_set_error_reason() {},
    llhttp_get_error_pos(ptr) {
      const parser = parsers.get(ptr);
      return parser && parser.errorPos ? parser.errorPos : 0;
    },
    llhttp_errno_name() { return 0; },
    llhttp_method_name() { return 0; },
    llhttp_status_name() { return 0; },
    llhttp_set_lenient_headers() {},
    llhttp_set_lenient_chunked_length() {},
    llhttp_set_lenient_keep_alive() {},
    llhttp_set_lenient_transfer_encoding() {},
    llhttp_set_lenient_version() {},
    llhttp_set_lenient_data_after_close() {},
    llhttp_set_lenient_optional_lf_after_cr() {},
    llhttp_set_lenient_optional_crlf_after_chunk() {},
    llhttp_set_lenient_optional_cr_before_lf() {},
    llhttp_set_lenient_spaces_after_chunk_size() {}
  };

  function Module(bytes) {
    if (!(this instanceof Module)) throw new TypeError("WebAssembly.Module must be called with new");
    this.bytes = bytes;
  }

  function Instance(module, imports) {
    if (!(this instanceof Instance)) throw new TypeError("WebAssembly.Instance must be called with new");
    env = imports && imports.env || {};
    this.exports = exports;
  }

  Object.defineProperty(globalThis, "WebAssembly", {
    configurable: true,
    writable: true,
    value: { Module, Instance }
  });
})();
)JS";

        JSValue result = JS_Eval(ctx, kShim, std::strlen(kShim), "<quickjs-undici-llhttp-wasm-shim>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, result);
    }

    // Brief: EnsureQuickjsWeakRefCompat belongs to the global shim compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void EnsureQuickjsWeakRefCompat(JSContext *ctx)
    {
        static constexpr const char *kShim = R"JS(
(() => {
  "use strict";
  let needsCompat = false;
  try {
    needsCompat = typeof WeakRef !== "function" || new WeakRef({}).deref() === undefined;
  } catch {
    needsCompat = true;
  }
  if (!needsCompat) return;

  class QuickjsWeakRefCompat {
    constructor(target) {
      if ((typeof target !== "object" && typeof target !== "function") || target === null) {
        throw new TypeError("WeakRef target must be an object");
      }
      this._target = target;
    }

    deref() {
      return this._target;
    }
  }

  Object.defineProperty(QuickjsWeakRefCompat, "name", {
    configurable: true,
    value: "WeakRef"
  });
  Object.defineProperty(globalThis, "WeakRef", {
    configurable: true,
    writable: true,
    value: QuickjsWeakRefCompat
  });
})();
)JS";

        JSValue result = JS_Eval(ctx, kShim, std::strlen(kShim), "<quickjs-weakref-compat>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, result);
    }

    // Brief: EnsureQuickjsGlobalCompat belongs to the global shim compatibility layer.
    // It keeps Node-facing behavior out of the public N-API entry points.
    // Inputs stay as QuickJS or N-API handles owned by the caller.
    // Failures either preserve QuickJS exception state or report N-API status.
    // Keep changes narrow so this compatibility bridge remains easy to remove.
    void EnsureQuickjsGlobalCompat(JSContext *ctx)
    {
        EnsureNodeWellKnownSymbols(ctx);
        EnsureQuickjsWeakRefCompat(ctx);
        EnsureUndiciLlhttpWebAssemblyShim(ctx);
    }
}
