#include "internal/napi_env.h"

#include "internal/napi_external.h"
#include "internal/napi_lifetime_macros.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <cassert>

napi_env__::napi_env__(JSContext *context, int32_t module_api_version)
    : context_{context},
      module_api_version_{module_api_version},
      last_exception_{JS_UNDEFINED},
      scopes_{this},
      refs_{this},
      cleanup_hooks_{this},
      deferreds_{this},
      external_backing_stores_{this},
      promises_{this, context},
      contextify_{this, context},
      module_wrap_{this, context}
{
  root_scope_ = create_scope(nullptr);
  current_scope_ = root_scope_;
  clear_last_error();
}

napi_env__::~napi_env__()
{
  prepare_teardown();
  finalize_instance_data();
}

void napi_env__::prepare_teardown()
{
  if (torn_down_)
    return;

  NAPI_QUICKJS_LIFETIME_DUMP(this, "napi_env__ teardown begin");
  while (!env_cleanup_hooks_.empty())
  {
    auto *entry = env_cleanup_hooks_.back();
    env_cleanup_hooks_.pop_back();
    if (entry != nullptr)
    {
      entry->run();
      napi_env_cleanup_hook__::destroy(entry);
    }
  }
  cleanup_hooks_.close();
  deferreds_.close();

  clear_last_exception();
  clear_refs_for_teardown();

  napi_scope__ *root_scope = scope_from_handle(root_scope_);
  if (root_scope != nullptr)
  {
    root_scope->close();
    if (context_ != nullptr)
      JS_RunGC(JS_GetRuntime(context_));
  }
  scopes_.close();
  current_scope_ = nullptr;
  root_scope_ = nullptr;
  module_wrap_.teardown();
  contextify_.teardown();
  promises_.teardown();
  NAPI_QUICKJS_LIFETIME_DUMP(this, "napi_env__ teardown end");
  torn_down_ = true;
}

void napi_env__::clear_refs_for_teardown()
{
  refs_.for_each_active([](const napi_ref__ &ref) {
    const_cast<napi_ref__ &>(ref).clear_for_teardown();
  });
}

void napi_env__::finalize_instance_data()
{
  void *data = instance_data_;
  napi_finalize finalize_cb = instance_data_finalize_cb_;
  void *finalize_hint = instance_data_finalize_hint_;

  instance_data_ = nullptr;
  instance_data_finalize_cb_ = nullptr;
  instance_data_finalize_hint_ = nullptr;

  if (data != nullptr && finalize_cb != nullptr)
    finalize_cb(this, data, finalize_hint);
}

JSContext *napi_env__::context() const
{
  return context_;
}

int32_t napi_env__::module_api_version() const
{
  return module_api_version_;
}

napi_handle_scope napi_env__::root_scope() const
{
  return root_scope_;
}

napi_handle_scope napi_env__::current_scope() const
{
  return current_scope_;
}

napi_handle_scope napi_env__::create_scope(napi_handle_scope parent)
{
  if (context_ == nullptr)
    return nullptr;
  auto *handle = scopes_.allocate(this, parent);
  return reinterpret_cast<napi_handle_scope>(handle);
}

void napi_env__::destroy_scope(napi_handle_scope scope)
{
  scopes_.release(scope);
}

napi_scope__ *napi_env__::scope_from_handle(napi_handle_scope scope_handle) const
{
#ifdef NDEBUG
  auto scope = napi_allocator__<napi_handle_scope, napi_scope__, napi_env__>::unsafe_data_from_handle(scope_handle);
#else
  assert(scopes_.owns_handle(scope_handle));
  auto [scope, owner] = napi_allocator__<napi_handle_scope, napi_scope__, napi_env__>::unsafe_data_with_owner_from_handle(scope_handle);
  assert(owner == this);
#endif

  return scope;
}

JSValue napi_env__::wrap_external_data(void *data)
{
  return wrap_external_data(data, nullptr, nullptr);
}

JSValue napi_env__::wrap_external_data(void *data,
                                       node_api_basic_finalize finalize_cb,
                                       void *finalize_hint)
{
  if (context_ == nullptr)
    return JS_EXCEPTION;

  JSValue obj = JS_NewObjectClass(context_, napi_external__::class_id());
  if (JS_IsException(obj))
    return obj;

  auto *hint = napi_external_backing_store_hint__::create(this, data, finalize_cb, finalize_hint);
  if (hint == nullptr)
  {
    JS_FreeValue(context_, obj);
    JS_ThrowOutOfMemory(context_);
    return JS_EXCEPTION;
  }

  JS_SetOpaque(obj, hint);
  return obj;
}

napi_value napi_env__::wrap_value_in_current_scope(JSValue value, bool owned)
{
  napi_scope__ *scope = scope_from_handle(current_scope_);
  return scope == nullptr ? nullptr : scope->wrap_value(value, owned);
}

