#include "internal/napi_ref.h"

#include <new>

#include "internal/napi_lifetime_tracker.h"
#include "internal/napi_v8_env.h"

namespace {

inline bool CanBeHeldWeakly(v8::Local<v8::Value> value) {
  return value->IsObject() || value->IsSymbol();
}

}  // namespace

napi_ref__::napi_ref__(napi_env__* env,
                       v8::Local<v8::Value> value,
                       uint32_t initial_refcount,
                       napi_ref_ownership__ ownership)
    : env_(env),
      value_(env->isolate, value),
      refcount_(initial_refcount),
      ownership_(ownership),
      can_be_weak_(CanBeHeldWeakly(value)) {
  if (refcount_ == 0) {
    SetWeak();
  }
}

napi_ref__::~napi_ref__() {
  if (!value_.IsEmpty() && can_be_weak_ && refcount_ == 0) {
    value_.ClearWeak();
  }
  value_.Reset();
  Unlink();
}

napi_ref__* napi_ref__::New(napi_env__* env,
                            v8::Local<v8::Value> value,
                            uint32_t initial_refcount,
                            napi_ref_ownership__ ownership) {
  napi_ref__* reference = env->allocate<napi_ref__>(
      env, value, initial_refcount, ownership);
  if (reference != nullptr) {
    reference->Link(&env->reflist);
  }
  return reference;
}

void napi_ref__::Destroy() {
  napi_env__* env = env_;
  if (env != nullptr) {
    env->release(this);
  }
}

uint32_t napi_ref__::Ref() {
  if (value_.IsEmpty()) {
    return 0;
  }
  if (++refcount_ == 1 && can_be_weak_) {
    value_.ClearWeak();
  }
  return refcount_;
}

uint32_t napi_ref__::Unref() {
  if (value_.IsEmpty() || refcount_ == 0) {
    return 0;
  }
  if (--refcount_ == 0) {
    SetWeak();
  }
  return refcount_;
}

v8::Local<v8::Value> napi_ref__::Get() const {
  if (env_ == nullptr || value_.IsEmpty()) {
    return v8::Local<v8::Value>();
  }
  return value_.Get(env_->isolate);
}

void napi_ref__::Invalidate() {
  if (!value_.IsEmpty()) {
    if (can_be_weak_ && refcount_ == 0) {
      value_.ClearWeak();
    }
    value_.Reset();
  }
  Unlink();
}

void napi_ref__::Finalize() {
  ForgetWrap();
  value_.Reset();
  bool deleteMe = ownership_ == napi_ref_ownership__::kRuntime;
  Unlink();
  CallUserFinalizer();
  if (deleteMe) {
    Destroy();
  }
}

// A wrap finalized while its object is alive, as the env's release does:
// the object lets go of the wrap first, so napi_unwrap and napi_remove_wrap
// find none, rather than this reference, which the finalizer may delete and
// which is freed after it if the provider owns it.
void napi_ref__::ForgetWrap() {
  if (!wrap_ || value_.IsEmpty() || env_ == nullptr) return;
  wrap_ = false;
  v8::Isolate* isolate = env_->isolate;
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Value> value = value_.Get(isolate);
  if (!value->IsObject()) return;
  v8::Local<v8::Object> object = value.As<v8::Object>();
  v8::Local<v8::Context> context = env_->main_context();
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Private> key = env_->agent->wrap_private_key.Get(isolate);
  v8::Local<v8::Value> wrapped;
  if (object->GetPrivate(context, key).ToLocal(&wrapped) && wrapped->IsExternal() &&
      wrapped.As<v8::External>()->Value() == this) {
    (void)object->DeletePrivate(context, key);
  }
}

void napi_ref__::InvokeFinalizerFromGC() {
  Finalize();
}

void napi_ref__::SetWeak() {
  if (can_be_weak_) {
    value_.SetWeak(this, WeakCallback, v8::WeakCallbackType::kParameter);
  } else {
    value_.Reset();
  }
}

void napi_ref__::WeakCallback(const v8::WeakCallbackInfo<napi_ref__>& info) {
  napi_ref__* ref = info.GetParameter();
  if (ref == nullptr) return;
  ref->value_.Reset();
  ref->InvokeFinalizerFromGC();
}
