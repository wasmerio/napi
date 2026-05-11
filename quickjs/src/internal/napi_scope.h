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
  ~napi_scope__();

  void initialize(napi_env env, napi_handle_scope parent);
  void release();
  bool is_active() const;
  size_t level() const;

  napi_value wrap_value(JSValue value, bool owned);
  napi_value escape_value(napi_value value);
  void delete_value(napi_value value);
  napi_value__ *value_from_handle(napi_value value);

  napi_ref wrap_ref(JSValueConst value, uint32_t initial_ref_count);
  void delete_ref(napi_ref ref);
  napi_ref__ *ref_from_handle(napi_ref ref);
  void close();

  size_t value_slot_count() const;
  size_t value_storage_slot_count() const;
  size_t active_value_count() const;
  size_t ref_storage_slot_count() const;
  size_t active_ref_count() const;
  napi_handle_scope parent_handle() const;
  napi_scope__ *parent() const;
  napi_env env() const;
  bool has_escaped() const;
  void mark_escaped();

  template <typename Fn>
  void for_each_active_value(Fn fn) const
  {
    values_.for_each_active(fn);
  }

  template <typename Fn>
  void for_each_active_ref(Fn fn) const
  {
    refs_.for_each_active(fn);
  }

  napi_scope__();
  napi_scope__(napi_scope__ &&other) noexcept;
  napi_scope__ &operator=(napi_scope__ &&other) noexcept;

  napi_scope__(const napi_scope__ &) = delete;
  napi_scope__ &operator=(const napi_scope__ &) = delete;

private:
  napi_env env_ = nullptr;
  size_t level_ = 0;
  napi_handle_scope parent_ = nullptr;
  napi_allocator__<napi_value__> values_;
  napi_allocator__<napi_ref__> refs_;
  bool closed_ = false;
  bool active_ = false;
  bool escaped_ = false;
};

#endif // NAPI_QUICKJS_SCOPE_H_
