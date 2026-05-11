#include "internal/napi_env.h"

#include "internal/napi_lifetime_macros.h"

#include <algorithm>
#include <cstdint>
#include <limits>

napi_env__::napi_env__(JSContext *context, int32_t module_api_version)
    : context_(context),
      last_exception_(JS_UNDEFINED),
      module_api_version_(module_api_version),
      promises_(this, context),
      contextify_(this, context),
      module_wrap_(this, context)
{
  NAPI_QUICKJS_LIFETIME_RECORD(create, env, this, this);
  root_scope_ = create_scope(nullptr);
  current_scope_ = root_scope_;
  clear_last_error();
}

napi_env__::~napi_env__()
{
  prepare_teardown();
}

void napi_env__::prepare_teardown()
{
  if (torn_down_)
    return;

  NAPI_QUICKJS_LIFETIME_DUMP("napi_env__ teardown begin");
  for (auto it = env_cleanup_hooks_.rbegin(); it != env_cleanup_hooks_.rend(); ++it)
  {
    auto *entry = *it;
    if (entry != nullptr)
    {
      entry->run();
      napi_env_cleanup_hook__::destroy(entry);
    }
  }
  env_cleanup_hooks_.clear();

  if (instance_data_ != nullptr && instance_data_finalize_cb_ != nullptr)
    instance_data_finalize_cb_(this, instance_data_, instance_data_finalize_hint_);

  clear_last_exception();

  napi_scope__ *root_scope = this->root_scope_value();
  if (root_scope != nullptr)
  {
    root_scope->close();
    if (context_ != nullptr)
      JS_RunGC(JS_GetRuntime(context_));
  }
  scopes_.close();
  current_scope_ = nullptr;
  root_scope_ = nullptr;
  weak_refs_.clear();
  module_wrap_.teardown();
  contextify_.teardown();
  promises_.teardown();
  NAPI_QUICKJS_LIFETIME_RECORD(destroy, env, this, this);
  NAPI_QUICKJS_LIFETIME_DUMP("napi_env__ teardown end");
  torn_down_ = true;
}

JSContext *napi_env__::context() const
{
  return context_;
}

int32_t napi_env__::module_api_version() const
{
  return module_api_version_;
}

napi_scope_handle__ napi_env__::root_scope() const
{
  return root_scope_;
}

napi_scope_handle__ napi_env__::current_scope() const
{
  return current_scope_;
}

napi_scope__ *napi_env__::root_scope_value() const
{
  return scope_from_handle(root_scope_);
}

napi_scope__ *napi_env__::current_scope_value() const
{
  return scope_from_handle(current_scope_);
}

napi_scope_handle__ napi_env__::create_scope(napi_scope_handle__ parent)
{
  if (context_ == nullptr)
    return nullptr;
  auto *handle = scopes_.allocate(this, parent);
  napi_scope__ *scope = scopes_.get(handle);
  if (scope != nullptr)
    scope->set_index(reinterpret_cast<uintptr_t>(handle) - 1);
  return static_cast<napi_scope_handle__>(handle);
}

void napi_env__::destroy_scope(napi_scope_handle__ scope)
{
  scopes_.release(static_cast<napi_scope__ *>(scope));
}

napi_scope__ *napi_env__::scope_from_handle(napi_scope_handle__ scope) const
{
  return const_cast<napi_allocator__<napi_scope__> &>(scopes_).get(
      static_cast<napi_scope__ *>(scope));
}

bool napi_env__::is_current_scope(napi_scope_handle__ scope) const
{
  return current_scope_ == scope;
}

void napi_env__::set_current_scope(napi_scope_handle__ scope)
{
  current_scope_ = scope;
}

const napi_extended_error_info *napi_env__::last_error_info() const
{
  return &last_error_;
}

napi_status napi_env__::set_last_error(napi_status status, const char *message)
{
  last_error_.error_code = status;
  last_error_.engine_error_code = 0;
  last_error_.engine_reserved = nullptr;
  last_error_message_ = (message == nullptr) ? "" : message;
  last_error_.error_message =
      last_error_message_.empty() ? nullptr : last_error_message_.c_str();
  return status;
}

napi_status napi_env__::clear_last_error()
{
  return set_last_error(napi_ok, nullptr);
}

bool napi_env__::has_last_exception() const
{
  return has_last_exception_;
}

void napi_env__::clear_last_exception()
{
  if (!has_last_exception_)
    return;

  JS_FreeValue(context_, last_exception_);
  last_exception_ = JS_UNDEFINED;
  has_last_exception_ = false;
}

void napi_env__::set_last_exception(JSValue exception)
{
  clear_last_exception();
  last_exception_ = exception;
  has_last_exception_ = true;
}

JSValue napi_env__::take_last_exception()
{
  if (!has_last_exception_)
    return JS_UNDEFINED;

  JSValue exception = last_exception_;
  last_exception_ = JS_UNDEFINED;
  has_last_exception_ = false;
  return exception;
}