void napi_env__::delete_value_from_current_scope(napi_value value)
{
  napi_scope__ *scope = scope_from_handle(current_scope_);
  if (scope != nullptr)
    scope->delete_value(value);
}

napi_value__ *napi_env__::value_from_handle(napi_value value_handle)
{
#ifdef NDEBUG
  auto value = napi_allocator__<napi_value, napi_value__, napi_scope__>::unsafe_data_from_handle(value_handle);
#else
  auto [value, owner] = napi_allocator__<napi_value, napi_value__, napi_scope__>::unsafe_data_with_owner_from_handle(value_handle);
  auto owner_handle = napi_allocator__<napi_handle_scope, napi_scope__, napi_env__>::unsafe_handle_from_data(owner);
  assert(scopes_.owns_handle(owner_handle));
#endif

  return value;
}

napi_ref napi_env__::wrap_ref_in_root_scope(JSValueConst value, uint32_t initial_ref_count)
{
  if (context_ == nullptr)
    return nullptr;

  napi_ref wrapped = refs_.allocate(this, value, initial_ref_count);
  return wrapped;
}

void napi_env__::delete_ref_from_root_scope(napi_ref ref)
{
  refs_.release(ref);
}

napi_ref__ *napi_env__::ref_from_handle(napi_ref ref_handle)
{
#ifdef NDEBUG
  auto ref = napi_allocator__<napi_ref, napi_ref__, napi_env__>::unsafe_data_from_handle(ref_handle);
#else
  auto [ref, owner] = napi_allocator__<napi_ref, napi_ref__, napi_env__>::unsafe_data_with_owner_from_handle(ref_handle);
  assert(owner == this);
  assert(refs_.owns_handle(ref_handle));
#endif

  return ref;
}

size_t napi_env__::ref_storage_slot_count() const
{
  return refs_.storage_slot_count();
}

size_t napi_env__::active_ref_count() const
{
  return refs_.active_count();
}

bool napi_env__::is_current_scope(napi_handle_scope scope) const
{
  return current_scope_ == scope;
}

void napi_env__::set_current_scope(napi_handle_scope scope)
{
  current_scope_ = scope;
}

size_t napi_env__::scope_storage_slot_count() const
{
  return scopes_.storage_slot_count();
}

size_t napi_env__::active_scope_count() const
{
  return scopes_.active_count();
}

#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS)
bool napi_env__::should_dump_lifetime_stats(int64_t now_ms)
{
  constexpr int64_t interval_ms = 2000;
  if (lifetime_last_stats_ms_ == 0)
  {
    lifetime_last_stats_ms_ = now_ms;
    return false;
  }

  if (now_ms - lifetime_last_stats_ms_ < interval_ms)
    return false;

  lifetime_last_stats_ms_ = now_ms;
  return true;
}

bool napi_env__::should_dump_lifetime_string_symbol_values(int64_t now_ms)
{
  constexpr int64_t interval_ms = 10000;
  if (lifetime_last_string_symbol_values_ms_ == 0)
  {
    lifetime_last_string_symbol_values_ms_ = now_ms;
    return false;
  }

  if (now_ms - lifetime_last_string_symbol_values_ms_ < interval_ms)
    return false;

  lifetime_last_string_symbol_values_ms_ = now_ms;
  return true;
}
#endif

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
  void *old_data = instance_data_;
  napi_finalize old_finalize_cb = instance_data_finalize_cb_;
  void *old_finalize_hint = instance_data_finalize_hint_;

  instance_data_ = nullptr;
  instance_data_finalize_cb_ = nullptr;
  instance_data_finalize_hint_ = nullptr;

  if (old_data != nullptr && old_finalize_cb != nullptr)
    old_finalize_cb(this, old_data, old_finalize_hint);

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

napi_env_cleanup_hook__ *napi_env__::create_cleanup_hook(napi_cleanup_hook hook, void *arg)
{
  if (context_ == nullptr || hook == nullptr)
    return nullptr;
  return cleanup_hooks_.allocate(this, hook, arg);
}

void napi_env__::destroy_cleanup_hook(napi_env_cleanup_hook__ *entry)
{
  cleanup_hooks_.release(entry);
}

napi_deferred__ *napi_env__::create_deferred(JSValue resolve, JSValue reject)
{
  if (context_ == nullptr)
    return nullptr;
  return deferreds_.allocate(this, resolve, reject);
}

void napi_env__::destroy_deferred(napi_deferred__ *deferred)
{
  deferreds_.release(deferred);
}

napi_external_backing_store_hint__ *napi_env__::create_external_backing_store(
    void *external_data,
    node_api_basic_finalize finalize_cb,
    void *finalize_hint)
{
  if (context_ == nullptr)
    return nullptr;
  return external_backing_stores_.allocate(this, external_data, finalize_cb, finalize_hint);
}

void napi_env__::destroy_external_backing_store(napi_external_backing_store_hint__ *hint)
{
  external_backing_stores_.release(hint);
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
