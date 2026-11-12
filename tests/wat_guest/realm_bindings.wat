(module
  (import "napi_extension_wasmer_v0" "unofficial_napi_configure_runtime"
    (func $configure (param i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_env"
    (func $create_env (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_release_env"
    (func $release_env (param i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_create_realm"
    (func $realm (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_enter_realm"
    (func $enter (param i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_leave_realm"
    (func $leave (param i32 i64) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_get_current_realm_data"
    (func $owner (param i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_define_binding"
    (func $binding (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_snapshot_commit"
    (func $commit (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_snapshot_guest_data"
    (func $snapshot_data (param i32 i32 i32 i32) (result i32)))
  (import "napi_extension_wasmer_v0" "unofficial_napi_run_script"
    (func $run_script (param i32 i32 i32 i32 i32 i32 i64 i32 i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_open_handle_scope" (func $open_scope (param i32 i32) (result i32)))
  (import "napi" "napi_close_handle_scope" (func $close_scope (param i32 i32) (result i32)))
  (import "napi" "napi_create_object" (func $object (param i32 i32) (result i32)))
  (import "napi" "napi_create_int32" (func $number (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_value_int32" (func $int32 (param i32 i32 i32) (result i32)))
  (import "napi" "napi_get_undefined" (func $undefined (param i32 i32) (result i32)))
  (import "napi" "napi_create_string_utf8" (func $string (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_define_properties" (func $properties (param i32 i32 i32 i32) (result i32)))
  (import "napi" "napi_set_named_property" (func $set (param i32 i32 i32 i32) (result i32)))

  (memory (export "memory") 1)
  (table (export "__indirect_function_table") 3 funcref)
  (global $setter_calls (mut i32) (i32.const 0))
  (data (i32.const 160) "root\00value\00meth\00Ctor\00realm.js\00payload")
  (elem (i32.const 1) $get $set_value)

  (export "realm" (func $realm))
  (export "enter" (func $enter))
  (export "leave" (func $leave))
  (export "owner" (func $owner))
  (export "binding" (func $binding))
  (export "object" (func $object))
  (export "properties" (func $properties))
  (export "set" (func $set))
  (export "commit" (func $commit))
  (export "snapshot_data" (func $snapshot_data))
  (export "open_scope" (func $open_scope))
  (export "close_scope" (func $close_scope))
  (export "release" (func $release_env))

  (func (export "create") (result i32)
    (if (call $configure (i32.const 0)) (then unreachable))
    (call $create_env (i32.const 8) (i32.const 0) (i32.const 8) (i32.const 12)))

  (func (export "remove_callback")
    (table.set (i32.const 1) (ref.null func)))

  ;; Return the count minted before the value table stopped issuing handles.
  (func (export "fill_values") (param $env i32) (param $limit i32) (result i32)
    (local $count i32)
    (block $end
      (loop $next
        (br_if $end (i32.eq (local.get $count) (local.get $limit)))
        (i32.store (i32.const 96) (i32.const 0))
        (br_if $end (call $undefined (local.get $env) (i32.const 96)))
        (br_if $end (i32.eqz (i32.load (i32.const 96))))
        (local.set $count (i32.add (local.get $count) (i32.const 1)))
        (br $next)))
    (local.get $count))

  ;; Script source is at 1024. The three words at 64 are the source
  ;; descriptor, followed by filename and evaluation result handles.
  (func (export "eval") (param $env i32) (param $realm i32) (param $len i32) (result i32)
    (if (call $string (local.get $env) (i32.const 1024) (local.get $len) (i32.const 68))
      (then unreachable))
    (if (call $string (local.get $env) (i32.const 181) (i32.const 8) (i32.const 76))
      (then unreachable))
    (call $run_script (local.get $env) (local.get $realm) (i32.const 64)
      (i32.load (i32.const 76)) (i32.const 0) (i32.const 0) (i64.const -1)
      (i32.const 1) (i32.const 0) (i32.const 0) (i32.const 0) (i32.const 80)))

  (func (export "result") (param $env i32) (result i32)
    (if (call $int32 (local.get $env) (i32.load (i32.const 80)) (i32.const 84))
      (then unreachable))
    (i32.load (i32.const 84)))

  (func (export "setter_calls") (result i32) (global.get $setter_calls))

  ;; Report all 32 bits of the creation owner. The prefill detects a partial
  ;; write or sign extension in the guest ABI.
  (func $get (param $env i32) (param $info i32) (result i32)
    (i64.store (i32.const 112) (i64.const -1))
    (if (call $owner (local.get $env) (i32.const 112)) (then unreachable))
    (if (i32.load (i32.const 116)) (then unreachable))
    (if (call $number (local.get $env) (i32.load (i32.const 112)) (i32.const 120))
      (then unreachable))
    (i32.load (i32.const 120)))

  (func $set_value (param $env i32) (param $info i32) (result i32)
    (global.set $setter_calls (i32.add (global.get $setter_calls) (i32.const 1)))
    (i32.const 0)))
