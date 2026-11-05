import { createHash } from "node:crypto";
import { spawn } from "node:child_process";
import { createServer } from "node:net";
import { copyFile, mkdir, mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, resolve } from "node:path";
import { once } from "node:events";
import { fileURLToPath, pathToFileURL } from "node:url";

const ACORN_SHA256 = "953573b8fdab71599749ea5f2b33d3e760c2116178f9423ee7458dbe39d59453";
const TEST_NAME = "facade_js_napi_executes_and_releases_the_instance_session";
const PASS_RESULT = /test result:\s*ok\.\s+1 passed;\s+0 failed;\s+0 ignored;/;
const FAILURE_RESULT = /test result:\s*FAILED\./;

function required(value, name) {
  if (!value) throw new Error(`${name} is required`);
  return resolve(value);
}

function installSignalCancellation() {
  const controller = new AbortController();
  const handlers = new Map();
  for (const signal of ["SIGINT", "SIGTERM", "SIGHUP"]) {
    const handler = () => {
      if (!controller.signal.aborted) controller.abort(new Error(`received ${signal}`));
    };
    handlers.set(signal, handler);
    process.on(signal, handler);
  }
  return {
    signal: controller.signal,
    remove() {
      for (const [signal, handler] of handlers) process.off(signal, handler);
    },
  };
}

function cancellationError(signal) {
  return signal.reason instanceof Error ? signal.reason : new Error("browser test cancelled");
}

function throwIfCancelled(signal) {
  if (signal.aborted) throw cancellationError(signal);
}

function waitWithCancellation(promise, signal) {
  if (signal.aborted) return Promise.reject(cancellationError(signal));
  let onAbort;
  const cancelled = new Promise((_, reject) => {
    onAbort = () => reject(cancellationError(signal));
    signal.addEventListener("abort", onAbort, { once: true });
  });
  return Promise.race([Promise.resolve(promise), cancelled]).finally(() => {
    signal.removeEventListener("abort", onAbort);
  });
}

async function operationWithDeadline(promise, timeoutMs, name, signal) {
  const result = await waitWithCancellation(settleWithin(promise, timeoutMs), signal);
  if (result.kind === "error") throw result.error;
  if (result.kind === "timeout") throw new Error(`${name} timed out after ${timeoutMs} ms`);
  return result.value;
}

async function availablePort() {
  const server = createServer();
  await new Promise((resolveListen, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolveListen);
  });
  const { port } = server.address();
  await new Promise((resolveClose, reject) => server.close(error => error ? reject(error) : resolveClose()));
  return port;
}

function delay(milliseconds) {
  return new Promise(resolveDelay => setTimeout(resolveDelay, milliseconds));
}

function settleWithin(promise, timeoutMs) {
  let timer;
  const settled = Promise.resolve(promise).then(
    value => ({ kind: "settled", value }),
    error => ({ kind: "error", error }),
  );
  const timeout = new Promise(resolveTimeout => {
    timer = setTimeout(() => resolveTimeout({ kind: "timeout" }), timeoutMs);
  });
  return Promise.race([settled, timeout]).finally(() => clearTimeout(timer));
}

function hasExited(child) {
  return child.exitCode !== null || child.signalCode !== null;
}

async function waitForChildExit(child, timeoutMs) {
  if (hasExited(child)) return { exited: true };
  const result = await settleWithin(once(child, "exit"), timeoutMs);
  if (result.kind === "error") return { exited: hasExited(child), error: result.error };
  return { exited: result.kind === "settled" || hasExited(child) };
}

async function waitForServer(url, child, getSpawnError, output, cancellationSignal, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    throwIfCancelled(cancellationSignal);
    if (getSpawnError()) throw getSpawnError();
    if (child.exitCode !== null || child.signalCode !== null) {
      throw new Error(`wasm-bindgen-test-runner exited before serving ${url}\n${output()}`);
    }

    const requestTimeout = Math.min(1000, deadline - Date.now());
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), requestTimeout);
    const cancelRequest = () => controller.abort(cancellationError(cancellationSignal));
    cancellationSignal.addEventListener("abort", cancelRequest, { once: true });
    if (cancellationSignal.aborted) cancelRequest();
    try {
      const response = await fetch(url, { signal: controller.signal });
      response.body?.cancel().catch(() => {});
      if (response.ok) return;
    } catch {
      throwIfCancelled(cancellationSignal);
      // The bounded request timed out or the runner is still starting.
    } finally {
      clearTimeout(timer);
      cancellationSignal.removeEventListener("abort", cancelRequest);
    }
    await waitWithCancellation(delay(Math.min(100, Math.max(0, deadline - Date.now()))), cancellationSignal);
  }
  throwIfCancelled(cancellationSignal);
  throw new Error(`timed out waiting for wasm-bindgen-test-runner at ${url}\n${output()}`);
}

