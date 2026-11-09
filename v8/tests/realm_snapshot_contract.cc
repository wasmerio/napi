#include "node_api.h"
#include "napi_bridge_v8.h"
#include "unofficial_napi.h"

#undef NDEBUG
#include <barrier>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <source_location>
#include <thread>

static void ok(napi_status status, std::source_location location = std::source_location::current()) {
  if (status == napi_ok) return;
  std::fprintf(stderr, "unexpected status %d at line %u\n", status, location.line());
  std::abort();
}
static napi_value text(napi_env e, const char* s) {
  napi_value v = nullptr;
  ok(napi_create_string_utf8(e, s, NAPI_AUTO_LENGTH, &v));
  return v;
}
static napi_value eval(napi_env e, const char* s) {
  napi_value v = nullptr;
  ok(napi_run_script(e, text(e,s), &v));
  return v;
}
static uint64_t owner(napi_env e) {
  uint64_t data = UINT64_MAX;
  ok(unofficial_napi_get_current_realm_data(e, &data));
  return data;
}
static napi_value ctor(napi_env e, napi_callback_info info) {
  napi_value receiver = nullptr;
  ok(napi_get_cb_info(e, info, nullptr, nullptr, &receiver, nullptr));
  return receiver;
}
static napi_value method(napi_env e, napi_callback_info info) {
  napi_value receiver = nullptr;
  ok(napi_get_cb_info(e, info, nullptr, nullptr, &receiver, nullptr));
  napi_valuetype type;
  ok(napi_typeof(e, receiver, &type));
  napi_value v = nullptr;
  ok(napi_create_uint32(e, static_cast<uint32_t>(owner(e)) + static_cast<uint32_t>(type), &v));
  return v;
}
static int getter_count = 0;
static int setter_count = 0;
static napi_value get(napi_env e, napi_callback_info) { ++getter_count; return text(e, "getter"); }
static napi_value set(napi_env, napi_callback_info) { ++setter_count; return nullptr; }
static napi_value promise_hook(napi_env e, napi_callback_info info) {
  void* data = nullptr;
  ok(napi_get_cb_info(e, info, nullptr, nullptr, nullptr, &data));
  ++*static_cast<int*>(data);
  return nullptr;
}

static void builder_finalizer(napi_env env, void* data, void*) {
  napi_value global = nullptr;
  assert(napi_get_global(env, &global) == napi_invalid_arg);
  *static_cast<bool*>(data) = true;
}

static constexpr char kKey[] = "plan-smoke-v8";

