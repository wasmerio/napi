#ifndef NAPI_QUICKJS_VALUE_H_
#define NAPI_QUICKJS_VALUE_H_

#include "../../../include/js_native_api.h"

#include <vector>
#include <quickjs.h>

struct napi_value__
{
  static napi_value__ *create(napi_env env, JSValue value, bool owned);
  static void destroy(napi_value__ *value);

  ~napi_value__();

  JSValueConst get_inner() const;

private:
  napi_value__(napi_env env, JSValue value, bool owned);

  napi_env env_;
  JSValue value_;
};

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

protected:
  napi_scope__(napi_env env, napi_scope__ *parent);

private:
  napi_env env_;
  napi_scope__ *parent_;
  std::vector<napi_value> values_;
  bool closed_ = false;
};

struct napi_handle_scope__ : napi_scope__
{
  napi_handle_scope__(napi_env env, napi_scope__ *parent)
      : napi_scope__(env, parent)
  {
  }
};

struct napi_escapable_handle_scope__ : napi_scope__
{
  napi_escapable_handle_scope__(napi_env env, napi_scope__ *parent)
      : napi_scope__(env, parent)
  {
  }

  bool escaped = false;
};

#endif // NAPI_QUICKJS_VALUE_H_
