#ifndef NAPI_QUICKJS_ENV_CLEANUP_HOOK_H_
#define NAPI_QUICKJS_ENV_CLEANUP_HOOK_H_

#include "../../../include/js_native_api.h"
#include "../../../include/node_api_types.h"

struct napi_env_cleanup_hook__
{
  static napi_env_cleanup_hook__ *create(napi_env env, napi_cleanup_hook hook, void *arg);
  static void destroy(napi_env_cleanup_hook__ *entry);

  void run() const;
  bool matches(napi_cleanup_hook hook, void *arg) const;

private:
  napi_env_cleanup_hook__(napi_env env, napi_cleanup_hook hook, void *arg);
  ~napi_env_cleanup_hook__();

  napi_env env_;
  napi_cleanup_hook hook_;
  void *arg_;
};

#endif // NAPI_QUICKJS_ENV_CLEANUP_HOOK_H_
