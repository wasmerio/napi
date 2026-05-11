#ifndef NAPI_QUICKJS_HANDLE_SCOPE_H_
#define NAPI_QUICKJS_HANDLE_SCOPE_H_

#include "napi_scope.h"

struct napi_handle_scope__
{
  static napi_handle_scope__ *create(napi_env env, napi_scope_handle__ parent);
  static void destroy(napi_handle_scope__ *scope);
  ~napi_handle_scope__();

  napi_scope_handle__ scope_handle() const;
  napi_scope_handle__ parent_handle() const;
  napi_scope__ *scope() const;
  napi_scope__ *parent() const;
  napi_env env() const;

private:
  explicit napi_handle_scope__(napi_env env);

  napi_env env_ = nullptr;
  napi_scope_handle__ scope_ = nullptr;
};

#endif // NAPI_QUICKJS_HANDLE_SCOPE_H_
