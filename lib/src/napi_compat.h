#ifndef NAPI_COMPAT_H_
#define NAPI_COMPAT_H_

#include "unofficial_napi.h"

// The deprecated API (the contextify calls and version 1 of the option
// structs) on the public realm API, the same for every provider. Where it
// named a context it named a contextified object, the env's own global, or
// nothing for the current context, which is the current realm now.
namespace napi::compat {

// The current realm: NULL for the env's own. The env's own global means the
// env's realm even when it is also a realm's sandbox, as the env's context
// did before realms existed.
napi_status current_realm(napi_env env, unofficial_napi_realm* realm_out);

// The realm `context` names: a contextified object's (a realm's global or
// sandbox), the env's (NULL) for the env's own global, and for NULL, undefined
// or null the current realm, or the env's when `nullish_is_current` is false.
// Any other value fails with napi_invalid_arg.
napi_status realm_from_context(napi_env env,
                               napi_value context,
                               bool nullish_is_current,
                               unofficial_napi_realm* realm_out);

// Checks the size and version of bytecode-open options (version 1 or 2) and
// returns the realm to compile for: version 1 compiles for the current realm.
napi_status bytecode_open_realm(napi_env env,
                                const unofficial_napi_bytecode_open_options* options,
                                unofficial_napi_realm* realm_out);

// Checks the size and version of module-create options (version 1 or 2) and
// returns the realm the module lives in, as unofficial_napi.h describes.
napi_status module_create_realm(napi_env env,
                                const unofficial_napi_module_create_options* options,
                                unofficial_napi_realm* realm_out);

}  // namespace napi::compat

#endif  // NAPI_COMPAT_H_
