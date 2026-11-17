# Browser run for the JavaScript N-API facade

`wasix_facade_js.rs` uses the real `NapiHook` and `WebHost` path. The pinned
`wasm-bindgen-test-runner` browser harness does not bundle the bare `acorn`
specifier imported by `lib/napi/src/snapi_js.rs`, so its built-in page fails
while loading the test module. This harness runs that same runner in its
interactive server mode and opens a test-owned page with an import map for the
existing Acorn ES module. It leaves the production import unchanged.

The script does not install packages, build the Wasm test, or start a separate
HTTP service. It copies the existing Acorn module into a temporary runner asset
directory, keeps the runner's normal COOP/COEP response headers, opens the
custom page through Playwright, and requires the named test plus exactly one
passing wasm-bindgen result. It fails on page errors and browser console errors.
The runner server is intentionally long-lived in interactive mode. The script
owns both the runner and Playwright browser processes, bounds their shutdown,
and removes the temporary stage after reading the page verdict. The runner's
kept test-build output is placed under that same stage for cleanup.
Playwright's automatic signal handlers are disabled; SIGINT, SIGTERM, and
SIGHUP use the script's cancellation and cleanup path. Browser launch and
connection have finite deadlines, and a launch still pending during cleanup
remains owned so a late browser server is closed.

Use an already-built `wasix_facade_js` Wasm test, the matching
`wasm-bindgen-test-runner`/`wasm-bindgen` pair, the existing Acorn 8.18.0
`acorn.mjs`, and an existing Playwright module. `PLAYWRIGHT_MODULE` must name
Playwright's `index.mjs`; `CHROME` optionally selects the installed browser.
The Acorn module is checked against SHA-256
`953573b8fdab71599749ea5f2b33d3e760c2116178f9423ee7458dbe39d59453` before it
is staged. The verified bytes are staged as `acorn.js` so the runner serves the
imported module with its JavaScript MIME type.

```sh
PLAYWRIGHT_MODULE=/path/to/existing/node_modules/playwright/index.mjs \
CHROME=/path/to/chrome \
node lib/napi/tests/browser/run.mjs \
  /path/to/wasix_facade_js.wasm \
  /path/to/matched/wasm-bindgen-test-runner \
  /path/to/existing/acorn/dist/acorn.mjs
```

Do not set `WASM_BINDGEN_TEST_NO_ORIGIN_ISOLATION`; the runner supplies the same
COOP/COEP headers to the custom HTML and all generated test modules.
