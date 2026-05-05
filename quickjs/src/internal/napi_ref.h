#ifndef NAPI_QUICKJS_REF_H_
#define NAPI_QUICKJS_REF_H_

#include "../../../include/js_native_api.h"

#include <cstdint>
#include <quickjs.h>

struct napi_ref__
{
  static napi_ref__ *create(napi_env env, JSValueConst value, uint32_t initial_ref_count);
  static void destroy(napi_ref__ *ref);

  ~napi_ref__();

  uint32_t add_ref();
  uint32_t rem_ref();
  uint32_t ref_count() const;
  bool can_be_weak() const;
  bool is_empty() const;
  bool is_weak() const;
  JSValueConst get_inner() const;
  JSValue dup_inner() const;
  void clear_if_matches(JSValueConst value);

private:
  napi_ref__(napi_env env, JSValueConst value, uint32_t initial_ref_count);

  napi_env env_;
  JSValue value_;
  bool can_be_weak_;
  uint32_t ref_count_;
};

#endif // NAPI_QUICKJS_REF_H_
