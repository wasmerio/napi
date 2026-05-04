#include "internal/napi_env.h"

#include <algorithm>
#include <limits>

napi_env__::napi_env__(JSContext *context, int32_t module_api_version)
    : context_(context),
      last_exception_(JS_UNDEFINED),
      module_api_version_(module_api_version)
{
  root_scope_ = napi_scope__::create(this, nullptr);
  current_scope_ = root_scope_;
  clear_last_error();
}

napi_env__::~napi_env__()
{
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

  root_scope_ = nullptr;
  current_scope_ = nullptr;
}

JSContext *napi_env__::context() const
{
  return context_;
}

int32_t napi_env__::module_api_version() const
{
  return module_api_version_;
}

napi_scope__ *napi_env__::root_scope() const
{
  return root_scope_;
}

napi_scope__ *napi_env__::current_scope() const
{
  return current_scope_;
}

bool napi_env__::is_current_scope(napi_scope__ *scope) const
{
  return current_scope_ == scope;
}

void napi_env__::set_current_scope(napi_scope__ *scope)
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
  if (ref == nullptr || !ref->is_weak())
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
    if (ref != nullptr)
      ref->clear_if_matches(value);
  }
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
