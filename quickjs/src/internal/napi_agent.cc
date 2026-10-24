#include "internal/napi_agent.h"

#include "internal/napi_env.h"
#include "internal/napi_lifetime_macros.h"
#include "internal/napi_util.h"

#include <cassert>
#include <limits>

namespace
{
void *js_identity(JSValueConst value)
{
  return JS_IsObject(value) ? JS_VALUE_GET_PTR(value) : nullptr;
}

// Replaces *target with `value` when it is a function, undefined or null.
napi_status set_optional_function(napi_env env, napi_value value, JSValue *target)
{
  JSContext *ctx = env->context();
  JSValueConst raw = value == nullptr ? JS_UNDEFINED : napi_quickjs_value_inner(env, value);
  if (JS_IsUndefined(raw) || JS_IsNull(raw))
  {
    JS_FreeValue(ctx, *target);
    *target = JS_UNDEFINED;
    return napi_ok;
  }
  if (!JS_IsFunction(ctx, raw))
    return napi_function_expected;
  JS_FreeValue(ctx, *target);
  *target = JS_DupValue(ctx, raw);
  return napi_ok;
}
} // namespace

napi_agent__::napi_agent__(JSRuntime *runtime)
    : runtime_{runtime},
      scopes_{this},
      refs_{this},
      cleanup_hook_slots_{this},
      deferreds_{this},
      external_backing_stores_{this}
{
  clear_last_error();
}

napi_agent__::~napi_agent__() = default;

JSRuntime *napi_agent__::runtime() const
{
  return runtime_;
}

napi_env napi_agent__::add_realm(JSContext *context)
{
  auto *env = new (std::nothrow) napi_env__{this, context};
  if (env == nullptr)
    return nullptr;
  envs_.emplace_back(env);
  JS_SetContextOpaque(context, env);
  JSValue global = JS_GetGlobalObject(context);
  set_realm_global(global, env);
  JS_FreeValue(context, global);

  if (envs_.size() == 1)
  {
    root_scope_ = create_scope(env, nullptr);
    current_scope_ = root_scope_;
    buffers_ = std::make_unique<napi_buffer__>(env, context);
    contextify_ = std::make_unique<quickjs::detail::napi_contextify__>(env, context);
    module_wrap_ = std::make_unique<quickjs::detail::napi_module_wrap__>(env, context);
    JS_SetPromiseHook(runtime_, napi_agent__::promise_hook, this);
    JS_SetModuleImportMetaInitFunc(runtime_, napi_agent__::import_meta_init, this);
    JS_SetModuleDynamicImportFunc(runtime_, napi_agent__::dynamic_import, this);
  }
  return env;
}

napi_env napi_agent__::principal_env() const
{
  return envs_.empty() ? nullptr : envs_.front().get();
}

napi_env napi_agent__::env_for_context(JSContext *ctx) const
{
  if (ctx != nullptr)
  {
    auto *env = static_cast<napi_env>(JS_GetContextOpaque(ctx));
    if (env != nullptr && env->agent() == this)
      return env;
  }
  return principal_env();
}

napi_env napi_agent__::env_for_global(JSValueConst object) const
{
  auto it = realm_globals_.find(js_identity(object));
  return it == realm_globals_.end() ? nullptr : it->second;
}

// The first realm to claim an object keeps it: a principal global used as a
// sandbox still finds the principal realm.
void napi_agent__::set_realm_global(JSValueConst object, napi_env env)
{
  if (js_identity(object) != nullptr)
    realm_globals_.emplace(js_identity(object), env);
}

