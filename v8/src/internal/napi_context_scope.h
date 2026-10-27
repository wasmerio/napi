#ifndef NAPI_V8_CONTEXT_SCOPE_H_
#define NAPI_V8_CONTEXT_SCOPE_H_

#include <cassert>
#include "internal/napi_v8_env.h"

// Selected environment context and entered VM context must move together.
//
// A stack scope (token 0) wraps one native callback, finalizer or interrupt
// dispatch and must be nested inside a HandleScope that outlives it; it keeps
// the caller's Local, so native callback dispatch allocates no global handle.
// A token scope can outlive the HandleScope it was opened in (a Wasm guest
// enters and leaves in separate host imports), so it keeps a Global and never
// retains a Local from a temporary HandleScope.
//
// The chain is per environment, while V8's entered-context stack is per
// isolate. Strict LIFO here keeps V8's Enter/Exit balanced only if no other
// environment on the same isolate interleaves its own scopes; the provider
// creates one isolate per environment. depth() counts every scope on the
// chain, so native callback frames count toward the entry limit.
class NapiV8ContextScope {
 public:
  NapiV8ContextScope(napi_env env, v8::Local<v8::Context> context, uint64_t token = 0)
      : env_(env), previous_(env->context_scope), token_(token),
        depth_(previous_ ? previous_->depth() + 1 : 1) {
    if (token_ == 0) {
      local_ = context;
    } else {
      global_.Reset(env->isolate, context);
    }
    context->Enter();
    env_->context_scope = this;
  }
  ~NapiV8ContextScope() {
    // A native callback may return or trap with guest-entered scopes open.
    // Reclaim only those explicit scopes, never a live outer callback frame.
    while (env_->context_scope != this) {
      auto* scope = env_->context_scope;
      assert(scope != nullptr && scope->token_ != 0);
      delete scope;
    }
    if (token_ == 0) {
      local_->Exit();
    } else {
      // A token scope can close outside any caller HandleScope (a top-level
      // leave, or environment release). Keep the Local made here out of the
      // caller's scope, which may be the environment's never-closed root.
      v8::HandleScope handles(env_->isolate);
      global_.Get(env_->isolate)->Exit();
    }
    env_->context_scope = previous_;
  }
  NapiV8ContextScope(const NapiV8ContextScope&) = delete;
  NapiV8ContextScope& operator=(const NapiV8ContextScope&) = delete;
  v8::Local<v8::Context> context() const {
    return token_ == 0 ? local_ : global_.Get(env_->isolate);
  }
  uint64_t token() const { return token_; }
  unsigned depth() const { return depth_; }

 private:
  napi_env env_;
  v8::Local<v8::Context> local_;
  v8::Global<v8::Context> global_;
  NapiV8ContextScope* previous_;
  uint64_t token_;
  unsigned depth_;
};

#endif
