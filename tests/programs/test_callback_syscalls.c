// A guest function that JavaScript calls makes syscalls that have to wait:
// a sleep, file I/O and a write to stdout. The host cannot suspend the
// JavaScript frames between the guest and its caller, so the callback has to
// run to its end where it is, waits included.

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "napi_test_helpers.h"

static const char kText[] = "callback data";

// Sleeps, and returns the milliseconds that passed.
static napi_value sleep_cb(napi_env env, napi_callback_info info) {
  (void)info;
  struct timespec before, after;
  clock_gettime(CLOCK_MONOTONIC, &before);
  struct timespec delay = {0, 20 * 1000 * 1000};
  nanosleep(&delay, NULL);
  clock_gettime(CLOCK_MONOTONIC, &after);
  int64_t elapsed_ms = (int64_t)(after.tv_sec - before.tv_sec) * 1000 +
                       (after.tv_nsec - before.tv_nsec) / 1000000;
  printf("in callback: slept\n");
  fflush(stdout);
  napi_value result;
  napi_create_int32(env, (int32_t)elapsed_ms, &result);
  return result;
}

// Writes a file, reads it back and returns how many bytes matched.
static napi_value file_cb(napi_env env, napi_callback_info info) {
  (void)info;
  char path[64];
  snprintf(path, sizeof(path), "/tmp/napi_callback_syscalls_%d.txt",
           (int)getpid());
  int32_t matched = -1;
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd >= 0) {
    ssize_t written = write(fd, kText, sizeof(kText) - 1);
    close(fd);
    char buf[sizeof(kText)] = {0};
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
      ssize_t n = read(fd, buf, sizeof(buf) - 1);
      close(fd);
      if (written == (ssize_t)(sizeof(kText) - 1) && n == written &&
          memcmp(buf, kText, (size_t)n) == 0) {
        matched = (int32_t)n;
      }
    }
    unlink(path);
  }
  printf("in callback: wrote and read %d bytes\n", (int)matched);
  fflush(stdout);
  napi_value result;
  napi_create_int32(env, matched, &result);
  return result;
}

// Calls the JavaScript function it is given, which calls the guest again.
static napi_value reenter_cb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value global;
  napi_value result;
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  napi_get_global(env, &global);
  if (napi_call_function(env, global, argv[0], 0, NULL, &result) != napi_ok) {
    napi_get_undefined(env, &result);
  }
  return result;
}

static int define(napi_env env, napi_value global, const char* name,
                  napi_callback cb) {
  napi_value fn;
  NAPI_CALL(env, napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, NULL, &fn));
  NAPI_CALL(env, napi_set_named_property(env, global, name, fn));
  return 0;
}

static int run(napi_env env, const char* source, int32_t* out) {
  napi_value script, result;
  NAPI_CALL(env, napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &script));
  NAPI_CALL(env, napi_run_script(env, script, &result));
  NAPI_CALL(env, napi_get_value_int32(env, result, out));
  return 0;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  napi_value global;
  NAPI_CALL(env, napi_get_global(env, &global));
  if (define(env, global, "sleepInGuest", sleep_cb)) return 1;
  if (define(env, global, "fileInGuest", file_cb)) return 1;
  if (define(env, global, "reenter", reenter_cb)) return 1;

  int32_t value = 0;
  if (run(env, "sleepInGuest()", &value)) return 1;
  CHECK_OR_FAIL(value >= 15, "the callback did not sleep");

  if (run(env, "fileInGuest()", &value)) return 1;
  CHECK_OR_FAIL(value == (int32_t)(sizeof(kText) - 1), "file round trip failed");

  // The same from a callback nested in a callback: JavaScript, guest,
  // JavaScript, guest.
  if (run(env, "reenter(() => reenter(() => fileInGuest() + (sleepInGuest() >= 15 ? 1000 : 0)))",
          &value)) {
    return 1;
  }
  CHECK_OR_FAIL(value == 1000 + (int32_t)(sizeof(kText) - 1),
                "nested callbacks did not wait");

  return PrintSuccess("TEST_CALLBACK_SYSCALLS");
}
