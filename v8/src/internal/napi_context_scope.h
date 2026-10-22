#ifndef NAPI_V8_CONTEXT_SCOPE_H_
#define NAPI_V8_CONTEXT_SCOPE_H_

#include <cassert>
#include "internal/napi_v8_env.h"

// Selected environment context and entered VM context must move together.
// Globals keep explicit scopes valid across separate Wasm host imports; no
// Local from a temporary HandleScope is retained here.
class NapiV8ContextScope {
 public:
  NapiV8ContextScope(napi_env env, v8::Local<v8::Context> context, uint64_t token = 0)
      : env_(env), context_(env->isolate, context), previous_(env->context_scope), token_(token),
        depth_(previous_ ? previous_->depth() + 1 : 1) {
    context->Enter();
    env_->context_scope = this;
  }
  ~NapiV8ContextScope() {
    // A token scope can close outside any caller HandleScope (a top-level
    // leave, or environment release). Keep the Local made below out of the
    // caller's scope, which may be the environment's never-closed root.
    v8::HandleScope handles(env_->isolate);
    // A native callback may return or trap with guest-entered scopes open.
    // Reclaim only those explicit scopes, never a live outer callback frame.
    while (env_->context_scope != this) {
      auto* scope = env_->context_scope;
      assert(scope != nullptr && scope->token_ != 0);
      delete scope;
    }
    context_.Get(env_->isolate)->Exit();
    env_->context_scope = previous_;
  }
  NapiV8ContextScope(const NapiV8ContextScope&) = delete;
  NapiV8ContextScope& operator=(const NapiV8ContextScope&) = delete;
  v8::Local<v8::Context> context() const { return context_.Get(env_->isolate); }
  uint64_t token() const { return token_; }
  unsigned depth() const { return depth_; }

 private:
  napi_env env_;
  v8::Global<v8::Context> context_;
  NapiV8ContextScope* previous_;
  uint64_t token_;
  unsigned depth_;
};

#endif
