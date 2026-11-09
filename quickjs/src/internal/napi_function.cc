#include "internal/napi_function.h"

#include "internal/napi_callback_info.h"
#include "internal/napi_env.h"
#include "internal/napi_external.h"
#include "internal/napi_util.h"
#include "internal/napi_value.h"

#include <climits>
#include <cstring>
#include <string_view>
#include <unordered_set>

extern "C" bool napi_quickjs_is_construct_call(JSContext *ctx);
extern "C" JSValue napi_quickjs_new_object_from_constructor(JSContext *ctx, JSValueConst constructor);
extern "C" JSValue napi_quickjs_function_data(JSValueConst function, int magic);

namespace
{
constexpr int binding_constructor = 0x4e41;
constexpr int binding_method = 0x4e42;

class quickjs_callback_handle_scope__
{
public:
  explicit quickjs_callback_handle_scope__(napi_env env)
      : env_{env},
        parent_{env != nullptr ? env->current_scope() : nullptr},
        scope_{env != nullptr ? env->create_scope(parent_) : nullptr}
  {
    if (scope_ != nullptr)
      env_->set_current_scope(scope_);
  }

  ~quickjs_callback_handle_scope__()
  {
    if (scope_ == nullptr || env_ == nullptr)
      return;

    if (env_->is_current_scope(scope_))
      env_->set_current_scope(parent_);
    env_->destroy_scope(scope_);
  }

  bool is_open() const
  {
    return scope_ != nullptr;
  }

private:
  napi_env env_ = nullptr;
  napi_handle_scope parent_ = nullptr;
  napi_handle_scope scope_ = nullptr;
};

// Native code up the stack can have an exception pending when JS calls a
// native callback: an N-API call that makes an error (one over a detached
// buffer, say) formats it through Error.prepareStackTrace, which a page can
// define as a getter that calls native code. That exception is not the
// callback's to see or to throw. It is set aside while the callback runs, and
// is the one pending again once the callback's own has been thrown.
class quickjs_outer_exception_scope__
{
public:
  explicit quickjs_outer_exception_scope__(napi_env env) : env_{env}
  {
    if (env_->has_last_exception())
    {
      held_ = true;
      exception_ = env_->take_last_exception();
    }
  }

  ~quickjs_outer_exception_scope__()
  {
    if (held_)
      env_->set_last_exception(exception_);
  }

private:
  napi_env env_ = nullptr;
  bool held_ = false;
  JSValue exception_ = JS_UNDEFINED;
};
} // namespace

JSValue napi_function__::create_internal(napi_env env,
                                         const char *utf8name,
                                         napi_callback cb,
                                         void *data)
{
  napi_value fn_val;
  napi_status status = napi_create_function(env, utf8name, NAPI_AUTO_LENGTH, cb, data, &fn_val);
  if (status != napi_ok)
    return JS_EXCEPTION;

  return JS_DupValue(env->context(), napi_quickjs_value_inner(env, fn_val));
}

