#include "internal/napi_env.h"

#include "internal/napi_external.h"
#include "internal/napi_lifetime_macros.h"

#include <algorithm>
#include <cstdint>
#include <cassert>

napi_env__::napi_env__(napi_agent__ *agent, JSContext *context, int32_t module_api_version)
    : agent_{agent},
      context_{context},
      module_api_version_{module_api_version},
      refs_{this},
      cleanup_hooks_{this},
      deferreds_{this},
      external_backing_stores_{this},
      buffers_{this, context},
      promises_{this, context},
      contextify_{this, context},
      module_wrap_{this, context}
{
  if (agent_->principal_env() == nullptr)
    agent_->set_principal_env(this);
  if (context_ != nullptr)
    agent_->open_root_scope(this);
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

  closing_ = true;
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

  const bool principal = is_principal();
  if (principal)
  {
    if (agent_->root_scope() != nullptr)
    {
      agent_->close_root_scope();
      JS_RunGC(agent_->runtime());
    }
    agent_->close_scopes();
    agent_->clear_module_hooks();
  }
  module_wrap_.teardown();
  contextify_.teardown();
  buffers_.teardown();
  if (principal)
    agent_->clear_promise_hooks();
  promises_.teardown();
  if (principal)
    agent_->clear_values();
  NAPI_QUICKJS_LIFETIME_DUMP(this, "napi_env__ teardown end");
  torn_down_ = true;
}

void napi_env__::clear_refs_for_teardown()
{
  clearing_refs_for_teardown_ = true;
  for (napi_ref__ &ref : refs_)
    ref.clear_for_teardown();
  clearing_refs_for_teardown_ = false;
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

bool napi_env__::is_closing() const
{
  return closing_;
}

napi_agent__ *napi_env__::agent() const
{
  return agent_;
}

void napi_env__::adopt_agent(std::unique_ptr<napi_agent__> agent)
{
  owned_agent_ = std::move(agent);
}

bool napi_env__::is_principal() const
{
  return agent_->principal_env() == this;
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
  return agent_->root_scope();
}

napi_handle_scope napi_env__::current_scope() const
{
  return agent_->current_scope();
}

napi_handle_scope napi_env__::create_scope(napi_handle_scope parent)
{
  if (context_ == nullptr)
    return nullptr;
  return agent_->create_scope(this, parent);
}

void napi_env__::destroy_scope(napi_handle_scope scope)
{
  agent_->destroy_scope(scope);
}

napi_scope__ *napi_env__::scope_from_handle(napi_handle_scope scope_handle) const
{
  return agent_->scope_from_handle(scope_handle);
}

JSValue napi_env__::wrap_external_data(void *data)
{
  return wrap_external_data(data, nullptr, nullptr);
}

JSValue napi_env__::wrap_external_data(void *data,
                                       node_api_basic_finalize finalize_cb,
                                       void *finalize_hint)
{
  JSContext *ctx = context_;
  if (ctx == nullptr)
    return JS_EXCEPTION;

  JSValue obj = JS_NewObjectClass(ctx, napi_external__::class_id());
  if (JS_IsException(obj))
    return obj;

  auto *hint = napi_external_backing_store_hint__::create(this, data, finalize_cb, finalize_hint);
  if (hint == nullptr)
  {
    JS_FreeValue(ctx, obj);
    JS_ThrowOutOfMemory(ctx);
    return JS_EXCEPTION;
  }

  JS_SetOpaque(obj, hint);
  return obj;
}

napi_value napi_env__::wrap_value_in_current_scope(JSValue value, bool owned)
{
  napi_scope__ *scope = agent_->scope_from_handle(agent_->current_scope());
  return scope == nullptr ? nullptr : scope->wrap_value(value, owned);
}

void napi_env__::delete_value_from_current_scope(napi_value value)
{
  napi_scope__ *scope = agent_->scope_from_handle(agent_->current_scope());
  if (scope != nullptr)
    scope->delete_value(value);
}

napi_value__ *napi_env__::value_from_handle(napi_value value_handle)
{
#ifndef NDEBUG
  auto owner = napi_scope__::ValuesAllocator::unsafe_owner(value_handle);
  assert(agent_->owns_scope(owner));
#endif

  return value_handle;
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
  if (ref == nullptr || !ref->is_active())
    return;
  if (clearing_refs_for_teardown_)
  {
    ref->clear_for_teardown();
    return;
  }
  refs_.destroy(ref);
}

napi_ref__ *napi_env__::ref_from_handle(napi_ref ref_handle)
{
  using Alloc = decltype(refs_);
#ifdef NDEBUG
  auto ref = ref_handle;
#else
  auto ref = ref_handle;
  auto owner = Alloc::unsafe_owner(ref_handle);
  assert(owner == this);
  assert(refs_.owns(ref_handle));
#endif

  return ref;
}

size_t napi_env__::ref_storage_slot_count() const
{
  return refs_.storage_slot_count();
}

size_t napi_env__::active_ref_count() const
{
  return refs_.count_active();
}

bool napi_env__::is_current_scope(napi_handle_scope scope) const
{
  return agent_->is_current_scope(scope);
}

void napi_env__::set_current_scope(napi_handle_scope scope)
{
  agent_->set_current_scope(scope);
}

size_t napi_env__::scope_storage_slot_count() const
{
  return agent_->scope_storage_slot_count();
}

size_t napi_env__::active_scope_count() const
{
  return agent_->active_scope_count();
}

#if defined(NAPI_ENABLE_LIFETIME_TRACKER) && defined(NAPI_ENABLE_LIFETIME_PERIODIC_STATS)
bool napi_env__::should_dump_lifetime_stats(int64_t now_ms)
{
  return lifetime_stats_gate_.should_fire(now_ms);
}

bool napi_env__::should_dump_lifetime_string_symbol_values(int64_t now_ms)
{
  return lifetime_string_symbol_values_gate_.should_fire(now_ms);
}
#endif

const napi_extended_error_info *napi_env__::last_error_info() const
{
  return error_state_.info();
}

napi_status napi_env__::set_last_error(napi_status status, const char *message)
{
  return error_state_.set(status, message);
}

napi_status napi_env__::clear_last_error()
{
  return error_state_.clear();
}

bool napi_env__::has_last_exception() const
{
  return agent_->has_pending_exception();
}

void napi_env__::clear_last_exception()
{
  agent_->clear_pending_exception();
}

void napi_env__::set_last_exception(JSValue exception)
{
  agent_->set_pending_exception(exception);
}

JSValue napi_env__::take_last_exception()
{
  return agent_->take_pending_exception();
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
  cleanup_hooks_.destroy(entry);
}

napi_deferred__ *napi_env__::create_deferred(JSValue resolve, JSValue reject)
{
  if (context_ == nullptr)
    return nullptr;
  return deferreds_.allocate(this, resolve, reject);
}

void napi_env__::destroy_deferred(napi_deferred__ *deferred)
{
  deferreds_.destroy(deferred);
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
  external_backing_stores_.destroy(hint);
}

int64_t napi_env__::adjust_external_memory(int64_t change_in_bytes)
{
  return agent_->adjust_external_memory(change_in_bytes);
}

bool napi_env__::attach_embedder_hooks(const unofficial_napi_env_hooks &hooks)
{
  return agent_->attach_embedder_hooks(hooks);
}

napi_promises__ &napi_env__::promises()
{
  return promises_;
}

const napi_promises__ &napi_env__::promises() const
{
  return promises_;
}

napi_buffer__ &napi_env__::buffers()
{
  return buffers_;
}

const napi_buffer__ &napi_env__::buffers() const
{
  return buffers_;
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