async function stopRunner(child) {
  if (!child?.pid || hasExited(child)) return;
  const errors = [];
  try {
    child.kill("SIGTERM");
  } catch (error) {
    errors.push(error);
  }
  let result = await waitForChildExit(child, 5000);
  if (result.error) errors.push(result.error);
  if (result.exited) {
    if (errors.length !== 0) throw new AggregateError(errors, "runner shutdown failed");
    return;
  }

  try {
    if (!hasExited(child)) child.kill("SIGKILL");
  } catch (error) {
    errors.push(error);
  }
  result = await waitForChildExit(child, 5000);
  if (result.error) errors.push(result.error);
  if (!result.exited) errors.push(new Error("wasm-bindgen-test-runner did not exit after SIGKILL"));
  if (errors.length !== 0) throw new AggregateError(errors, "runner shutdown failed");
}

async function stopBrowser(browser, browserServer) {
  const errors = [];
  if (browser) {
    try {
      const result = await settleWithin(browser.close(), 5000);
      if (result.kind === "error") errors.push(result.error);
      if (result.kind === "timeout") errors.push(new Error("Playwright browser connection did not close within 5 seconds"));
    } catch (error) {
      errors.push(error);
    }
  }

  if (browserServer) {
    let child;
    try {
      child = browserServer.process();
    } catch (error) {
      errors.push(error);
    }

    let close = { kind: "settled" };
    try {
      close = await settleWithin(browserServer.close(), 5000);
    } catch (error) {
      close = { kind: "error", error };
    }
    if (close.kind === "error") errors.push(close.error);

    if (child) {
      let result = hasExited(child) ? { exited: true } : await waitForChildExit(child, close.kind === "timeout" ? 1000 : 5000);
      if (result.error) errors.push(result.error);
      if (!result.exited) {
        try {
          child.kill("SIGKILL");
        } catch (error) {
          errors.push(error);
        }
        result = await waitForChildExit(child, 5000);
        if (result.error) errors.push(result.error);
        if (!result.exited) errors.push(new Error("Playwright browser process did not exit after SIGKILL"));
      }
    }
  }
  return errors;
}

async function collectCleanupError(errors, name, cleanup) {
  try {
    await cleanup();
  } catch (error) {
    errors.push(new Error(`${name}: ${error.message}`, { cause: error }));
  }
}