namespace
{
// QuickJS passes the caller's realm as `ctx` for C function data: it does not
// switch realms as it does for its own built-in functions. The callback runs
// in the function's realm instead (`realm_ctx`, or the env's own when null),
// as in V8, and N-API acts in that realm while it runs.
JSValue invoke_callback(JSContext *caller_ctx,
                        JSContext *realm_ctx,
                        JSValueConst realm_anchor,
                        JSValueConst this_val,
                        int argc,
                        JSValueConst *argv,
                        int magic,
                        JSValue *func_data)
{
  auto env = napi_external__::get_env(func_data[0]);
  // The release's hooks and finalizers can use N-API, native functions too
  // (unofficial_napi_release_env); once they are done no user code runs.
  if (!napi_util__::check_env(env) || env->finalizers_done())
    return JS_ThrowReferenceError(caller_ctx, "Null NAPI env");
  // Closes the scopes the callback leaves open once everything below is done.
  napi_realm_frame_scope__ realm_frame{env, realm_ctx, realm_anchor};
  JSContext *ctx = env->context();

  auto cb_ptr = napi_external__::get_value(func_data[0]);
  if (cb_ptr == nullptr)
    return JS_ThrowReferenceError(ctx, "Null NAPI callback data");

  auto cb = reinterpret_cast<napi_callback>(cb_ptr);
  auto user_data = napi_external__::get_value(func_data[1]);
  // A terminating env runs no more native callbacks; JS that is still
  // running between interrupt polls stops here.
  if (env->termination_requested())
    return napi_util__::throw_termination(ctx);
  quickjs_callback_handle_scope__ callback_scope{env};
  if (!callback_scope.is_open())
    return JS_ThrowOutOfMemory(ctx);

  JSValue effective_this = this_val;
  JSValue new_target = JS_UNDEFINED;
  const bool is_binding_constructor = magic == binding_constructor;
  bool called_as_constructor = is_binding_constructor ? napi_quickjs_is_construct_call(caller_ctx)
      : magic == JS_CFUNC_constructor_magic || (magic == 0 && JS_IsConstructor(ctx, this_val));

  if (called_as_constructor)
  {
    if (is_binding_constructor)
      effective_this = napi_quickjs_new_object_from_constructor(ctx, this_val);
    else
    {
      JSValue proto = JS_GetPropertyStr(ctx, this_val, "prototype");
      effective_this = JS_NewObjectProto(ctx, proto);
      JS_FreeValue(ctx, proto);
    }

    if (JS_IsException(effective_this))
      return effective_this;

    new_target = this_val;
  }

  quickjs_outer_exception_scope__ outer_exception{env};
  auto info = napi_callback_info__{env, effective_this, new_target, argc, argv, user_data};
  auto result = cb(env, reinterpret_cast<napi_callback_info>(&info));

  // The callback may have requested termination itself (process.exit()), or
  // swallowed the termination of JS it called. Either way the calling JS must
  // not resume, matching V8, where termination keeps unwinding until every JS
  // frame has exited or the embedder cancels it.
  if (env->termination_requested())
  {
    napi_util__::clear_last_exception(env);
    if (called_as_constructor)
      JS_FreeValue(ctx, effective_this);
    return napi_util__::throw_termination(ctx);
  }

  if (napi_util__::rethrow_last_exception(env, ctx))
  {
    if (called_as_constructor)
      JS_FreeValue(ctx, effective_this);
    return JS_EXCEPTION;
  }

  JSValue returned = JS_UNDEFINED;
  if (result != nullptr)
  {
    returned = JS_DupValue(ctx, napi_quickjs_value_inner(env, result));
    env->delete_value_from_current_scope(result);
  }
  if (called_as_constructor)
  {
    if (result != nullptr && (!is_binding_constructor || JS_IsObject(returned)))
      JS_FreeValue(ctx, effective_this);
    else
    {
      JS_FreeValue(ctx, returned);
      returned = effective_this;
    }
  }

  return returned;
}
} // namespace

JSValue napi_function__::trampoline(JSContext *caller_ctx,
                                    JSValueConst this_val,
                                    int argc,
                                    JSValueConst *argv,
                                    int magic,
                                    JSValue *func_data)
{
  return invoke_callback(caller_ctx, nullptr, JS_UNDEFINED, this_val, argc, argv, magic, func_data);
}

JSValue napi_function__::realm_trampoline(JSContext *caller_ctx,
                                          JSValueConst this_val,
                                          int argc,
                                          JSValueConst *argv,
                                          int magic,
                                          JSValue *func_data)
{
  auto *realm_ctx = static_cast<JSContext *>(napi_external__::get_value(func_data[3]));
  return invoke_callback(caller_ctx, realm_ctx, func_data[2], this_val, argc, argv, magic, func_data);
}

napi_status napi_function__::make_constructible(napi_env env, JSValue fn)
{
  JSValue proto = JS_NewObject(env->context());
  if (JS_IsException(proto))
    return napi_util__::return_pending_if_caught(env, "Failed to create function prototype");

  JS_SetConstructor(env->context(), fn, proto);
  JS_SetConstructorBit(env->context(), fn, true);
  JS_FreeValue(env->context(), proto);
  return napi_ok;
}

