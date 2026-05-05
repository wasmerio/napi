#ifndef NAPI_QUICKJS_HANDLE_SCOPE_H_
#define NAPI_QUICKJS_HANDLE_SCOPE_H_

#include "napi_scope.h"

struct napi_handle_scope__ : napi_scope__
{
  static napi_handle_scope__ *create(napi_env env, napi_scope__ *parent);
  static void destroy(napi_handle_scope__ *scope);

private:
  napi_handle_scope__(napi_env env, napi_scope__ *parent);
};

#endif // NAPI_QUICKJS_HANDLE_SCOPE_H_