void napi_agent__::teardown()
{
  if (closing_)
    return;
  closing_ = true;
  JS_SetPromiseHook(runtime_, nullptr, nullptr);
  JS_SetHostPromiseRejectionTracker(runtime_, nullptr, nullptr);
  napi_env principal = principal_env();
  NAPI_QUICKJS_LIFETIME_DUMP(principal, "napi_agent__ teardown begin");

  while (!cleanup_hooks_.empty())
  {
    auto *entry = cleanup_hooks_.back();
    cleanup_hooks_.pop_back();
    entry->run();
    napi_env_cleanup_hook__::destroy(entry);
  }
  cleanup_hook_slots_.close();
  deferreds_.close();

  clear_pending_exception();
  clearing_refs_ = true;
  for (napi_ref__ &ref : refs_)
    ref.clear_for_teardown();
  clearing_refs_ = false;

  if (napi_scope__ *root = scope_from_handle(root_scope_))
  {
    root->close();
    JS_RunGC(runtime_);
  }
  scopes_.close();
  root_scope_ = nullptr;
  current_scope_ = nullptr;

  JS_SetModuleImportMetaInitFunc(runtime_, nullptr, nullptr);
  JS_SetModuleDynamicImportFunc(runtime_, nullptr, nullptr);
  if (module_wrap_ != nullptr)
    module_wrap_->teardown();
  // Frees the JSContexts of the realms create_realm made.
  if (contextify_ != nullptr)
    contextify_->teardown();
  if (buffers_ != nullptr)
    buffers_->teardown();

  JS_FreeValueRT(runtime_, promise_reject_callback_);
  promise_reject_callback_ = JS_UNDEFINED;
  for (JSValue &hook : promise_hooks_)
  {
    JS_FreeValueRT(runtime_, hook);
    hook = JS_UNDEFINED;
  }
  JS_FreeValueRT(runtime_, continuation_preserved_embedder_data_);
  continuation_preserved_embedder_data_ = JS_UNDEFINED;
  for (auto &entry : promise_context_frames_)
    JS_FreeValueRT(runtime_, entry.second);
  promise_context_frames_.clear();
  for (JSValue frame : promise_context_frame_stack_)
    JS_FreeValueRT(runtime_, frame);
  promise_context_frame_stack_.clear();
  realm_globals_.clear();
  NAPI_QUICKJS_LIFETIME_DUMP(principal, "napi_agent__ teardown end");

  // The realms' contexts, newest first.
  for (auto it = envs_.rbegin(); it != envs_.rend(); ++it)
  {
    JS_SetContextOpaque((*it)->context(), nullptr);
    JS_FreeContext((*it)->context());
  }
}

bool napi_agent__::is_closing() const
{
  return closing_;
}

void napi_agent__::finalize_instance_data()
{
  set_instance_data(nullptr, nullptr, nullptr);
}

napi_handle_scope napi_agent__::root_scope() const
{
  return root_scope_;
}

napi_handle_scope napi_agent__::current_scope() const
{
  return current_scope_;
}

void napi_agent__::set_current_scope(napi_handle_scope scope)
{
  current_scope_ = scope;
}

bool napi_agent__::is_current_scope(napi_handle_scope scope) const
{
  return current_scope_ == scope;
}

napi_handle_scope napi_agent__::create_scope(napi_env env, napi_handle_scope parent)
{
  return reinterpret_cast<napi_handle_scope>(scopes_.allocate(env, parent));
}

void napi_agent__::destroy_scope(napi_handle_scope scope)
{
  scopes_.destroy(reinterpret_cast<napi_scope__ *>(scope));
}

napi_scope__ *napi_agent__::scope_from_handle(napi_handle_scope scope_handle) const
{
  auto scope = reinterpret_cast<napi_scope__ *>(scope_handle);
#ifndef NDEBUG
  if (scope != nullptr)
    assert(scopes_.owns(scope));
#endif
  return scope;
}

bool napi_agent__::owns_scope(napi_scope__ *scope) const
{
  return scopes_.owns(scope);
}

size_t napi_agent__::scope_storage_slot_count() const
{
  return scopes_.storage_slot_count();
}

size_t napi_agent__::active_scope_count() const
{
  return scopes_.count_active();
}

napi_value napi_agent__::wrap_value_in_current_scope(JSValue value, bool owned)
{
  napi_scope__ *scope = scope_from_handle(current_scope_);
  return scope == nullptr ? nullptr : scope->wrap_value(value, owned);
}

void napi_agent__::delete_value_from_current_scope(napi_value value)
{
  if (napi_scope__ *scope = scope_from_handle(current_scope_))
    scope->delete_value(value);
}

napi_value__ *napi_agent__::value_from_handle(napi_value value_handle) const
{
#ifndef NDEBUG
  assert(owns_scope(napi_scope__::ValuesAllocator::unsafe_owner(value_handle)));
#endif
  return value_handle;
}