namespace
{
JSValue create_realm_function(napi_env env, const char *utf8name, size_t length,
                              napi_callback cb, void *data, int arity, int magic)
{
  // The function belongs to the current realm. One of another realm also
  // holds that realm's anchor, which keeps the realm alive exactly as long as
  // the function and lets the collector see the edge.
  JSContext *ctx = env->context();
  JSValueConst realm_anchor = env->current_realm_anchor();
  const bool own_realm = JS_IsUndefined(realm_anchor);
  JSValue data_values[4] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
  const int data_count = own_realm ? 2 : 4;
  auto free_data = [&]()
  {
    for (JSValue &value : data_values)
      JS_FreeValue(ctx, value);
  };
  data_values[0] = env->wrap_external_data(reinterpret_cast<void *>(cb));
  if (JS_IsException(data_values[0]))
    return JS_EXCEPTION;
  data_values[1] = env->wrap_external_data(data);
  if (JS_IsException(data_values[1]))
  {
    data_values[1] = JS_UNDEFINED;
    free_data();
    return JS_EXCEPTION;
  }
  if (!own_realm)
  {
    data_values[2] = JS_DupValue(ctx, realm_anchor);
    data_values[3] = env->wrap_external_data(ctx);
    if (JS_IsException(data_values[3]))
    {
      data_values[3] = JS_UNDEFINED;
      free_data();
      return JS_EXCEPTION;
    }
  }

  JSValue fn = JS_NewCFunctionData(ctx, own_realm ? napi_function__::trampoline : napi_function__::realm_trampoline, arity, magic,
                                   data_count, data_values);
  free_data();
  if (JS_IsException(fn))
    return fn;
  if (utf8name != nullptr)
  {
    JSValue name = JS_NewStringLen(ctx, utf8name, length == NAPI_AUTO_LENGTH ? std::strlen(utf8name) : length);
    if (JS_IsException(name) || JS_DefinePropertyValueStr(ctx, fn, "name", name, JS_PROP_CONFIGURABLE) < 0)
    {
      JS_FreeValue(ctx, fn);
      return JS_EXCEPTION;
    }
  }
  return fn;
}

bool owns_value(napi_env env, napi_value value)
{
  return value != nullptr && env->agent()->owns_scope(napi_scope__::ValuesAllocator::unsafe_owner(value));
}

class ScopedValue
{
public:
  ScopedValue(JSContext *context, JSValue value) : context_(context), value_(value) {}
  ~ScopedValue() { JS_FreeValue(context_, value_); }
  ScopedValue(const ScopedValue &) = delete;
  ScopedValue &operator=(const ScopedValue &) = delete;
  JSValue get() const { return value_; }
private:
  JSContext *context_;
  JSValue value_;
};
} // namespace

napi_status napi_function__::create(napi_env env, const char *utf8name, size_t length,
                                    napi_callback cb, void *data, int magic, napi_value *result)
{
  if (!napi_util__::check_env(env) || cb == nullptr || result == nullptr)
    return napi_util__::invalid_arg(env);
  JSValue fn = create_realm_function(env, utf8name, length, cb, data, 0, magic);
  if (JS_IsException(fn))
    return napi_util__::return_pending_if_caught(env, "Failed to create function");

  *result = env->wrap_value_in_current_scope(fn, true);
  return (*result == nullptr) ? napi_generic_failure : napi_ok;
}

