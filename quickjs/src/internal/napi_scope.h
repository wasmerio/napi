#ifndef NAPI_QUICKJS_SCOPE_H_
#define NAPI_QUICKJS_SCOPE_H_

#include "../../../include/js_native_api.h"

#include <vector>
#include <quickjs.h>

struct napi_scope__
{
  static napi_scope__ *create(napi_env env, napi_scope__ *parent);
  static void destroy(napi_scope__ *scope);

  ~napi_scope__();

  napi_value wrap_value(JSValue value, bool owned);
  napi_value escape_value(napi_value value);
  void delete_value(napi_value value);
  void close();

  napi_scope__ *parent() const;
  napi_env env() const;

protected:
  napi_scope__(napi_env env, napi_scope__ *parent);

private:
  napi_env env_;
  napi_scope__ *parent_;
  std::vector<napi_value> values_;
  bool closed_ = false;
};

#endif // NAPI_QUICKJS_SCOPE_H_
