#ifndef NAPI_QUICKJS_ENV_H_
#define NAPI_QUICKJS_ENV_H_

#include "../../../include/js_native_api.h"
#include "../../../include/node_api_types.h"
#include "../../../include/unofficial_napi.h"
#include "napi_agent.h"
#include "napi_callback_info.h"
#include "napi_value.h"

#include <quickjs.h>

// A realm: one JSContext of an agent. It owns nothing else. Values, scopes,
// references, the pending exception, instance data, cleanup hooks, modules,
// bytecode and hooks belong to the agent, and any env of the agent may be used
// with them.
struct napi_env__
{
  napi_env__(napi_agent__ *agent, JSContext *context);

  napi_agent__ *agent() const { return agent_; }
  JSContext *context() const { return context_; }

  // An external object of this realm wrapping `data`.
  JSValue wrap_external_data(void *data,
                             node_api_basic_finalize finalize_cb = nullptr,
                             void *finalize_hint = nullptr);

private:
  napi_agent__ *const agent_;
  JSContext *const context_;
};

napi_status napi_quickjs_set_last_error(napi_env env,
                                        napi_status status,
                                        const char *message);

napi_status napi_quickjs_clear_last_error(napi_env env);

#endif // NAPI_QUICKJS_ENV_H_