napi_ref napi_agent__::create_ref(napi_env env, JSValueConst value, uint32_t initial_ref_count)
{
  return refs_.allocate(env, value, initial_ref_count);
}

void napi_agent__::delete_ref(napi_ref ref)
{
  if (ref == nullptr || !ref->is_active())
    return;
  if (clearing_refs_)
  {
    ref->clear_for_teardown();
    return;
  }
  refs_.destroy(ref);
}

napi_ref__ *napi_agent__::ref_from_handle(napi_ref ref)
{
#ifndef NDEBUG
  assert(refs_.owns(ref));
#endif
  return ref;
}

size_t napi_agent__::ref_storage_slot_count() const
{
  return refs_.storage_slot_count();
}

size_t napi_agent__::active_ref_count() const
{
  return refs_.count_active();
}

napi_env_cleanup_hook__ *napi_agent__::create_cleanup_hook(napi_env env,
                                                           napi_cleanup_hook hook,
                                                           void *arg)
{
  return hook == nullptr ? nullptr : cleanup_hook_slots_.allocate(env, hook, arg);
}

void napi_agent__::destroy_cleanup_hook(napi_env_cleanup_hook__ *entry)
{
  cleanup_hook_slots_.destroy(entry);
}

napi_deferred__ *napi_agent__::create_deferred(napi_env env, JSValue resolve, JSValue reject)
{
  return deferreds_.allocate(env, resolve, reject);
}

void napi_agent__::destroy_deferred(napi_deferred__ *deferred)
{
  deferreds_.destroy(deferred);
}

napi_external_backing_store_hint__ *napi_agent__::create_external_backing_store(
    napi_env env,
    void *external_data,
    node_api_basic_finalize finalize_cb,
    void *finalize_hint)
{
  return external_backing_stores_.allocate(env, external_data, finalize_cb, finalize_hint);
}

void napi_agent__::destroy_external_backing_store(napi_external_backing_store_hint__ *hint)
{
  external_backing_stores_.destroy(hint);
}

bool napi_agent__::has_pending_exception() const
{
  return has_pending_exception_;
}

void napi_agent__::set_pending_exception(JSValue exception)
{
  clear_pending_exception();
  pending_exception_ = exception;
  has_pending_exception_ = true;
}

JSValue napi_agent__::take_pending_exception()
{
  if (!has_pending_exception_)
    return JS_UNDEFINED;

  JSValue exception = pending_exception_;
  pending_exception_ = JS_UNDEFINED;
  has_pending_exception_ = false;
  return exception;
}

void napi_agent__::clear_pending_exception()
{
  if (!has_pending_exception_)
    return;

  JS_FreeValueRT(runtime_, pending_exception_);
  pending_exception_ = JS_UNDEFINED;
  has_pending_exception_ = false;
}

const napi_extended_error_info *napi_agent__::last_error_info() const
{
  return error_state_.info();
}

napi_status napi_agent__::set_last_error(napi_status status, const char *message)
{
  return error_state_.set(status, message);
}

napi_status napi_agent__::clear_last_error()
{
  return error_state_.clear();
}

void *napi_agent__::instance_data() const
{
  return instance_data_;
}

void napi_agent__::set_instance_data(void *data, napi_finalize finalize_cb, void *finalize_hint)
{
  void *old_data = instance_data_;
  napi_finalize old_finalize_cb = instance_data_finalize_cb_;
  void *old_finalize_hint = instance_data_finalize_hint_;

  instance_data_ = nullptr;
  instance_data_finalize_cb_ = nullptr;
  instance_data_finalize_hint_ = nullptr;
  if (old_data != nullptr && old_finalize_cb != nullptr)
    old_finalize_cb(principal_env(), old_data, old_finalize_hint);

  instance_data_ = data;
  instance_data_finalize_cb_ = finalize_cb;
  instance_data_finalize_hint_ = finalize_hint;
}

napi_status napi_agent__::add_cleanup_hook(napi_env env, napi_cleanup_hook hook, void *arg)
{
  auto *entry = napi_env_cleanup_hook__::create(env, hook, arg);
  if (entry == nullptr)
    return napi_generic_failure;

  cleanup_hooks_.push_back(entry);
  return napi_ok;
}