static void bake(const char* path, const char* key, int answer,
                 const char* guest_data, bool write_error = false) {
  const unofficial_napi_snapshot_env_options request{
      sizeof(request), UNOFFICIAL_NAPI_SNAPSHOT_ENV_OPTIONS_VERSION,
      UNOFFICIAL_NAPI_SNAPSHOT_ROLE_BUILDER, 0, nullptr,
      key, std::strlen(key), path, std::strlen(path)};
  napi_env env = nullptr;
  unofficial_napi_env_owner eowner = nullptr;
  ok(unofficial_napi_create_env_with_snapshot(10, nullptr, &request, &env, &eowner));
  assert(owner(env) == 0);
  napi_handle_scope hs = nullptr;
  ok(napi_open_handle_scope(env, &hs));
  napi_value parent = nullptr, parent_prototype = nullptr;
  ok(unofficial_napi_define_binding(env, UNOFFICIAL_NAPI_BINDING_CLASS, "Foreign", 7,
      ctor, nullptr, 0, nullptr, 0, nullptr, nullptr, &parent, &parent_prototype));
  int promise_hooks = 0;
  napi_value hook = nullptr;
  ok(napi_create_function(env, "hook", 4, promise_hook, &promise_hooks, &hook));
  ok(unofficial_napi_set_promise_hooks(env, hook, nullptr, nullptr, nullptr));
  eval(env, "Promise.resolve(1)");
  assert(promise_hooks > 0);
  const int hooks_in_main = promise_hooks;
  bool finalized = false;
  ok(napi_set_instance_data(env, &finalized, builder_finalizer, nullptr));

  unofficial_napi_realm_options opts;
  unofficial_napi_realm_options_init(&opts);
  opts.own_microtask_queue = true;
  unofficial_napi_realm realm = nullptr;
  assert(unofficial_napi_create_realm(env, &opts, UNOFFICIAL_NAPI_REALM_MODE_TEMPLATE, 0,
      nullptr, &realm, nullptr, nullptr) == napi_invalid_arg);
  opts.own_microtask_queue = false;

  unofficial_napi_realm invalid = nullptr;
  ok(unofficial_napi_create_realm(env, &opts, UNOFFICIAL_NAPI_REALM_MODE_TEMPLATE, 0,
      nullptr, &invalid, nullptr, nullptr));
  unofficial_napi_realm_scope scope;
  ok(unofficial_napi_enter_realm(env, invalid, &scope));
  auto invalid_root = eval(env, "globalThis.stillPresent=1; var immutable=2; ({})");
  ok(unofficial_napi_leave_realm(env, scope));
  assert(unofficial_napi_snapshot_commit(env, invalid, invalid_root, nullptr, 0, nullptr) == napi_generic_failure);
  ok(unofficial_napi_enter_realm(env, invalid, &scope));
  bool unchanged = false;
  ok(napi_get_value_bool(env, eval(env, "stillPresent===1"), &unchanged));
  assert(unchanged);
  ok(unofficial_napi_leave_realm(env, scope));

  napi_value global = nullptr, root = nullptr;
  ok(unofficial_napi_create_realm(env, &opts, UNOFFICIAL_NAPI_REALM_MODE_TEMPLATE, 0,
      reinterpret_cast<void*>(0x80000042), &realm, &global, &root));
  assert(root == nullptr);
  ok(unofficial_napi_enter_realm(env, realm, &scope));
  assert(owner(env) == 0x80000042);
  ok(unofficial_napi_set_promise_hooks(env, hook, nullptr, nullptr, nullptr));
  eval(env, "Promise.resolve(2)");
  assert(promise_hooks == hooks_in_main);
  assert(unofficial_napi_set_promise_hooks(env, eval(env, "(() => {})"),
      nullptr, nullptr, nullptr) == napi_invalid_arg);
  napi_value refused = nullptr;
  assert(napi_define_class(env, "Unsafe", 6, ctor, nullptr, 0, nullptr, &refused) ==
         napi_generic_failure);
  assert(napi_create_external(env, nullptr, nullptr, nullptr, &refused) == napi_generic_failure);
  assert(unofficial_napi_define_binding(env, UNOFFICIAL_NAPI_BINDING_CLASS, "Child", 5,
      ctor, nullptr, 0, parent, 0, nullptr, nullptr, &refused, &parent_prototype) ==
         napi_invalid_arg);
  eval(env, R"(
    globalThis.savedData = 17;
    Object.defineProperty(globalThis, "savedAccessor", {get() { return 29 }, configurable: true});
    Object.defineProperty(Object.prototype, "get", {
      get() { throw new Error("inherited descriptor field") }, configurable: true
    });
  )");
  const std::string script = "({answer: " + std::to_string(answer) + "})";
  root = eval(env, script.c_str());
  assert(napi_wrap(env, root, nullptr, nullptr, nullptr, nullptr) == napi_generic_failure);
  napi_ref keep_root = nullptr;
  ok(napi_create_reference(env, root, 1, &keep_root));
  size_t guest_length = std::strlen(guest_data) + 1;
  assert(unofficial_napi_snapshot_commit(env, realm, root, guest_data, guest_length, nullptr) == napi_invalid_arg);
  ok(unofficial_napi_leave_realm(env, scope));
  assert(unofficial_napi_snapshot_commit(env, realm, invalid_root, nullptr, 0, nullptr) == napi_invalid_arg);
  uint32_t index = UINT32_MAX;
  ok(unofficial_napi_snapshot_commit(env, realm, root, nullptr, 0, &index));
  assert(index == 0);
  assert(napi_wrap(env, root, nullptr, nullptr, nullptr, nullptr) == napi_generic_failure);
  assert(unofficial_napi_snapshot_commit(env, realm, root, nullptr, 0, nullptr) == napi_invalid_arg);
  assert(unofficial_napi_enter_realm(env, realm, &scope) == napi_invalid_arg);
  auto source = unofficial_napi_js_source_from_text(text(env, "1"));
  auto filename = text(env, "sealed.js");
  assert(unofficial_napi_run_script(env, realm, &source, filename, 0, 0, -1, true,
      false, false, nullptr, &refused) == napi_invalid_arg);
  assert(unofficial_napi_compile_function(env, realm, &source, filename, 0, 0,
      nullptr, nullptr, nullptr, &refused) == napi_invalid_arg);

  unofficial_napi_realm last = nullptr;
  ok(unofficial_napi_create_realm(env, &opts, UNOFFICIAL_NAPI_REALM_MODE_TEMPLATE, 0,
      nullptr, &last, nullptr, nullptr));
  ok(unofficial_napi_enter_realm(env, last, &scope));
  auto last_root = eval(env, "({})");
  ok(unofficial_napi_leave_realm(env, scope));
  napi_escapable_handle_scope cleanup = nullptr;
  ok(napi_open_escapable_handle_scope(env, &cleanup));
  ok(unofficial_napi_snapshot_commit(env, last, last_root, guest_data, guest_length, &index));
  assert(index == 1);
  assert(napi_get_named_property(env, root, "answer", &refused) == napi_invalid_arg);
  assert(napi_set_named_property(env, root, "answer", filename) == napi_invalid_arg);
  assert(unofficial_napi_event_loop_checkpoint(
      env, unofficial_napi_event_loop_checkpoint_microtasks, false, nullptr) == napi_invalid_arg);
  ok(napi_delete_reference(env, keep_root));
  ok(napi_close_escapable_handle_scope(env, cleanup));
  ok(napi_close_handle_scope(env, hs));
  napi_status result = unofficial_napi_release_env(eowner, nullptr);
  assert(finalized);
  assert(result == (write_error ? napi_generic_failure : napi_ok));
}

