#ifndef NAPI_QUICKJS_REF_H_
#define NAPI_QUICKJS_REF_H_

#include "../../../include/js_native_api.h"

#include <cstddef>
#include <cstdint>
#include <quickjs.h>

struct napi_ref__
{
  napi_ref__() = default;
  napi_ref__(const napi_ref__ &) = delete;
  napi_ref__ &operator=(const napi_ref__ &) = delete;
  napi_ref__(napi_ref__ &&other) noexcept;
  napi_ref__ &operator=(napi_ref__ &&other) noexcept;
  ~napi_ref__();

  void initialize(napi_env env,
                  JSValueConst value,
                  uint32_t initial_ref_count);
  void release();
  bool is_active() const;
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
  napi_env env_;
  JSValue value_ = JS_UNDEFINED;
  bool can_be_weak_ = false;
  uint32_t ref_count_ = 0;
  bool active_ = false;
};

napi_ref__ *napi_quickjs_ref_slot(napi_env env, napi_ref ref);

#endif // NAPI_QUICKJS_REF_H_