napi_status napi_agent__::remove_cleanup_hook(napi_cleanup_hook hook, void *arg)
{
  for (auto it = cleanup_hooks_.begin(); it != cleanup_hooks_.end(); ++it)
  {
    if ((*it)->matches(hook, arg))
    {
      napi_env_cleanup_hook__::destroy(*it);
      cleanup_hooks_.erase(it);
      return napi_ok;
    }
  }
  return napi_invalid_arg;
}

int64_t napi_agent__::adjust_external_memory(int64_t change_in_bytes)
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

bool napi_agent__::attach_embedder_hooks(const unofficial_napi_env_hooks &hooks)
{
  if (embedder_hooks_attached_)
    return false;
  embedder_hooks_ = hooks;
  embedder_hooks_attached_ = true;
  return true;
}

napi_status napi_agent__::set_promise_reject_callback(napi_env env, napi_value callback)
{
  napi_status status = set_optional_function(env, callback, &promise_reject_callback_);
  if (status != napi_ok)
    return status;
  JS_SetHostPromiseRejectionTracker(
      runtime_,
      JS_IsUndefined(promise_reject_callback_) ? nullptr : napi_agent__::rejection_tracker,
      this);
  return napi_ok;
}

napi_status napi_agent__::set_promise_hooks(napi_env env,
                                            napi_value init,
                                            napi_value before,
                                            napi_value after,
                                            napi_value resolve)
{
  // All four are checked before any is replaced.
  for (napi_value callback : {init, before, after, resolve})
  {
    JSValueConst raw = callback == nullptr ? JS_UNDEFINED : napi_quickjs_value_inner(env, callback);
    if (!JS_IsUndefined(raw) && !JS_IsNull(raw) && !JS_IsFunction(env->context(), raw))
      return napi_function_expected;
  }
  napi_value callbacks[] = {init, before, after, resolve};
  for (size_t i = 0; i < promise_hooks_.size(); ++i)
    set_optional_function(env, callbacks[i], &promise_hooks_[i]);
  return napi_ok;
}

JSValueConst napi_agent__::continuation_preserved_embedder_data() const
{
  return continuation_preserved_embedder_data_;
}

void napi_agent__::set_continuation_preserved_embedder_data(JSValueConst value)
{
  replace_stored_value(&continuation_preserved_embedder_data_, value);
}

napi_buffer__ &napi_agent__::buffers()
{
  return *buffers_;
}

quickjs::detail::napi_contextify__ &napi_agent__::contextify()
{
  return *contextify_;
}

quickjs::detail::napi_module_wrap__ &napi_agent__::module_wrap()
{
  return *module_wrap_;
}

#if defined(NAPI_ENABLE_LIFETIME_TRACKER) && defined(NAPI_ENABLE_LIFETIME_PERIODIC_STATS)
bool napi_agent__::should_dump_lifetime_stats(int64_t now_ms)
{
  return lifetime_stats_gate_.should_fire(now_ms);
}

bool napi_agent__::should_dump_lifetime_string_symbol_values(int64_t now_ms)
{
  return lifetime_string_symbol_values_gate_.should_fire(now_ms);
}
#endif

void napi_agent__::capture_context_frame(JSValueConst promise)
{
  void *identity = js_identity(promise);
  if (identity == nullptr || JS_IsUndefined(continuation_preserved_embedder_data_))
    return;

  JSValue frame = JS_DupValueRT(runtime_, continuation_preserved_embedder_data_);
  auto it = promise_context_frames_.find(identity);
  if (it != promise_context_frames_.end())
  {
    JS_FreeValueRT(runtime_, it->second);
    it->second = frame;
    return;
  }
  promise_context_frames_.emplace(identity, frame);
}

void napi_agent__::enter_context_frame(JSValueConst promise)
{
  promise_context_frame_stack_.push_back(
      JS_DupValueRT(runtime_, continuation_preserved_embedder_data_));

  JSValueConst frame = JS_UNDEFINED;
  void *identity = js_identity(promise);
  if (identity != nullptr)
  {
    auto it = promise_context_frames_.find(identity);
    if (it != promise_context_frames_.end())
      frame = it->second;
  }
  replace_stored_value(&continuation_preserved_embedder_data_, frame);
}