static unofficial_napi_snapshot open_artifact(const char* path, const char* key) {
  std::ifstream file(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(file)), {});
  assert(!bytes.empty());
  unofficial_napi_snapshot artifact = nullptr;
  const char wrong_key[] = "wrong-key";
  assert(unofficial_napi_open_snapshot(bytes.data(), bytes.size(), wrong_key,
                                      sizeof(wrong_key) - 1, &artifact) == napi_generic_failure);
  assert(artifact == nullptr);
  ok(unofficial_napi_open_snapshot(bytes.data(), bytes.size(), key, std::strlen(key), &artifact));
  assert(artifact != nullptr);
  bytes.back() ^= 1;
  unofficial_napi_snapshot rejected = nullptr;
  assert(unofficial_napi_open_snapshot(bytes.data(), bytes.size(), key, std::strlen(key),
                                      &rejected) == napi_generic_failure);
  assert(rejected == nullptr);
  return artifact;
}

static void check_fresh() {
  napi_env env = nullptr;
  unofficial_napi_env_owner eowner = nullptr;
  ok(unofficial_napi_create_env(10, nullptr, &env, &eowner));
  size_t len = 1;
  ok(unofficial_napi_snapshot_guest_data(env, nullptr, 0, &len));
  assert(len == 0);
  assert(unofficial_napi_snapshot_set_static(env, 0, ctor, nullptr, 3) == napi_invalid_arg);
  unofficial_napi_realm realm = nullptr;
  assert(unofficial_napi_create_realm(env, nullptr, UNOFFICIAL_NAPI_REALM_MODE_TEMPLATE, 0,
      nullptr, &realm, nullptr, nullptr) == napi_generic_failure);
  napi_value root = nullptr;
  assert(unofficial_napi_create_realm(env, nullptr, UNOFFICIAL_NAPI_REALM_MODE_RESTORE, 0,
      nullptr, &realm, nullptr, &root) == napi_generic_failure);
  ok(unofficial_napi_release_env(eowner, nullptr));
}

