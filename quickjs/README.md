# QuickJS provider builds

The bundled QuickJS build includes the private constructor hooks used by
`unofficial_napi_define_binding`. It supports both `CLASS` and `METHOD`.

An external QuickJS library or a preexisting CMake `qjs` target supports
`METHOD`. A `CLASS` request returns `napi_generic_failure` and leaves both
outputs untouched when the engine lacks the constructor hooks. Existing
Node-API functions, including `napi_define_class`, remain available.

If the external engine was built with the hooks in
[`engine/napi_quickjs_engine.c`](engine/napi_quickjs_engine.c), configure NAPI
with `-DNAPI_QUICKJS_EXTERNAL_BINDING_HOOKS=ON` to enable `CLASS`.
