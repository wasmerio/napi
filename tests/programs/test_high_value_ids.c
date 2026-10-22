// napi_value ids are unsigned 32-bit handles. The WASIX bridge encodes an id
// as ((slot_index + 1) << 12) | generation, so once about 2^19 values are live
// it mints ids with bit 31 set. Every import must treat those ids as valid
// unsigned handles, not as negative i32s to reject or clamp to null.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "node_api.h"
#include "unofficial_napi.h"

// Live values kept in one scope before the values under test are minted.
// Slot 2^19 - 1 is the first whose id has bit 31 set; the slot table holds up
// to 2^20 - 1 live values.
#define FILLER_VALUES 540000

static int ExpectInt32(napi_env env, napi_value value, int32_t expected,
                       const char* what) {
  int32_t actual = 0;
  NAPI_CALL(env, napi_get_value_int32(env, value, &actual));
  if (actual != expected) {
    printf("FAIL: %s: expected %d, got %d\n", what, (int)expected, (int)actual);
    return 1;
  }
  return 0;
}

static int RunScript(napi_env env, const char* source, napi_value* result) {
  napi_value script;
  NAPI_CALL(env, napi_create_string_utf8(env, source, NAPI_AUTO_LENGTH, &script));
  NAPI_CALL(env, napi_run_script(env, script, result));
  return 0;
}

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  napi_handle_scope scope;
  NAPI_CALL(env, napi_open_handle_scope(env, &scope));
  for (int i = 0; i < FILLER_VALUES; i++) {
    napi_value filler;
    NAPI_CALL(env, napi_create_int32(env, i, &filler));
  }

  napi_value number;
  napi_value object;
  napi_value string;
  napi_value typed_array;
  NAPI_CALL(env, napi_create_int32(env, 42, &number));
  NAPI_CALL(env, napi_create_object(env, &object));
  NAPI_CALL(env, napi_create_string_utf8(env, "high", NAPI_AUTO_LENGTH, &string));
  NAPI_CALL(env, napi_set_named_property(env, object, "answer", number));
  if (RunScript(env, "globalThis.__high_ids = new Uint8Array([10, 20, 30, 40])",
                &typed_array) != 0) {
    return 1;
  }

#ifdef __wasm__
  // Native napi_values are host pointers; only wasm32 ids are bounded here.
  CHECK_OR_FAIL((uint32_t)(uintptr_t)number >= 0x80000000u &&
                    (uint32_t)(uintptr_t)object >= 0x80000000u &&
                    (uint32_t)(uintptr_t)typed_array >= 0x80000000u,
                "test values were not minted with ids >= 2^31");
#endif

  // Core N-API calls on high ids.
  napi_valuetype type;
  NAPI_CALL(env, napi_typeof(env, number, &type));
  CHECK_OR_FAIL(type == napi_number, "napi_typeof on a high id");
  if (ExpectInt32(env, number, 42, "napi_get_value_int32 on a high id") != 0) {
    return 1;
  }
  napi_value answer;
  NAPI_CALL(env, napi_get_named_property(env, object, "answer", &answer));
  if (ExpectInt32(env, answer, 42, "named property read through a high id") != 0) {
    return 1;
  }
  char text[16];
  size_t text_length = 0;
  NAPI_CALL(env, napi_get_value_string_utf8(env, string, text, sizeof(text),
                                            &text_length));
  CHECK_OR_FAIL(text_length == 4 && strcmp(text, "high") == 0,
                "napi_get_value_string_utf8 on a high id");
  bool is_typed_array = false;
  NAPI_CALL(env, napi_is_typedarray(env, typed_array, &is_typed_array));
  CHECK_OR_FAIL(is_typed_array, "napi_is_typedarray on a high id");

  // Unofficial imports that used to reject (`value <= 0`) or null
  // (`value.max(0)`) ids with bit 31 set.
  napi_value keys;
  NAPI_CALL(env, unofficial_napi_get_own_non_index_properties(env, object, 0,
                                                              &keys));
  uint32_t key_count = 0;
  NAPI_CALL(env, napi_get_array_length(env, keys, &key_count));
  CHECK_OR_FAIL(key_count == 1,
                "unofficial_napi_get_own_non_index_properties on a high id");

  unofficial_napi_buffer_lease lease = NULL;
  uint8_t* lease_data = NULL;
  NAPI_CALL(env, unofficial_napi_acquire_buffer_lease(
                     env, typed_array, 1, 2,
                     unofficial_napi_buffer_access_readwrite, &lease,
                     (void**)&lease_data));
  CHECK_OR_FAIL(lease != NULL && lease_data != NULL && lease_data[0] == 20 &&
                    lease_data[1] == 30,
                "unofficial_napi_acquire_buffer_lease on a high id");
  lease_data[0] = 21;
  lease_data[1] = 31;
  NAPI_CALL(env, unofficial_napi_release_buffer_lease(env, lease, true));
  napi_value leased_bytes;
  if (RunScript(env, "__high_ids[1] * 100 + __high_ids[2]", &leased_bytes) != 0 ||
      ExpectInt32(env, leased_bytes, 2131,
                  "buffer lease writes through a high id") != 0) {
    return 1;
  }

  unofficial_napi_message message = NULL;
  NAPI_CALL(env, unofficial_napi_message_create(env, object, &message));
  napi_value cloned;
  NAPI_CALL(env, unofficial_napi_message_take(env, message, &cloned));
  napi_value cloned_answer;
  NAPI_CALL(env, napi_get_named_property(env, cloned, "answer", &cloned_answer));
  if (ExpectInt32(env, cloned_answer, 42,
                  "unofficial_napi_message_create on a high id") != 0) {
    return 1;
  }

  // A reference to a high id outlives its scope like any other.
  napi_ref ref;
  NAPI_CALL(env, napi_create_reference(env, object, 1, &ref));
  NAPI_CALL(env, napi_close_handle_scope(env, scope));
  napi_value resolved;
  NAPI_CALL(env, napi_get_reference_value(env, ref, &resolved));
  NAPI_CALL(env, napi_get_named_property(env, resolved, "answer", &answer));
  if (ExpectInt32(env, answer, 42, "reference to a high id") != 0) {
    return 1;
  }
  NAPI_CALL(env, napi_delete_reference(env, ref));

  return PrintSuccess("TEST_HIGH_VALUE_IDS");
}