static void check_manifest(const char* path) {
  const unofficial_napi_snapshot_env_options builder{
      sizeof(builder), UNOFFICIAL_NAPI_SNAPSHOT_ENV_OPTIONS_VERSION,
      UNOFFICIAL_NAPI_SNAPSHOT_ROLE_BUILDER, 0, nullptr,
      kKey, sizeof(kKey) - 1, path, std::strlen(path)};
  napi_env env = nullptr;
  unofficial_napi_env_owner eowner = nullptr;
  ok(unofficial_napi_create_env_with_snapshot(10, nullptr, &builder, &env, &eowner));
  assert(unofficial_napi_snapshot_set_static(env, 0, ctor, nullptr, 3) == napi_invalid_arg);
  napi_handle_scope hs = nullptr;
  ok(napi_open_handle_scope(env, &hs));
  unofficial_napi_realm realm = nullptr;
  ok(unofficial_napi_create_realm(env, nullptr, UNOFFICIAL_NAPI_REALM_MODE_TEMPLATE, 0,
      nullptr, &realm, nullptr, nullptr));
  unofficial_napi_realm_scope scope;
  ok(unofficial_napi_enter_realm(env, realm, &scope));
  napi_value parent = nullptr, prototype = nullptr;
  ok(unofficial_napi_define_binding(env, UNOFFICIAL_NAPI_BINDING_CLASS, "Parent", 6,
      ctor, nullptr, 0, nullptr, 0, nullptr, nullptr, &parent, &prototype));
  auto root = eval(env, "({})");
  ok(napi_set_named_property(env, root, "Parent", parent));
  ok(unofficial_napi_leave_realm(env, scope));
  const uint32_t manifest[] = {1, 0, 3};
  const char guest_data[] = "manifest";
  assert(unofficial_napi_snapshot_commit(env, realm, root, guest_data, sizeof(guest_data), nullptr) ==
         napi_invalid_arg);
  ok(NapiV8SnapshotCommitWithManifest(env, realm, root, guest_data, sizeof(guest_data), nullptr,
      reinterpret_cast<const char*>(manifest), sizeof(manifest)));
  ok(napi_close_handle_scope(env, hs));
  ok(unofficial_napi_release_env(eowner, nullptr));

  auto artifact = open_artifact(path, kKey);
  const unofficial_napi_snapshot_env_options restore{
      sizeof(restore), UNOFFICIAL_NAPI_SNAPSHOT_ENV_OPTIONS_VERSION,
      UNOFFICIAL_NAPI_SNAPSHOT_ROLE_RESTORE, 0, artifact, nullptr, 0, nullptr, 0};
  ok(unofficial_napi_create_env_with_snapshot(10, nullptr, &restore, &env, &eowner));
  unofficial_napi_close_snapshot(artifact);
  ok(napi_open_handle_scope(env, &hs));
  size_t length = 1;
  ok(unofficial_napi_snapshot_guest_data(env, nullptr, 0, &length));
  assert(length == 0);
  assert(unofficial_napi_create_realm(env, nullptr, UNOFFICIAL_NAPI_REALM_MODE_RESTORE, 0,
      nullptr, &realm, nullptr, &root) == napi_generic_failure);
  assert(unofficial_napi_snapshot_set_static(env, UINT32_MAX, ctor, nullptr, 3) == napi_invalid_arg);
  assert(unofficial_napi_snapshot_set_static(env, 0, nullptr, nullptr, 3) == napi_invalid_arg);
  assert(unofficial_napi_snapshot_set_static(env, 0, ctor, nullptr, 4) == napi_invalid_arg);
  ok(unofficial_napi_snapshot_set_static(env, 0, ctor, nullptr, 3));
  assert(unofficial_napi_snapshot_set_static(env, 0, ctor, nullptr, 3) == napi_invalid_arg);
  ok(unofficial_napi_snapshot_guest_data(env, nullptr, 0, &length));
  assert(length == sizeof(guest_data));
  ok(unofficial_napi_create_realm(env, nullptr, UNOFFICIAL_NAPI_REALM_MODE_RESTORE, 0,
      nullptr, &realm, nullptr, &root));
  ok(unofficial_napi_enter_realm(env, realm, &scope));
  ok(napi_get_named_property(env, root, "Parent", &parent));
  napi_value instance = nullptr;
  ok(napi_new_instance(env, parent, 0, nullptr, &instance));
  napi_value child = nullptr;
  ok(unofficial_napi_define_binding(env, UNOFFICIAL_NAPI_BINDING_CLASS, "Child", 5,
      ctor, nullptr, 0, parent, 0, nullptr, nullptr, &child, &prototype));
  ok(napi_new_instance(env, child, 0, nullptr, &instance));
  bool inherits = false;
  ok(napi_instanceof(env, instance, parent, &inherits));
  assert(inherits);
  ok(unofficial_napi_leave_realm(env, scope));
  ok(napi_close_handle_scope(env, hs));
  ok(unofficial_napi_release_env(eowner, nullptr));
}

