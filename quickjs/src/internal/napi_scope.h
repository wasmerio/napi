#ifndef NAPI_QUICKJS_SCOPE_H_
#define NAPI_QUICKJS_SCOPE_H_

#include "../../../include/js_native_api.h"
#include "napi_allocator.h"
#include "napi_ref.h"
#include "napi_value.h"

#include <cstdint>
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
  napi_value__ *value_from_handle(napi_value value);

  napi_ref wrap_ref(JSValueConst value, uint32_t initial_ref_count);
  void delete_ref(napi_ref ref);
  napi_ref__ *ref_from_handle(napi_ref ref);
  void close();

  size_t value_slot_count() const;
  napi_scope__ *parent() const;
  napi_env env() const;

protected:
  napi_scope__(napi_env env, napi_scope__ *parent);

private:
  napi_env env_;
  napi_scope__ *parent_;
  napi_allocator__<napi_value__> values_;
  napi_allocator__<napi_ref__> refs_;
  bool closed_ = false;
};

#endif // NAPI_QUICKJS_SCOPE_H_
