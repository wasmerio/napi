#include "node_api.h"
#include "unofficial_napi.h"

#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <source_location>

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

int main(int argc, char** argv) {
  assert(argc >= 3);
  const std::string arm = argv[1];
  const char* path = argv[2];
  constexpr char key[] = "plan-smoke-v8";
  unofficial_napi_snapshot_host_options host{};
  host.size = sizeof(host);
  host.version = 1;
  std::string bytes;
  if (arm == "bake" || arm == "write-error") {
    host.role = UNOFFICIAL_NAPI_SNAPSHOT_ROLE_BUILDER;
    host.key = key;
    host.key_length = sizeof(key) - 1;
    host.output_path = path;
    host.output_path_length = std::strlen(path);
    ok(unofficial_napi_set_snapshot_host_options(&host));
  } else if (arm == "restore") {
    std::ifstream file(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(file), {});
    assert(!bytes.empty());
    host.role = UNOFFICIAL_NAPI_SNAPSHOT_ROLE_RESTORE;
    host.key = key;
    host.key_length = sizeof(key) - 1;
    host.artifact = bytes.data();
    host.artifact_length = bytes.size();
    ok(unofficial_napi_set_snapshot_host_options(&host));
    // A different artifact must not replace the process's accepted bytes.
    bytes.back() ^= 1;
    assert(unofficial_napi_set_snapshot_host_options(&host) == napi_generic_failure);
    bytes.clear();
  }
  unofficial_napi_runtime_options runtime{};
  runtime.size = sizeof(runtime);
  runtime.version = UNOFFICIAL_NAPI_RUNTIME_OPTIONS_VERSION;
  ok(unofficial_napi_configure_runtime(&runtime));
  napi_env env = nullptr;
  unofficial_napi_env_owner eowner = nullptr;
  ok(unofficial_napi_create_env(10, nullptr, &env, &eowner));
  assert(owner(env) == 0);
  napi_handle_scope hs = nullptr;
  ok(napi_open_handle_scope(env, &hs));
  unofficial_napi_realm_options opts;
  unofficial_napi_realm_options_init(&opts);
  if (arm == "bake" || arm == "write-error") {
    unofficial_napi_realm realm = nullptr;
    napi_value global = nullptr, root = nullptr;
    ok(unofficial_napi_create_realm(env, &opts, UNOFFICIAL_NAPI_REALM_MODE_TEMPLATE, 0,
        reinterpret_cast<void*>(0x80000042), &realm, &global, &root));
    assert(root == nullptr);
    unofficial_napi_realm_scope scope;
    ok(unofficial_napi_enter_realm(env, realm, &scope));
    assert(owner(env) == 0x80000042);
    root = eval(env, "({answer: 42})");
    const char guest_data[] = "payload";
    assert(unofficial_napi_snapshot_commit(env, realm, root, guest_data, sizeof(guest_data), nullptr) == napi_invalid_arg);
    ok(unofficial_napi_leave_realm(env, scope));
    ok(unofficial_napi_snapshot_commit(env, realm, root, guest_data, sizeof(guest_data), nullptr));
    assert(unofficial_napi_enter_realm(env, realm, &scope) == napi_invalid_arg);
    ok(napi_close_handle_scope(env, hs));
    napi_status result = unofficial_napi_release_env(eowner, nullptr);
    assert(result == (arm == "write-error" ? napi_generic_failure : napi_ok));
    // The next environment must not inherit the consumed builder role.
    ok(unofficial_napi_create_env(10, nullptr, &env, &eowner));
    assert(unofficial_napi_create_realm(env, nullptr, UNOFFICIAL_NAPI_REALM_MODE_TEMPLATE, 0,
        nullptr, &realm, nullptr, nullptr) == napi_generic_failure);
    ok(unofficial_napi_release_env(eowner, nullptr));
  } else if (arm == "restore") {
    char guest[8]{};
    size_t len = 0;
    ok(unofficial_napi_snapshot_guest_data(env, guest, sizeof(guest), &len));
    assert(len == sizeof(guest) && !std::strcmp(guest, "payload"));
    opts.allow_code_gen_strings = false;
    opts.own_microtask_queue = true;
    unofficial_napi_realm realm = nullptr;
    napi_value root = nullptr, value = nullptr;
    assert(unofficial_napi_create_realm(env, &opts, UNOFFICIAL_NAPI_REALM_MODE_RESTORE, 1,
        nullptr, &realm, nullptr, &root) == napi_invalid_arg);
    ok(unofficial_napi_create_realm(env, &opts, UNOFFICIAL_NAPI_REALM_MODE_RESTORE, 0,
        reinterpret_cast<void*>(0x80000043), &realm, nullptr, &root));
    unofficial_napi_realm_scope scope;
    ok(unofficial_napi_enter_realm(env, realm, &scope));
    assert(owner(env) == 0x80000043);
    ok(napi_get_named_property(env, root, "answer", &value));
    int32_t answer = 0;
    ok(napi_get_value_int32(env, value, &answer));
    assert(answer == 42);
    assert(napi_run_script(env, text(env, "eval('1')"), &value) == napi_pending_exception);
    ok(napi_get_and_clear_last_exception(env, &value));
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
    ok(unofficial_napi_release_env(eowner, nullptr));
  } else {
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