static void restore(unofficial_napi_snapshot artifact, int expected_answer,
                    const char* expected_guest_data, std::barrier<>* live = nullptr) {
  const unofficial_napi_snapshot_env_options request{
      sizeof(request), UNOFFICIAL_NAPI_SNAPSHOT_ENV_OPTIONS_VERSION,
      UNOFFICIAL_NAPI_SNAPSHOT_ROLE_RESTORE, 0, artifact,
      nullptr, 0, nullptr, 0};
  napi_env env = nullptr;
  unofficial_napi_env_owner eowner = nullptr;
  ok(unofficial_napi_create_env_with_snapshot(10, nullptr, &request, &env, &eowner));
  unofficial_napi_close_snapshot(artifact);
  if (live != nullptr) live->arrive_and_wait();

  assert(owner(env) == 0);
  napi_handle_scope hs = nullptr;
  ok(napi_open_handle_scope(env, &hs));
  std::string guest(std::strlen(expected_guest_data) + 1, '\0');
  size_t len = 0;
  ok(unofficial_napi_snapshot_guest_data(env, guest.data(), guest.size(), &len));
  assert(len == guest.size() && !std::strcmp(guest.c_str(), expected_guest_data));
  unofficial_napi_realm_options opts;
  unofficial_napi_realm_options_init(&opts);
  opts.allow_code_gen_strings = false;
  opts.own_microtask_queue = true;
  unofficial_napi_realm realm = nullptr;
  napi_value root = nullptr, value = nullptr;
  assert(unofficial_napi_create_realm(env, &opts, UNOFFICIAL_NAPI_REALM_MODE_RESTORE, 2,
      nullptr, &realm, nullptr, &root) == napi_invalid_arg);
  ok(unofficial_napi_create_realm(env, &opts, UNOFFICIAL_NAPI_REALM_MODE_RESTORE, 0,
      reinterpret_cast<void*>(0x80000043), &realm, nullptr, &root));
  unofficial_napi_realm_scope scope;
  ok(unofficial_napi_enter_realm(env, realm, &scope));
  assert(owner(env) == 0x80000043);
  ok(napi_get_named_property(env, root, "answer", &value));
  int32_t answer = 0;
  ok(napi_get_value_int32(env, value, &answer));
  assert(answer == expected_answer);
  ok(napi_get_value_int32(env, eval(env, "savedData + savedAccessor"), &answer));
  assert(answer == 46);
  assert(napi_run_script(env, text(env, "eval('1')"), &value) == napi_pending_exception);
  ok(napi_get_and_clear_last_exception(env, &value));
  napi_value name = nullptr;
  ok(napi_get_named_property(env, value, "name", &name));
  char error_name[16]{};
  ok(napi_get_value_string_utf8(env, name, error_name, sizeof(error_name), nullptr));
  assert(std::strcmp(error_name, "EvalError") == 0);
  ok(unofficial_napi_leave_realm(env, scope));
  auto queue_probe = unofficial_napi_js_source_from_text(text(env,
      "globalThis.queueProbe=0; Promise.resolve().then(()=>{queueProbe=19})"));
  ok(unofficial_napi_run_script(env, realm, &queue_probe, text(env, "queue.js"), 0, 0,
                               -1, true, false, false, nullptr, &value));
  ok(unofficial_napi_enter_realm(env, realm, &scope));
  ok(napi_get_value_int32(env, eval(env, "queueProbe"), &answer));
  assert(answer == 19);
  ok(unofficial_napi_leave_realm(env, scope));
  ok(napi_close_handle_scope(env, hs));
  if (live != nullptr) live->arrive_and_wait();
  ok(unofficial_napi_release_env(eowner, nullptr));
}

