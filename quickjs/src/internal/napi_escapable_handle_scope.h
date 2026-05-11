#ifndef NAPI_QUICKJS_ESCAPABLE_HANDLE_SCOPE_H_
#define NAPI_QUICKJS_ESCAPABLE_HANDLE_SCOPE_H_

#include "napi_scope.h"

struct napi_escapable_handle_scope__ : napi_scope__
{
  static napi_escapable_handle_scope__ *create(napi_env env, napi_scope__ *parent);
  static void destroy(napi_escapable_handle_scope__ *scope);
  ~napi_escapable_handle_scope__();

  bool has_escaped() const;
  void mark_escaped();

private:
  napi_escapable_handle_scope__(napi_env env, napi_scope__ *parent);

  bool escaped_ = false;
};

#endif // NAPI_QUICKJS_ESCAPABLE_HANDLE_SCOPE_H_
