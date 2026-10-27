#include "internal/napi_agent.h"

#include "internal/napi_env.h"
#include "internal/napi_module_wrap.h"
#include "internal/napi_promises.h"
#include "internal/napi_util.h"

#include <cassert>
#include <limits>

namespace
{
void *js_identity(JSValueConst value)
{
  return JS_IsObject(value) ? JS_VALUE_GET_PTR(value) : nullptr;
}
} // namespace

napi_agent__::napi_agent__(JSRuntime *runtime)
    : runtime_{runtime},
      scopes_{this}
{
}

// JSValues are released by clear_values() during env teardown, while the
// runtime is still alive.
napi_agent__::~napi_agent__() = default;

JSRuntime *napi_agent__::runtime() const
{
  return runtime_;
}

napi_env napi_agent__::principal_env() const
{
  return principal_env_;
}

void napi_agent__::set_principal_env(napi_env env)
{
  principal_env_ = env;
}

napi_env napi_agent__::env_for_context(JSContext *ctx) const
{
  if (ctx != nullptr)
  {
    auto *env = static_cast<napi_env>(JS_GetContextOpaque(ctx));
    if (env != nullptr && env->agent() == this)
      return env;
  }
  return principal_env_;
}

bool napi_agent__::open_root_scope(napi_env env)
{
  if (root_scope_ == nullptr)
  {
    root_scope_ = create_scope(env, nullptr);
    current_scope_ = root_scope_;
  }
  return root_scope_ != nullptr;
}

void napi_agent__::close_root_scope()
{
  napi_scope__ *root = scope_from_handle(root_scope_);
  if (root != nullptr)
    root->close();
}

void napi_agent__::close_scopes()
{
  scopes_.close();
  current_scope_ = nullptr;
  root_scope_ = nullptr;
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
  auto *handle = scopes_.allocate(env, parent);
  return reinterpret_cast<napi_handle_scope>(handle);
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

bool napi_agent__::has_pending_exception() const
{
  return has_pending_exception_;
}

bool napi_agent__::pending_exception_is_uncatchable() const
{
  return has_pending_exception_ && JS_IsUncatchableError(pending_exception_);
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

void napi_agent__::attach_promise_hook()
{
  JS_SetPromiseHook(runtime_, napi_agent__::promise_hook, this);
}

void napi_agent__::set_rejection_tracking(bool enabled)
{
  JS_SetHostPromiseRejectionTracker(
      runtime_, enabled ? napi_agent__::rejection_tracker : nullptr, this);
}

void napi_agent__::clear_promise_hooks()
{
  JS_SetPromiseHook(runtime_, nullptr, nullptr);
  JS_SetHostPromiseRejectionTracker(runtime_, nullptr, nullptr);
}

void napi_agent__::attach_module_hooks()
{
  JS_SetModuleImportMetaInitFunc(runtime_, napi_agent__::import_meta_init, this);
  JS_SetModuleDynamicImportFunc(runtime_, napi_agent__::dynamic_import, this);
}

void napi_agent__::clear_module_hooks()
{
  JS_SetModuleImportMetaInitFunc(runtime_, nullptr, nullptr);
  JS_SetModuleDynamicImportFunc(runtime_, nullptr, nullptr);
}

JSValueConst napi_agent__::continuation_preserved_embedder_data() const
{
  return continuation_preserved_embedder_data_;
}

void napi_agent__::set_continuation_preserved_embedder_data(JSValueConst value)
{
  replace_stored_value(&continuation_preserved_embedder_data_, value);
}

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
  if (!promise_context_frame_stack_.empty())
  {
    JSValue previous = promise_context_frame_stack_.back();
    promise_context_frame_stack_.pop_back();
    JS_FreeValueRT(runtime_, continuation_preserved_embedder_data_);
    continuation_preserved_embedder_data_ = previous;
  }

  (void)promise;
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

void napi_agent__::clear_values()
{
  clear_pending_exception();

  JS_FreeValueRT(runtime_, continuation_preserved_embedder_data_);
  continuation_preserved_embedder_data_ = JS_UNDEFINED;

  for (auto &entry : promise_context_frames_)
    JS_FreeValueRT(runtime_, entry.second);
  promise_context_frames_.clear();

  for (auto &frame : promise_context_frame_stack_)
    JS_FreeValueRT(runtime_, frame);
  promise_context_frame_stack_.clear();
}

void napi_agent__::replace_stored_value(JSValue *target, JSValueConst value)
{
  JS_FreeValueRT(runtime_, *target);
  *target = JS_DupValueRT(runtime_, value);
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

  napi_env env = agent->env_for_context(ctx);
  if (napi_util__::check_env(env))
  {
    if (type == JS_PROMISE_HOOK_INIT)
    {
      JSValueConst argv[] = {promise, parent_promise};
      env->promises().call_hook(static_cast<size_t>(type), 2, argv);
    }
    else
    {
      JSValueConst argv[] = {promise};
      env->promises().call_hook(static_cast<size_t>(type), 1, argv);
    }
  }

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

  napi_env env = agent->env_for_context(ctx);
  if (napi_util__::check_env(env))
    env->promises().call_reject_callback(promise, reason, is_handled);
}

int napi_agent__::import_meta_init(JSContext *ctx,
                                   JSModuleDef *module,
                                   JSValueConst meta,
                                   void *opaque)
{
  auto *agent = static_cast<napi_agent__ *>(opaque);
  napi_env env = agent == nullptr ? nullptr : agent->env_for_context(ctx);
  if (!napi_util__::check_env(env))
    return 0;
  return env->module_wrap().initialize_import_meta(module, meta);
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
  napi_env env = agent == nullptr ? nullptr : agent->env_for_context(ctx);
  if (!napi_util__::check_env(env))
    return JS_ThrowTypeError(ctx, "Dynamic import callback is not registered");
  return env->module_wrap().import_module_dynamically(referrer, referrer_name, specifier,
                                                      attributes, phase);
}
