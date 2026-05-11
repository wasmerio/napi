#ifndef NAPI_QUICKJS_LIFETIME_TRACKER_H_
#define NAPI_QUICKJS_LIFETIME_TRACKER_H_

#include <cstddef>

struct napi_env__;

namespace quickjs::detail
{

class napi_lifetime_tracker__
{
public:
  static void maybe_dump(napi_env__ *env);
  static void dump(napi_env__ *env, const char *reason);
};

} // namespace quickjs::detail

extern "C" void napi_quickjs_lifetime_dump(napi_env__ *env, const char *reason);

#endif // NAPI_QUICKJS_LIFETIME_TRACKER_H_