void *napi_env__::instance_data() const
{
  return instance_data_;
}

void napi_env__::set_instance_data(void *data, napi_finalize finalize_cb, void *finalize_hint)
{
  if (instance_data_ != nullptr && instance_data_finalize_cb_ != nullptr)
    instance_data_finalize_cb_(this, instance_data_, instance_data_finalize_hint_);

  instance_data_ = data;
  instance_data_finalize_cb_ = finalize_cb;
  instance_data_finalize_hint_ = finalize_hint;
}

napi_status napi_env__::add_cleanup_hook(napi_cleanup_hook hook, void *arg)
{
  auto *entry = napi_env_cleanup_hook__::create(this, hook, arg);
  if (entry == nullptr)
    return napi_generic_failure;

  env_cleanup_hooks_.push_back(entry);
  return napi_ok;
}

napi_status napi_env__::remove_cleanup_hook(napi_cleanup_hook hook, void *arg)
{
  for (auto it = env_cleanup_hooks_.begin(); it != env_cleanup_hooks_.end(); ++it)
  {
    auto *entry = *it;
    if (entry != nullptr && entry->matches(hook, arg))
    {
      napi_env_cleanup_hook__::destroy(entry);
      env_cleanup_hooks_.erase(it);
      return napi_ok;
    }
  }
  return napi_invalid_arg;
}

void napi_env__::track_weak_ref(napi_ref ref)
{
  napi_ref__ *slot = napi_quickjs_ref_slot(this, ref);
  if (slot == nullptr || !slot->is_weak())
    return;

  if (std::find(weak_refs_.begin(), weak_refs_.end(), ref) == weak_refs_.end())
    weak_refs_.push_back(ref);
}

void napi_env__::remove_weak_ref(napi_ref ref)
{
  auto it = std::find(weak_refs_.begin(), weak_refs_.end(), ref);
  if (it != weak_refs_.end())
    weak_refs_.erase(it);
}

void napi_env__::clear_weak_refs_for_value(JSValueConst value)
{
  if (!JS_VALUE_HAS_REF_COUNT(value))
    return;

  for (auto *ref : weak_refs_)
  {
    napi_ref__ *slot = napi_quickjs_ref_slot(this, ref);
    if (slot != nullptr)
      slot->clear_if_matches(value);
  }
}

void napi_env__::track_external_array_buffer_hint(
    JSValueConst arraybuffer,
    napi_external_backing_store_hint__ *hint)
{
  if (hint == nullptr)
    return;
  external_array_buffer_hints_.push_back({JS_VALUE_GET_PTR(arraybuffer), hint});
}

napi_external_backing_store_hint__ *napi_env__::external_array_buffer_hint(JSValueConst arraybuffer) const
{
  void *identity = JS_VALUE_GET_PTR(arraybuffer);
  for (auto it = external_array_buffer_hints_.rbegin();
       it != external_array_buffer_hints_.rend();
       ++it)
  {
    if (it->first == identity)
      return it->second;
  }
  return nullptr;
}

void napi_env__::untrack_external_array_buffer_hint(napi_external_backing_store_hint__ *hint)
{
  external_array_buffer_hints_.erase(
      std::remove_if(external_array_buffer_hints_.begin(),
                     external_array_buffer_hints_.end(),
                     [hint](const auto &entry) { return entry.second == hint; }),
      external_array_buffer_hints_.end());
}

int64_t napi_env__::adjust_external_memory(int64_t change_in_bytes)
{
  if (change_in_bytes > 0 &&
      external_memory_ > std::numeric_limits<int64_t>::max() - change_in_bytes)
  {
    external_memory_ = std::numeric_limits<int64_t>::max();
  }
  else if (change_in_bytes < 0 &&
           external_memory_ < std::numeric_limits<int64_t>::min() - change_in_bytes)
  {
    external_memory_ = std::numeric_limits<int64_t>::min();
  }
  else
  {
    external_memory_ += change_in_bytes;
  }
  return external_memory_;
}

napi_promises__ &napi_env__::promises()
{
  return promises_;
}

const napi_promises__ &napi_env__::promises() const
{
  return promises_;
}

quickjs::detail::napi_contextify__ &napi_env__::contextify()
{
  return contextify_;
}

const quickjs::detail::napi_contextify__ &napi_env__::contextify() const
{
  return contextify_;
}

quickjs::detail::napi_module_wrap__ &napi_env__::module_wrap()
{
  return module_wrap_;
}

const quickjs::detail::napi_module_wrap__ &napi_env__::module_wrap() const
{
  return module_wrap_;
}

napi_status napi_quickjs_set_last_error(napi_env env,
                                        napi_status status,
                                        const char *message)
{
  if (env == nullptr)
    return status;
  return env->set_last_error(status, message);
}

napi_status napi_quickjs_clear_last_error(napi_env env)
{
  if (env == nullptr)
    return napi_ok;
  return env->clear_last_error();
}