async function main() {
  const [wasmArgument, runnerArgument, acornArgument] = process.argv.slice(2);
  if (!wasmArgument || !runnerArgument || !acornArgument) {
    throw new Error("usage: node run.mjs TEST.wasm wasm-bindgen-test-runner acorn.mjs");
  }

  const wasmPath = required(wasmArgument, "test Wasm path");
  const runnerPath = required(runnerArgument, "wasm-bindgen-test-runner path");
  const acornPath = required(acornArgument, "Acorn ES module path");
  const playwrightPath = required(process.env.PLAYWRIGHT_MODULE, "PLAYWRIGHT_MODULE path to Playwright's index.mjs");
  const { chromium } = await import(pathToFileURL(playwrightPath).href);
  if (!chromium) throw new Error("PLAYWRIGHT_MODULE does not export chromium");

  const acornBytes = await readFile(acornPath);
  const acornHash = createHash("sha256").update(acornBytes).digest("hex");
  if (acornHash !== ACORN_SHA256) {
    throw new Error(`Acorn hash mismatch: expected ${ACORN_SHA256}, got ${acornHash}`);
  }

  const cancellation = installSignalCancellation();
  let stage;
  let browserServer;
  let browser;
  let runner;
  let pendingBrowserServerLaunch;
  let pendingBrowserConnect;
  let cleanupStarted = false;
  let browserConnectionCleanup;
  let browserServerCleanup;
  let runnerOutput = "";
  let runnerError = "";
  let spawnError = null;
  let testOutput;
  let browserUrl;
  let failure;
  const cleanupErrors = [];

  const cleanupConnection = connection => {
    if (!connection) return Promise.resolve([]);
    browserConnectionCleanup ??= stopBrowser(connection, undefined);
    return browserConnectionCleanup;
  };
  const cleanupServer = server => {
    if (!server) return Promise.resolve([]);
    browserServerCleanup ??= stopBrowser(undefined, server);
    return browserServerCleanup;
  };

  try {
    stage = await mkdtemp(resolve(tmpdir(), "wasmer-napi-browser-"));
    throwIfCancelled(cancellation.signal);
    const stageTmp = resolve(stage, "tmp");
    await mkdir(stageTmp, { recursive: true });
    throwIfCancelled(cancellation.signal);
    const here = dirname(fileURLToPath(import.meta.url));
    await copyFile(resolve(here, "napi-browser.html"), resolve(stage, "napi-browser.html"));
    throwIfCancelled(cancellation.signal);
    await copyFile(acornPath, resolve(stage, "acorn.js"));
    throwIfCancelled(cancellation.signal);

    const port = await availablePort();
    throwIfCancelled(cancellation.signal);
    const address = `127.0.0.1:${port}`;
    browserUrl = `http://${address}/napi-browser.html`;
    const environment = {
      ...process.env,
      NO_HEADLESS: "1",
      WASM_BINDGEN_KEEP_TEST_BUILD: "1",
      WASM_BINDGEN_TEST_ADDRESS: address,
      TMPDIR: stageTmp,
      TEMP: stageTmp,
      TMP: stageTmp,
      PATH: `${dirname(runnerPath)}${process.platform === "win32" ? ";" : ":"}${process.env.PATH ?? ""}`,
    };
    // Preserve the runner's COOP/COEP response headers for every served asset.
    delete environment.WASM_BINDGEN_TEST_NO_ORIGIN_ISOLATION;

    runner = spawn(runnerPath, [wasmPath, "--nocapture"], {
      cwd: stage,
      env: environment,
      stdio: ["ignore", "pipe", "pipe"],
    });
    runner.once("error", error => { spawnError = error; });
    runner.stdout.on("data", chunk => { runnerOutput += chunk.toString(); });
    runner.stderr.on("data", chunk => { runnerError += chunk.toString(); });

    await waitForServer(
      browserUrl,
      runner,
      () => spawnError,
      () => `${runnerOutput}\n${runnerError}`,
      cancellation.signal,
    );
    throwIfCancelled(cancellation.signal);

    pendingBrowserServerLaunch = chromium.launchServer({
      headless: true,
      timeout: 30000,
      handleSIGINT: false,
      handleSIGTERM: false,
      handleSIGHUP: false,
      ...(process.env.CHROME ? { executablePath: resolve(process.env.CHROME) } : {}),
    }).then(async server => {
      browserServer = server;
      if (cleanupStarted) {
        const errors = await cleanupServer(server);
        if (errors.length !== 0) {
          throw new AggregateError(errors, "late Playwright browser server shutdown failed");
        }
      }
      return server;
    });
    browserServer = await waitWithCancellation(pendingBrowserServerLaunch, cancellation.signal);
    throwIfCancelled(cancellation.signal);

    pendingBrowserConnect = chromium.connect(browserServer.wsEndpoint(), { timeout: 30000 }).then(async connection => {
      browser = connection;
      if (cleanupStarted) {
        const errors = await cleanupConnection(connection);
        if (errors.length !== 0) {
          throw new AggregateError(errors, "late Playwright browser connection shutdown failed");
        }
      }
      return connection;
    });
    browser = await waitWithCancellation(pendingBrowserConnect, cancellation.signal);
    throwIfCancelled(cancellation.signal);

    const page = await operationWithDeadline(browser.newPage(), 15000, "Playwright newPage", cancellation.signal);
    const browserErrors = [];
    const scriptResponses = [];
    let reportBrowserError;
    const firstBrowserError = new Promise(resolveError => { reportBrowserError = resolveError; });
    page.on("response", response => {
      if (response.request().resourceType() === "script") {
        const contentType = response.headers()["content-type"] ?? "<missing content-type>";
        scriptResponses.push(`${response.status()} ${contentType} ${response.url()}`);
      }
    });
    page.on("pageerror", error => {
      browserErrors.push(`page error: ${error.stack ?? error.message}`);
      reportBrowserError(error);
    });
    page.on("console", message => {
      if (message.type() === "error") {
        const location = message.location();
        const detail = `${message.text()} at ${location.url}:${location.lineNumber}:${location.columnNumber}`;
        browserErrors.push(`console error: ${detail}`);
        reportBrowserError(new Error(detail));
      }
    });

    await operationWithDeadline(
      page.goto(browserUrl, { waitUntil: "domcontentloaded", timeout: 15000 }),
      15000,
      "Playwright page navigation",
      cancellation.signal,
    );
    const result = operationWithDeadline(page.waitForFunction(() => {
      const output = document.querySelector("#output")?.textContent ?? "";
      return output.includes("test result: ok.") || output.includes("test result: FAILED.");
    }, null, { timeout: 90000 }), 90000, "wasm-bindgen browser test", cancellation.signal);
    const verdict = await Promise.race([
      result.then(() => ({ complete: true })),
      firstBrowserError.then(error => ({ error })),
    ]);
    if (verdict.error) {
      throw new Error(`${verdict.error.message}\nscript responses:\n${scriptResponses.join("\n")}`);
    }

    testOutput = await operationWithDeadline(
      page.locator("#output").innerText(),
      15000,
      "wasm-bindgen output read",
      cancellation.signal,
    );
    if (!testOutput.includes(TEST_NAME)) {
      throw new Error(`expected test ${TEST_NAME} in wasm-bindgen output\n${testOutput}`);
    }
    if (FAILURE_RESULT.test(testOutput) || !PASS_RESULT.test(testOutput)) {
      throw new Error(`expected exactly one passing wasm-bindgen browser test\n${testOutput}`);
    }
    if (browserErrors.length !== 0) {
      throw new Error(`browser console/page errors:\n${browserErrors.join("\n")}\nscript responses:\n${scriptResponses.join("\n")}`);
    }
  } catch (error) {
    failure = error;
  } finally {
    cleanupStarted = true;

    if (pendingBrowserServerLaunch) {
      const result = await settleWithin(pendingBrowserServerLaunch, 35000);
      if (result.kind === "timeout") {
        cleanupErrors.push(new Error("Playwright launchServer did not settle within 35 seconds"));
      } else if (result.kind === "error") {
        if (!failure) cleanupErrors.push(result.error);
      } else {
        browserServer ??= result.value;
      }
    }
    if (pendingBrowserConnect) {
      const result = await settleWithin(pendingBrowserConnect, 35000);
      if (result.kind === "timeout") {
        cleanupErrors.push(new Error("Playwright connect did not settle within 35 seconds"));
      } else if (result.kind === "error") {
        if (!failure) cleanupErrors.push(result.error);
      } else {
        browser ??= result.value;
      }
    }

    await collectCleanupError(cleanupErrors, "Playwright browser connection shutdown", async () => {
      const errors = await cleanupConnection(browser);
      if (errors.length !== 0) throw new AggregateError(errors, "Playwright browser connection shutdown failed");
    });
    await collectCleanupError(cleanupErrors, "Playwright browser server shutdown", async () => {
      const errors = await cleanupServer(browserServer);
      if (errors.length !== 0) throw new AggregateError(errors, "Playwright browser server shutdown failed");
    });
    await collectCleanupError(cleanupErrors, "wasm-bindgen-test-runner shutdown", () => stopRunner(runner));
    if (stage) {
      await collectCleanupError(cleanupErrors, "temporary stage cleanup", async () => {
        const result = await settleWithin(rm(stage, { recursive: true, force: true }), 15000);
        if (result.kind === "error") throw result.error;
        if (result.kind === "timeout") throw new Error("temporary stage removal exceeded 15 seconds");
      });
    }

    if (cancellation.signal.aborted && !failure) failure = cancellationError(cancellation.signal);
    cancellation.remove();
  }

  if (failure) {
    for (const error of cleanupErrors) console.error(error);
    throw failure;
  }
  if (cleanupErrors.length !== 0) {
    throw new AggregateError(cleanupErrors, "browser test cleanup failed");
  }

  console.log(testOutput.trim());
  console.log(`browser_test_url=${browserUrl}`);
  console.log("browser_test_status=pass (runner server and browser were stopped after the DOM verdict)");
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