int main(int argc, char** argv) {
  assert(argc >= 3);
  const std::string arm = argv[1];
  const char* path = argv[2];
  unofficial_napi_runtime_options runtime{};
  runtime.size = sizeof(runtime);
  runtime.version = UNOFFICIAL_NAPI_RUNTIME_OPTIONS_VERSION;
  ok(unofficial_napi_configure_runtime(&runtime));
  if (arm == "bake" || arm == "write-error") {
    bake(path, kKey, 42, "payload", arm == "write-error");
    check_fresh();
  } else if (arm == "restore") {
    auto artifact = open_artifact(path, kKey);
    check_fresh();
    restore(artifact, 42, "payload");
  } else if (arm == "manifest") {
    check_manifest(path);
  } else if (arm == "multirestore") {
    assert(argc == 4);
    constexpr char other_key[] = "plan-smoke-v8-other";
    bake(path, kKey, 42, "payload");
    bake(argv[3], other_key, 83, "other payload");
    auto first = open_artifact(path, kKey);
    auto second = open_artifact(argv[3], other_key);
    check_fresh();
    std::barrier live(2);
    std::thread a(restore, first, 42, "payload", &live);
    std::thread b(restore, second, 83, "other payload", &live);
    a.join();
    b.join();
  } else {
    assert(arm == "shape");
    napi_env env = nullptr;
    unofficial_napi_env_owner eowner = nullptr;
    ok(unofficial_napi_create_env(10, nullptr, &env, &eowner));
    napi_handle_scope hs = nullptr;
    ok(napi_open_handle_scope(env, &hs));
    unofficial_napi_realm realm = nullptr;
    napi_value global = nullptr;
    ok(unofficial_napi_create_realm(env, nullptr, UNOFFICIAL_NAPI_REALM_MODE_FRESH, 0,
        reinterpret_cast<void*>(0x80000044), &realm, &global, nullptr));
    unofficial_napi_realm_scope scope;
    ok(unofficial_napi_enter_realm(env, realm, &scope));
    napi_property_descriptor props[2]{};
    props[0].utf8name = "m"; props[0].method = method; props[0].attributes = napi_default_method;
    props[1].utf8name = "x"; props[1].getter = get; props[1].setter = set; props[1].attributes = napi_configurable;
    const uint8_t lengths[] = {4, 0};
    napi_value c = nullptr, p = nullptr, child = nullptr, child_p = nullptr, f = nullptr;
    ok(unofficial_napi_define_binding(env, UNOFFICIAL_NAPI_BINDING_CLASS, "Base", 4, ctor, nullptr,
        2, nullptr, 2, props, lengths, &c, &p));
    ok(unofficial_napi_define_binding(env, UNOFFICIAL_NAPI_BINDING_CLASS, "Child", 5, ctor, nullptr,
        1, c, 0, nullptr, nullptr, &child, &child_p));
    ok(unofficial_napi_define_binding(env, UNOFFICIAL_NAPI_BINDING_METHOD, "solo", 4, method, nullptr,
        3, nullptr, 0, nullptr, nullptr, &f, nullptr));
    ok(unofficial_napi_leave_realm(env, scope));
    assert(owner(env) == 0);
    napi_value main = nullptr;
    ok(napi_get_global(env, &main));
    ok(napi_set_named_property(env, main, "Base", c));
    ok(napi_set_named_property(env, main, "Child", child));
    ok(napi_set_named_property(env, main, "solo", f));
    const char* script = R"(
      const p = Base.prototype, x = Object.getOwnPropertyDescriptor(p,'x');
      const pd = Object.getOwnPropertyDescriptor(Base,'prototype');
      const cd = Object.getOwnPropertyDescriptor(p,'constructor');
      if (!(
        Base.length===2 && Child.length===1 && p.m.length===4 && solo.length===3 &&
        Base.name==='Base' && x.get.name==='get x' && x.set.name==='set x' &&
        x.get.length===0 && x.set.length===1 &&
        x.get===Object.getOwnPropertyDescriptor(p,'x').get &&
        !pd.writable && !pd.configurable && !pd.enumerable &&
        cd.writable && cd.configurable && !cd.enumerable &&
        !Object.hasOwn(solo,'prototype') && !Object.hasOwn(p.m,'prototype') &&
        Object.getPrototypeOf(Child)===Base && Object.getPrototypeOf(Child.prototype)===p &&
        x.get.call({},1)==='getter' && x.set.call({})===undefined &&
        (()=>{try{new solo;return false}catch(e){return e.name==='TypeError'}})()
      )) throw new Error('binding shape');
      [solo(), solo.call(2), solo.call(null)];
    )";
    napi_value result = eval(env, script);
    assert(getter_count == 1 && setter_count == 1 && owner(env) == 0);
    for (uint32_t i = 0; i != 3; ++i) {
      uint32_t received = 0;
      napi_value value;
      ok(napi_get_element(env, result, i, &value));
      ok(napi_get_value_uint32(env, value, &received));
      // The callback ran in its defining realm, independently of receiver
      // coercion in the V8 public FunctionCallbackInfo API.
      assert(received >= 0x80000044 && received <= 0x80000044 + napi_bigint);
    }
    ok(napi_close_handle_scope(env, hs));
    ok(unofficial_napi_release_env(eowner, nullptr));
  }
  std::printf("%s passed\n", arm.c_str());
}
