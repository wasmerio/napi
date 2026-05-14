#ifndef NAPI_V8_LIFETIME_TRACKER_H_
#define NAPI_V8_LIFETIME_TRACKER_H_

#include "js_native_api.h"

#include <cstddef>
#include <v8.h>

struct napi_buffer_record__;
struct napi_deferred__;
struct napi_env__;
struct napi_env_cleanup_hook__;
struct napi_escapable_handle_scope__;
struct napi_external_backing_store_hint__;
struct napi_handle_scope__;
struct napi_ref__;

namespace v8impl::detail {

class napi_lifetime_tracker__ {
 public:
#ifdef NAPI_V8_ENABLE_LIFETIME_TRACKER
  static void record_create_raw(napi_env__* owner,
                                void* value,
                                const char* type_name);
  static void record_release_raw(napi_env__* owner,
                                 void* value,
                                 const char* type_name);
  static void record_scope_escape(napi_env__* env, bool succeeded);
  static void record_value(napi_env__* env,
                           v8::Local<v8::Value> value,
                           std::size_t parent_scope_depth = 0);
  static void record_scope_values_release(napi_env__* env, const void* scope);
#else
  static void record_create_raw(napi_env__* owner,
                                void* value,
                                const char* type_name) {
    (void)owner;
    (void)value;
    (void)type_name;
  }
  static void record_release_raw(napi_env__* owner,
                                 void* value,
                                 const char* type_name) {
    (void)owner;
    (void)value;
    (void)type_name;
  }
  static void record_scope_escape(napi_env__* env, bool succeeded) {
    (void)env;
    (void)succeeded;
  }
  static void record_value(napi_env__* env,
                           v8::Local<v8::Value> value,
                           std::size_t parent_scope_depth = 0) {
    (void)env;
    (void)value;
    (void)parent_scope_depth;
  }
  static void record_scope_values_release(napi_env__* env, const void* scope) {
    (void)env;
    (void)scope;
  }
#endif
  static void dump(napi_env__* env, const char* reason);
};

template <typename T>
struct napi_lifetime_type_name__ {
  static constexpr const char* value = "unknown";
};

template <>
struct napi_lifetime_type_name__<napi_ref__> {
  static constexpr const char* value = "napi_ref";
};

template <>
struct napi_lifetime_type_name__<napi_env_cleanup_hook__> {
  static constexpr const char* value = "napi_env_cleanup_hook";
};

template <>
struct napi_lifetime_type_name__<napi_deferred__> {
  static constexpr const char* value = "napi_deferred";
};

template <>
struct napi_lifetime_type_name__<napi_external_backing_store_hint__> {
  static constexpr const char* value = "napi_external_backing_store_hint";
};

template <>
struct napi_lifetime_type_name__<napi_handle_scope__> {
  static constexpr const char* value = "napi_handle_scope";
};

template <>
struct napi_lifetime_type_name__<napi_escapable_handle_scope__> {
  static constexpr const char* value = "napi_escapable_handle_scope";
};

template <>
struct napi_lifetime_type_name__<napi_buffer_record__> {
  static constexpr const char* value = "napi_buffer_record";
};

template <class T>
struct napi_lifetime__ {
  static void record_create(napi_env__* owner, T* val) {
    napi_lifetime_tracker__::record_create_raw(
        owner, val, napi_lifetime_type_name__<T>::value);
  }

  static void record_release(napi_env__* owner, T* val) {
    napi_lifetime_tracker__::record_release_raw(
        owner, val, napi_lifetime_type_name__<T>::value);
  }
};

}  // namespace v8impl::detail

extern "C" void napi_v8_lifetime_dump(napi_env__* env, const char* reason);

#endif  // NAPI_V8_LIFETIME_TRACKER_H_
