 # napi/v8

 `napi/v8` is a standalone compatibility layer that implements Node-API (N-API)
 on top of V8 while keeping V8 details internal.

 ## Scope

 - Public surface: N-API headers and ABI (`js_native_api.h`, `node_api.h`).
 - Internal implementation: V8-backed code only in `src/`.
 - Host/runtime services such as `libuv` loops and memory queries are owned by
   Edge native code and injected through unofficial embedder hooks.
 - Test strategy: keep standalone `napi/v8` coverage focused on portable V8
   adapter behavior first.

 ## Porting Policy

 - Source and tests should be ported from upstream Node as fully as possible.
 - Keep upstream files/logic verbatim unless adaptation is strictly required.
 - The only intended code adaptation rule: replace direct V8 API usage with
   equivalent N-API usage.
 - Favor harness/environment shims over rewriting upstream test content.

 ## Layout

- `../include/`: shared public C headers (engine-agnostic surface)
- `../tests/`: shared canonical Node-API fixtures
- `src/`: V8-backed implementation and environment glue, with no direct
  `libuv` ownership
- `tests/`: V8-specific test assets and compatibility test docs

 ## Current Phase

 This directory implements the standalone V8 adapter plus a portable test
 subset. Runtime-owned Node-API behavior that depends on host event-loop or
 `libuv` services is validated from the Edge runtime build rather than this
 package.

See `tests/README.md` for build/run instructions and
`tests/PORTABILITY_MATRIX.md` for the current portability classification.

## Diagnostics

### Per-env CPU profiles: `NAPI_V8_CPU_PROFILE_DIR`

When the host process sets `NAPI_V8_CPU_PROFILE_DIR` to an existing
directory, each env that `unofficial_napi_create_env` creates records a V8
CPU profile, from the end of that call to the start of
`unofficial_napi_release_env`. Nothing else can turn it on: no N-API call,
guest import or script. Unset or empty, it does nothing. It is read each time
an env is created.

Each session writes two files, `<name>.cpuprofile` and
`<name>.metadata.json`. `<name>` is `env-<pid>-<token>-<n>`, where `<token>`
is 16 hex digits drawn once per process and `<n>` numbers the process's
sessions from 1.

- `<name>.cpuprofile`: the profile, in the JSON format Chrome DevTools loads.
  It is written from V8's public profile API, as ASCII-only JSON. (V8 13.6's
  `CpuProfile::Serialize` doesn't escape function names or URLs, and narrows
  timestamps to 32 bits.)
- `<name>.metadata.json`: one line of JSON about the session. `status` is
  `complete` when the profile is whole, and `valid` is true only then. It also
  gives `samples`, `nodes`, `outputBytes`, `errno` when a file operation
  failed, the profile's start and end times in V8's microseconds, and the
  host's steady clock, in nanoseconds, around the profiler's start and stop.

Both files are created with the env, with `O_EXCL` and mode `0600`, so an
existing file is never overwritten, and neither is reached through a
symbolic link. The profile stays empty until the env is released. The
directory must be an absolute path; it may be a symbolic link (macOS's `/tmp`
is one).

When the directory can't be used (not absolute, missing, not writable), the
env runs as usual and the record goes to stderr instead, as one line that
starts with `NAPI_V8_CPU_PROFILE `. Its `status` says which step failed
(`directory_not_absolute`, `directory_open_failed`, `output_open_failed`),
and `errno` says why. If the record file was created but the profile's
wasn't, the record goes to its file. Any other `status` but `complete` also
names what went wrong, and `valid` is false for all of them.

Limits, per process: 8 sessions. Every env created while the variable is set
takes one, even if its session fails to start. Envs after those aren't
profiled, and the first one says so with `session_limit_reached` on stderr.
Per profile: 2^20 samples (a profile that reaches it is written, with
`sample_limit_reached`, and `valid` false), 2^20 nodes and 256 MiB of JSON
(past those, nothing is written). The session counts toward V8's limit of 100
simultaneous CPU profiles per env, so while it is on,
`unofficial_napi_profile_start` reports `unofficial_napi_profile_start_busy`
one profile earlier.

V8 samples the env's thread every millisecond, so profiling costs CPU time;
don't measure performance with it on. Like `unofficial_napi_profile_stop`,
writing the profile needs the env's isolate to be the current one, so
release the env on the thread that created it. Releasing a thread's envs out
of reverse creation order is not supported, with or without profiling: V8's
`Isolate::Exit` then touches an isolate that was already disposed. If an env
is released on another thread, or its isolate isn't the current one at the
release (as can happen when a thread's envs are released out of order), the
record says `stop_wrong_thread` or `stop_isolate_not_current`, no profile is
written, and env teardown stops the session. Linux and macOS only; elsewhere
each session reports `unsupported_host_platform` on stderr.