extern "C" napi_status NAPI_CDECL unofficial_napi_define_binding(
    napi_env env, uint32_t kind, const char *utf8name, size_t name_length,
    napi_callback callback, void *data, uint32_t function_length,
    napi_value parent_or_null, size_t property_count, const napi_property_descriptor *properties,
    const uint8_t *member_lengths_or_null, napi_value *function_out, napi_value *prototype_out)
{
  const bool is_class = kind == UNOFFICIAL_NAPI_BINDING_CLASS;
  if (!napi_util__::check_env(env) || (kind != UNOFFICIAL_NAPI_BINDING_METHOD && !is_class) ||
      function_out == nullptr || (is_class && prototype_out == nullptr))
    return napi_invalid_arg;
  JSContext *ctx = env->context();
  if (env->termination_requested())
    return napi_util__::fail_if_terminating(env, ctx);
  if (utf8name == nullptr || name_length == 0 || name_length > INT_MAX || callback == nullptr ||
      function_length > UINT8_MAX || (property_count != 0 && properties == nullptr) ||
      (!is_class && (parent_or_null != nullptr || property_count != 0 || properties != nullptr ||
                     member_lengths_or_null != nullptr || prototype_out != nullptr)))
    return napi_invalid_arg;

  JSValueConst parent = JS_UNDEFINED;
  if (parent_or_null != nullptr)
  {
    if (!owns_value(env, parent_or_null))
      return napi_invalid_arg;
    parent = napi_quickjs_value_inner(env, parent_or_null);
    auto registration = napi_quickjs_function_data(parent, binding_constructor);
    if (JS_IsUndefined(registration) || napi_external__::get_env(registration) != env)
      return napi_invalid_arg;
  }

  std::unordered_set<std::string_view> static_names, prototype_names;
  for (size_t i = 0; i < property_count; ++i)
  {
    const auto &member = properties[i];
    const bool accessor = member.getter != nullptr || member.setter != nullptr;
    if (member.name != nullptr || member.utf8name == nullptr ||
        (member.method != nullptr) + accessor + (member.value != nullptr) != 1 ||
        (member.method == nullptr && member_lengths_or_null != nullptr && member_lengths_or_null[i] != 0) ||
        (member.value != nullptr && (!owns_value(env, member.value) || JS_IsObject(napi_quickjs_value_inner(env, member.value)))))
      return napi_invalid_arg;
    const bool is_static = member.attributes & napi_static;
    const std::string_view name(member.utf8name);
    auto &names = is_static ? static_names : prototype_names;
    if (name.size() >= 65536 || (is_static && (name == "length" || name == "name" || name == "prototype")) ||
        (!is_static && name == "constructor") || !names.insert(name).second)
      return napi_invalid_arg;
  }

  const auto failed = [&]() { return napi_util__::return_pending_if_caught(env, ctx, "Failed to define binding"); };
  ScopedValue function(ctx, create_realm_function(env, utf8name, name_length, callback, data,
                                                  function_length, is_class ? binding_constructor : binding_method));
  if (JS_IsException(function.get()))
    return failed();
  if (!is_class)
  {
    napi_value result = env->wrap_value_in_current_scope(function.get(), false);
    if (result == nullptr)
      return napi_generic_failure;
    *function_out = result;
    return napi_ok;
  }

  ScopedValue prototype(ctx, JS_NewObject(ctx));
  if (JS_IsException(prototype.get()) || JS_SetConstructor(ctx, function.get(), prototype.get()) < 0 ||
      !JS_SetConstructorBit(ctx, function.get(), true))
    return failed();
  if (!JS_IsUndefined(parent))
  {
    // The parent is a binding constructor, whose prototype is an own, fixed
    // data property. Neither this read nor the prototype writes run JS.
    ScopedValue parent_prototype(ctx, JS_GetPropertyStr(ctx, parent, "prototype"));
    if (JS_IsException(parent_prototype.get()) || JS_SetPrototype(ctx, function.get(), parent) < 0 ||
        JS_SetPrototype(ctx, prototype.get(), parent_prototype.get()) < 0)
      return failed();
  }
  for (size_t i = 0; i < property_count; ++i)
  {
    const auto &member = properties[i];
    JSValueConst target = member.attributes & napi_static ? function.get() : prototype.get();
    const auto *name = member.utf8name;
    const size_t length = std::strlen(name);
    int flags = JS_PROP_THROW;
    if (member.attributes & napi_configurable) flags |= JS_PROP_CONFIGURABLE;
    if (member.attributes & napi_enumerable) flags |= JS_PROP_ENUMERABLE;
    if (member.attributes & napi_writable) flags |= JS_PROP_WRITABLE;
    if (member.getter != nullptr || member.setter != nullptr)
    {
      std::string getter_name = std::string("get ") + name;
      std::string setter_name = std::string("set ") + name;
      ScopedValue getter(ctx, member.getter == nullptr ? JS_UNDEFINED : create_realm_function(
          env, getter_name.data(), getter_name.size(), member.getter, member.data, 0, binding_method));
      ScopedValue setter(ctx, member.setter == nullptr ? JS_UNDEFINED : create_realm_function(
          env, setter_name.data(), setter_name.size(), member.setter, member.data, 1, binding_method));
      if (JS_IsException(getter.get()) || JS_IsException(setter.get()))
        return failed();
      const auto atom = JS_NewAtomLen(ctx, name, length);
      if (atom == JS_ATOM_NULL)
        return failed();
      int defined = JS_DefinePropertyGetSet(ctx, target, atom, JS_DupValue(ctx, getter.get()),
                                           JS_DupValue(ctx, setter.get()), flags & ~JS_PROP_WRITABLE);
      JS_FreeAtom(ctx, atom);
      if (defined < 0)
        return failed();
    }
    else
    {
      auto value = member.method == nullptr ? JS_DupValue(ctx, napi_quickjs_value_inner(env, member.value))
          : create_realm_function(env, name, length, member.method, member.data,
                                  member_lengths_or_null == nullptr ? 0 : member_lengths_or_null[i], binding_method);
      if (JS_IsException(value) || JS_DefinePropertyValueStr(ctx, target, name, value, flags) < 0)
        return failed();
    }
  }

  napi_value result = env->wrap_value_in_current_scope(function.get(), false);
  if (result == nullptr)
    return napi_generic_failure;
  napi_value proto_result = env->wrap_value_in_current_scope(prototype.get(), false);
  if (proto_result == nullptr)
  {
    env->delete_value_from_current_scope(result);
    return napi_generic_failure;
  }
  *function_out = result;
  *prototype_out = proto_result;
  return napi_ok;
}
