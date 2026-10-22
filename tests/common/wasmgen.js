// Builds small WebAssembly modules as Uint8Arrays, so tests need no binary
// fixtures. Evaluates to nothing; defines globalThis.wasmgen.
(() => {
  const I32 = 0x7f;
  function leb(out, n) {
    do {
      let byte = n & 0x7f;
      n = Math.floor(n / 128);
      if (n) byte |= 0x80;
      out.push(byte);
    } while (n);
  }
  function section(out, id, body) {
    out.push(id);
    leb(out, body.length);
    for (let i = 0; i < body.length; i++) out.push(body[i]);
  }
  function name(out, text) {
    leb(out, text.length);
    for (let i = 0; i < text.length; i++) out.push(text.charCodeAt(i));
  }
  // types: [[params], [results]]; funcs: [{type, body: [...instrs]}];
  // memory: [flags, min, max?]; exports: [[name, kind, index]].
  function build({ types, funcs, memory, exports }) {
    const out = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];
    const t = [];
    leb(t, types.length);
    for (const [params, results] of types) {
      t.push(0x60);
      leb(t, params.length);
      t.push(...params);
      leb(t, results.length);
      t.push(...results);
    }
    section(out, 1, t);
    const f = [];
    leb(f, funcs.length);
    for (const func of funcs) leb(f, func.type);
    section(out, 3, f);
    if (memory) {
      const m = [1];
      for (const value of memory) leb(m, value);
      section(out, 5, m);
    }
    if (exports && exports.length) {
      const e = [];
      leb(e, exports.length);
      for (const [text, kind, index] of exports) {
        name(e, text);
        e.push(kind);
        leb(e, index);
      }
      section(out, 7, e);
    }
    const c = [];
    leb(c, funcs.length);
    for (const func of funcs) {
      leb(c, func.body.length + 2);
      c.push(0x00);
      for (let i = 0; i < func.body.length; i++) c.push(func.body[i]);
      c.push(0x0b);
    }
    section(out, 10, c);
    return new Uint8Array(out);
  }
  globalThis.wasmgen = {
    // (func (export "add") (param i32 i32) (result i32))
    add() {
      return build({
        types: [[[I32, I32], [I32]]],
        funcs: [{ type: 0, body: [0x20, 0x00, 0x20, 0x01, 0x6a] }],
        exports: [["add", 0, 0]],
      });
    },
    // `count` exported functions f<i>(p) chaining `loads` explicit-bounds
    // checked i32.loads from p over a one-page memory (all zero): lots of
    // compiled code, cheap to run.
    loads(count, loads) {
      const body = [0x20, 0x00];
      for (let i = 0; i < loads; i++) body.push(0x28, 0x02, 0x00);
      const funcs = [];
      const exports = [];
      for (let i = 0; i < count; i++) {
        funcs.push({ type: 0, body });
        exports.push(["f" + i, 0, i]);
      }
      return build({ types: [[[I32], [I32]]], funcs, memory: [0, 1], exports });
    },
    // `count` empty functions, no exports.
    functions(count) {
      const funcs = [];
      for (let i = 0; i < count; i++) funcs.push({ type: 0, body: [] });
      return build({ types: [[[], []]], funcs });
    },
    // (func (export "spin") (loop (br 0)))
    spin() {
      return build({
        types: [[[], []]],
        funcs: [{ type: 0, body: [0x03, 0x40, 0x0c, 0x00, 0x0b] }],
        exports: [["spin", 0, 0]],
      });
    },
    // A shared one-page memory and (func (export "wait") (result i32)) that
    // waits on address 0 forever.
    wait() {
      return build({
        types: [[[], [I32]]],
        funcs: [{
          type: 0,
          body: [0x41, 0x00, 0x41, 0x00, 0x42, 0x7f, 0xfe, 0x01, 0x02, 0x00],
        }],
        memory: [3, 1, 1],
        exports: [["wait", 0, 0]],
      });
    },
    // Memory "m" (initial 1, no maximum), "grow"(pages) and "bomb", which
    // grows by 16 pages until memory.grow fails and returns the final size.
    grower() {
      return build({
        types: [[[I32], [I32]], [[], [I32]]],
        funcs: [
          { type: 0, body: [0x20, 0x00, 0x40, 0x00] },
          {
            type: 1,
            body: [0x02, 0x40, 0x03, 0x40, 0x41, 0x10, 0x40, 0x00, 0x41, 0x7f,
                   0x46, 0x0d, 0x01, 0x0c, 0x00, 0x0b, 0x0b, 0x3f, 0x00],
          },
        ],
        memory: [0, 1],
        exports: [["m", 2, 0], ["grow", 0, 0], ["bomb", 0, 1]],
      });
    },
  };
})();
