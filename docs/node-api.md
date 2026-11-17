# node_api.h in napi

`include/js_native_api.h` is the engine: values, objects, functions,
ArrayBuffers and typed arrays. napi implements it for V8 and QuickJS, and
`include/unofficial_napi.h` extends it.

`include/node_api.h` is Node's runtime on top of that: addon registration,
the event loop, async work, threadsafe functions, async context, Buffers and
env teardown. Most of it was never napi's. Edge.js defines async work,
threadsafe functions, `napi_get_uv_event_loop`, `napi_make_callback` and the
rest in its own `src/node_api.cc`. The calls napi still defines are
**deprecated**:

| Call | Why it is the embedder's |
|---|---|
| `napi_create_buffer`, `napi_create_buffer_copy`, `napi_create_external_buffer`, `node_api_create_buffer_from_arraybuffer`, `napi_is_buffer`, `napi_get_buffer_info` | A Buffer is a Uint8Array with `Buffer.prototype`, which Node's `lib/` makes. Only the embedder that runs `lib/` has it; napi could only look it up on the global, which a script controls. |
| `napi_add_env_cleanup_hook`, `napi_remove_env_cleanup_hook` | Node runs them when its Environment is cleaned up, which is the embedder's teardown. The embedder keeps them; what must run at the env's release, interleaved with its finalizers (a hook a finalizer adds then), it runs from the `env_cleanup_callback` of `unofficial_napi_env_hooks`. |
| `napi_fatal_exception` | Node reports it as an uncaught exception (`process.on('uncaughtException')`), which is the embedder's. napi only throws it. |
| `napi_fatal_error` | Kept with the others so an embedder takes over all of `node_api.h`. |

## Building without them

CMake option `NAPI_PROVIDE_NODE_API` (default `ON`) compiles these calls
(`v8/src/node_api_v8.cc`, `quickjs/src/node_api_quickjs.cc`). An embedder
that defines them turns it off; otherwise a native link has two definitions
of each. napi's own code never calls the public names (it uses the
provider's internal `napi_v8_*` / `napi_quickjs_*` functions), so it does
not end up in the embedder's.

The WASIX host (`build.rs`) keeps them and keeps serving the `napi`
imports of the same names, so a guest built before its embedder defined
them still runs. A guest whose embedder defines them no longer imports them.

## Making a Buffer without them

`unofficial_napi_create_uint8array(env, length, prototype_or_null, &data,
&result)` makes a zero-filled Uint8Array of the current realm over its own
ArrayBuffer and gives it a prototype. With the embedder's `Buffer.prototype`
that is a Node Buffer, made in one call (one import on WASIX) without running
JS. The other Buffer calls compose from `js_native_api.h`:

- `napi_create_external_buffer`: `napi_create_external_arraybuffer`, then
  `napi_create_typedarray` and `node_api_set_prototype`.
- `node_api_create_buffer_from_arraybuffer`: `napi_create_typedarray` over
  the ArrayBuffer and `node_api_set_prototype`.
- `napi_is_buffer`: `napi_is_typedarray` or `napi_is_dataview` (Node's
  `Buffer::HasInstance` is any ArrayBufferView).
- `napi_get_buffer_info`: `napi_get_typedarray_info` or
  `napi_get_dataview_info`.

napi's own APIs that return bytes return plain Uint8Arrays, never Buffers:
`unofficial_napi_bytecode_serialize`, `unofficial_napi_module_wrap_create_cached_data`
and the serdes binding's `Serializer#releaseBuffer`. The embedder gives them
its Buffer prototype where its API promises a Buffer.
