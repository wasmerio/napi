#ifndef NAPI_QUICKJS_ENV_H_
#define NAPI_QUICKJS_ENV_H_

#include "../../../include/js_native_api.h"
#include "../../../include/node_api_types.h"
#include "napi_callback_info.h"
#include "napi_contextify.h"
#include "napi_deferred.h"
#include "napi_env_cleanup_hook.h"
#include "napi_escapable_handle_scope.h"
#include "napi_handle_scope.h"
#include "napi_module_wrap.h"
#include "napi_promises.h"
#include "napi_ref.h"
#include "napi_scope.h"
#include "napi_value.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <quickjs.h>

struct napi_external_backing_store_hint__;

struct napi_env__
{
  explicit napi_env__(JSContext *context, int32_t module_api_version);
  ~napi_env__();

  void prepare_teardown();

  JSContext *context() const;
  int32_t module_api_version() const;

  napi_scope__ *root_scope() const;
  napi_scope__ *current_scope() const;
  bool is_current_scope(napi_scope__ *scope) const;
  void set_current_scope(napi_scope__ *scope);

  const napi_extended_error_info *last_error_info() const;
  napi_status set_last_error(napi_status status, const char *message);
  napi_status clear_last_error();

  bool has_last_exception() const;
  void clear_last_exception();
  void set_last_exception(JSValue exception);
  JSValue take_last_exception();

  void *instance_data() const;
  void set_instance_data(void *data, napi_finalize finalize_cb, void *finalize_hint);

  napi_status add_cleanup_hook(napi_cleanup_hook hook, void *arg);
  napi_status remove_cleanup_hook(napi_cleanup_hook hook, void *arg);

  void track_weak_ref(napi_ref ref);
  void remove_weak_ref(napi_ref ref);
  void clear_weak_refs_for_value(JSValueConst value);
  void track_external_array_buffer_hint(JSValueConst arraybuffer, napi_external_backing_store_hint__ *hint);
  napi_external_backing_store_hint__ *external_array_buffer_hint(JSValueConst arraybuffer) const;
  void untrack_external_array_buffer_hint(napi_external_backing_store_hint__ *hint);

  int64_t adjust_external_memory(int64_t change_in_bytes);

  napi_promises__ &promises();
  const napi_promises__ &promises() const;
  quickjs::detail::napi_contextify__ &contextify();
  const quickjs::detail::napi_contextify__ &contextify() const;
  quickjs::detail::napi_module_wrap__ &module_wrap();
  const quickjs::detail::napi_module_wrap__ &module_wrap() const;

private:
  JSContext *context_;
  napi_extended_error_info last_error_{};
  std::string last_error_message_;
  JSValue last_exception_;
  bool has_last_exception_ = false;
  int32_t module_api_version_ = 8;
  void *instance_data_ = nullptr;
  napi_finalize instance_data_finalize_cb_ = nullptr;
  void *instance_data_finalize_hint_ = nullptr;
  std::vector<napi_env_cleanup_hook__ *> env_cleanup_hooks_;
  std::vector<napi_ref> weak_refs_;
  std::vector<std::pair<void *, napi_external_backing_store_hint__ *>> external_array_buffer_hints_;
  napi_scope__ *root_scope_ = nullptr;
  napi_scope__ *current_scope_ = nullptr;
  int64_t external_memory_ = 0;
  napi_promises__ promises_;
  quickjs::detail::napi_contextify__ contextify_;
  quickjs::detail::napi_module_wrap__ module_wrap_;
  bool torn_down_ = false;
};

napi_status napi_quickjs_set_last_error(napi_env env,
                                        napi_status status,
                                        const char *message);

napi_status napi_quickjs_clear_last_error(napi_env env);

#endif // NAPI_QUICKJS_ENV_H_