void napi_agent__::leave_context_frame(JSValueConst promise)
{
  (void)promise;
  if (promise_context_frame_stack_.empty())
    return;
  JSValue previous = promise_context_frame_stack_.back();
  promise_context_frame_stack_.pop_back();
  JS_FreeValueRT(runtime_, continuation_preserved_embedder_data_);
  continuation_preserved_embedder_data_ = previous;
}

void napi_agent__::release_context_frame(JSValueConst promise)
{
  void *identity = js_identity(promise);
  if (identity == nullptr)
    return;

  auto it = promise_context_frames_.find(identity);
  if (it != promise_context_frames_.end())
  {
    JS_FreeValueRT(runtime_, it->second);
    promise_context_frames_.erase(it);
  }
}

void napi_agent__::replace_stored_value(JSValue *target, JSValueConst value)
{
  JS_FreeValueRT(runtime_, *target);
  *target = JS_DupValueRT(runtime_, value);
}

// Calls a hook with the realm that triggered it as `this`. An exception it
// throws is dropped.
void napi_agent__::call_hook(JSContext *ctx, JSValueConst hook, int argc, JSValueConst *argv)
{
  if (closing_ || !JS_IsFunction(ctx, hook))
    return;
  // The hook may replace itself while it runs.
  JSValue function = JS_DupValue(ctx, hook);
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ret = JS_Call(ctx, function, global, argc, argv);
  JS_FreeValue(ctx, global);
  JS_FreeValue(ctx, function);
  if (JS_IsException(ret))
    JS_FreeValue(ctx, JS_GetException(ctx));
  else
    JS_FreeValue(ctx, ret);
}

void napi_agent__::promise_hook(JSContext *ctx,
                                JSPromiseHookType type,
                                JSValueConst promise,
                                JSValueConst parent_promise,
                                void *opaque)
{
  auto *agent = static_cast<napi_agent__ *>(opaque);
  if (agent == nullptr || ctx == nullptr || JS_GetRuntime(ctx) != agent->runtime_)
    return;

  if (type == JS_PROMISE_HOOK_INIT)
    agent->capture_context_frame(promise);
  else if (type == JS_PROMISE_HOOK_BEFORE)
    agent->enter_context_frame(promise);

  JSValueConst argv[] = {promise, parent_promise};
  agent->call_hook(ctx, agent->promise_hooks_[static_cast<size_t>(type)],
                   type == JS_PROMISE_HOOK_INIT ? 2 : 1, argv);

  if (type == JS_PROMISE_HOOK_AFTER)
    agent->leave_context_frame(promise);
  else if (type == JS_PROMISE_HOOK_RESOLVE)
    agent->release_context_frame(promise);
}

void napi_agent__::rejection_tracker(JSContext *ctx,
                                     JSValueConst promise,
                                     JSValueConst reason,
                                     bool is_handled,
                                     void *opaque)
{
  auto *agent = static_cast<napi_agent__ *>(opaque);
  if (agent == nullptr || ctx == nullptr || JS_GetRuntime(ctx) != agent->runtime_)
    return;

  agent->release_context_frame(promise);
  JSValue event_type = JS_NewInt32(ctx, is_handled ? 1 : 0);
  JSValueConst argv[] = {event_type, promise, is_handled ? JS_UNDEFINED : reason};
  agent->call_hook(ctx, agent->promise_reject_callback_, 3, argv);
}

int napi_agent__::import_meta_init(JSContext *ctx,
                                   JSModuleDef *module,
                                   JSValueConst meta,
                                   void *opaque)
{
  (void)ctx;
  auto *agent = static_cast<napi_agent__ *>(opaque);
  if (agent == nullptr || agent->closing_)
    return 0;
  return agent->module_wrap_->initialize_import_meta(module, meta);
}

JSValue napi_agent__::dynamic_import(JSContext *ctx,
                                     JSModuleDef *referrer,
                                     JSValueConst referrer_name,
                                     JSValueConst specifier,
                                     JSValueConst attributes,
                                     JSModuleImportPhaseEnum phase,
                                     void *opaque)
{
  auto *agent = static_cast<napi_agent__ *>(opaque);
  if (agent == nullptr || agent->closing_)
    return JS_ThrowTypeError(ctx, "Dynamic import callback is not registered");
  return agent->module_wrap_->import_module_dynamically(referrer, referrer_name, specifier,
                                                        attributes, phase);
}
