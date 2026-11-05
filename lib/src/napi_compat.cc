// This file implements the deprecated API.
#define UNOFFICIAL_NAPI_NO_DEPRECATION_WARNINGS
#include "napi_compat.h"

namespace napi::compat {
namespace {

bool is_nullish(napi_env env, napi_value value, napi_valuetype* type_out) {
  *type_out = napi_undefined;
  if (value == nullptr) return true;
  if (napi_typeof(env, value, type_out) != napi_ok) {
    *type_out = napi_undefined;
    return false;
  }
  return *type_out == napi_undefined || *type_out == napi_null;
}

// The env's own global, whichever realm is current.
napi_status env_global(napi_env env, napi_value* global_out) {
  unofficial_napi_realm_scope scope = 0;
  napi_status status = unofficial_napi_enter_realm(env, nullptr, &scope);
  if (status != napi_ok) return status;
  status = napi_get_global(env, global_out);
  const napi_status left = unofficial_napi_leave_realm(env, scope);
  return status != napi_ok ? status : left;
}

napi_status is_env_global(napi_env env, napi_value value, bool* result_out) {
  napi_value global = nullptr;
  napi_status status = env_global(env, &global);
  if (status != napi_ok) return status;
  return napi_strict_equals(env, value, global, result_out);
}

}  // namespace

napi_status current_realm(napi_env env, unofficial_napi_realm* realm_out) {
  *realm_out = nullptr;
  napi_value global = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok) return status;
  unofficial_napi_realm realm = nullptr;
  status = unofficial_napi_get_realm(env, global, &realm);
  if (status != napi_ok || realm == nullptr) return status;
  bool env_own = false;
  status = is_env_global(env, global, &env_own);
  if (status != napi_ok) return status;
  if (!env_own) *realm_out = realm;
  return napi_ok;
}

napi_status realm_from_context(napi_env env,
                               napi_value context,
                               bool nullish_is_current,
                               unofficial_napi_realm* realm_out) {
  *realm_out = nullptr;
  napi_valuetype type = napi_undefined;
  if (is_nullish(env, context, &type)) {
    return nullish_is_current ? current_realm(env, realm_out) : napi_ok;
  }
  if (type != napi_object && type != napi_function) return napi_invalid_arg;
  if (unofficial_napi_get_realm(env, context, realm_out) != napi_ok) return napi_invalid_arg;
  if (*realm_out != nullptr) return napi_ok;
  bool env_own = false;
  const napi_status status = is_env_global(env, context, &env_own);
  if (status != napi_ok) return status;
  return env_own ? napi_ok : napi_invalid_arg;
}

napi_status bytecode_open_realm(napi_env env,
                                const unofficial_napi_bytecode_open_options* options,
                                unofficial_napi_realm* realm_out) {
  *realm_out = nullptr;
  if (options == nullptr) return napi_invalid_arg;
  if (options->version == 1) {
    if (options->size < UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_V1_SIZE) return napi_invalid_arg;
    return current_realm(env, realm_out);
  }
  if (options->version != UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION ||
      options->size < sizeof(*options)) {
    return napi_invalid_arg;
  }
  *realm_out = options->realm_or_null;
  return napi_ok;
}

napi_status module_create_realm(napi_env env,
                                const unofficial_napi_module_create_options* options,
                                unofficial_napi_realm* realm_out) {
  *realm_out = nullptr;
  if (options == nullptr) return napi_invalid_arg;
  const bool v1 = options->version == 1;
  if (v1 ? options->size < UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_V1_SIZE
         : options->version != UNOFFICIAL_NAPI_MODULE_CREATE_OPTIONS_VERSION ||
               options->size < sizeof(*options)) {
    return napi_invalid_arg;
  }
  if (!v1 && options->realm_or_null != nullptr) {
    napi_valuetype type = napi_undefined;
    if (!is_nullish(env, options->context_or_undefined, &type)) return napi_invalid_arg;
    *realm_out = options->realm_or_null;
    return napi_ok;
  }
  return realm_from_context(env, options->context_or_undefined, v1, realm_out);
}

}  // namespace napi::compat

extern "C" {

napi_status NAPI_CDECL unofficial_napi_contextify_make_context(
    napi_env env,
    napi_value sandbox_or_symbol,
    napi_value name,
    napi_value origin_or_undefined,
    bool allow_code_gen_strings,
    bool allow_code_gen_wasm,
    bool own_microtask_queue,
    napi_value host_defined_option_id,
    napi_value* result_out) {
  if (env == nullptr || sandbox_or_symbol == nullptr || name == nullptr || result_out == nullptr) {
    return napi_invalid_arg;
  }
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, sandbox_or_symbol, &type) != napi_ok ||
      (type != napi_symbol && type != napi_object && type != napi_function)) {
    return napi_invalid_arg;
  }
  unofficial_napi_realm_options options;
  unofficial_napi_realm_options_init(&options);
  options.sandbox_or_null = type == napi_symbol ? nullptr : sandbox_or_symbol;
  options.name = name;
  options.origin = origin_or_undefined;
  options.host_defined_option_id = host_defined_option_id;
  options.allow_code_gen_strings = allow_code_gen_strings;
  options.allow_code_gen_wasm = allow_code_gen_wasm;
  options.own_microtask_queue = own_microtask_queue;
  unofficial_napi_realm realm = nullptr;
  return unofficial_napi_create_realm(env, &options, &realm, result_out);
}

napi_status NAPI_CDECL unofficial_napi_contextify_run_script(
    napi_env env,
    napi_value sandbox_or_null,
    const unofficial_napi_js_source* source,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    int64_t timeout,
    bool display_errors,
    bool break_on_sigint,
    bool break_on_first_line,
    napi_value host_defined_option_id,
    napi_value* result_out) {
  if (env == nullptr) return napi_invalid_arg;
  unofficial_napi_realm realm = nullptr;
  const napi_status status =
      napi::compat::realm_from_context(env, sandbox_or_null, true, &realm);
  if (status != napi_ok) return status;
  return unofficial_napi_run_script(env,
                                    realm,
                                    source,
                                    filename,
                                    line_offset,
                                    column_offset,
                                    timeout,
                                    display_errors,
                                    break_on_sigint,
                                    break_on_first_line,
                                    host_defined_option_id,
                                    result_out);
}

napi_status NAPI_CDECL unofficial_napi_contextify_compile_function(
    napi_env env,
    const unofficial_napi_js_source* source,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    napi_value parsing_context_or_undefined,
    napi_value context_extensions_or_undefined,
    napi_value params_or_undefined,
    napi_value host_defined_option_id,
    napi_value* result_out) {
  if (env == nullptr) return napi_invalid_arg;
  unofficial_napi_realm realm = nullptr;
  const napi_status status =
      napi::compat::realm_from_context(env, parsing_context_or_undefined, true, &realm);
  if (status != napi_ok) return status;
  return unofficial_napi_compile_function(env,
                                          realm,
                                          source,
                                          filename,
                                          line_offset,
                                          column_offset,
                                          context_extensions_or_undefined,
                                          params_or_undefined,
                                          host_defined_option_id,
                                          result_out);
}

}  // extern "C"
