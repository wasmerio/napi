// Guest addresses are unsigned 32-bit offsets. A wasm32 guest whose linear
// memory has grown past 2 GiB hands out pointers with bit 31 set; every import
// must read from and write through them instead of treating them as negative
// (rejected inputs, silently skipped outputs).
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "napi_test_helpers.h"
#include "node_api.h"
#include "unofficial_napi.h"

#define WASM_PAGE_SIZE 65536u

#ifdef __wasm__
// Claims a page at or above 2 GiB. memory.grow hands the caller exclusive
// ownership of the pages it adds (wasi-libc's sbrk and the host's guest heap
// use the same protocol), so the page is safe to use as scratch memory.
static uint8_t* ClaimHighPage(void) {
  const size_t high_end_pages = (0x80000000u / WASM_PAGE_SIZE) + 1;
  size_t current = __builtin_wasm_memory_size(0);
  size_t delta = current < high_end_pages ? high_end_pages - current : 1;
  size_t previous = __builtin_wasm_memory_grow(0, delta);
  if (previous == (size_t)-1) return NULL;
  return (uint8_t*)((previous + delta - 1) * WASM_PAGE_SIZE);
}
#else
static uint64_t native_high_page[WASM_PAGE_SIZE / sizeof(uint64_t)];
static uint8_t* ClaimHighPage(void) { return (uint8_t*)native_high_page; }
#endif

int main(void) {
  napi_env env = napi_wasm_init_env();
  CHECK_OR_FAIL(env != NULL, "napi_wasm_init_env returned NULL");

  uint8_t* high = ClaimHighPage();
  CHECK_OR_FAIL(high != NULL, "could not grow memory past 2 GiB");
#ifdef __wasm__
  CHECK_OR_FAIL((uintptr_t)high >= 0x80000000u,
                "scratch page is not above 2 GiB");
#endif

  char* text = (char*)high;
  char* property_name = (char*)(high + 64);
  char* text_out = (char*)(high + 128);
  size_t* text_out_length = (size_t*)(high + 256);
  napi_value* value_out = (napi_value*)(high + 264);
  napi_typedarray_type* ta_type = (napi_typedarray_type*)(high + 272);
  size_t* ta_length = (size_t*)(high + 280);
  void** ta_data = (void**)(high + 288);
  napi_value* ta_arraybuffer = (napi_value*)(high + 296);
  size_t* ta_offset = (size_t*)(high + 304);
  unofficial_napi_heap_statistics* stats =
      (unofficial_napi_heap_statistics*)(high + 1024);

  strcpy(text, "high-memory");
  strcpy(property_name, "highName");

  // Inputs read from >= 2 GiB: NUL-terminated and explicit-length strings.
  NAPI_CALL(env, napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, value_out));
  napi_value full = *value_out;
  napi_value prefix;
  NAPI_CALL(env, napi_create_string_utf8(env, text, 4, &prefix));

  // Outputs written to >= 2 GiB: string bytes and length.
  memset(text_out, 0x7f, 32);
  *text_out_length = 12345;
  NAPI_CALL(env, napi_get_value_string_utf8(env, full, text_out, 32,
                                            text_out_length));
  CHECK_OR_FAIL(*text_out_length == 11 && strcmp(text_out, "high-memory") == 0,
                "string read from / written to memory above 2 GiB");
  NAPI_CALL(env, napi_get_value_string_utf8(env, prefix, text_out, 32,
                                            text_out_length));
  CHECK_OR_FAIL(*text_out_length == 4 && strcmp(text_out, "high") == 0,
                "explicit-length string read from memory above 2 GiB");

  // A property name read from >= 2 GiB.
  napi_value object;
  NAPI_CALL(env, napi_create_object(env, &object));
  NAPI_CALL(env, napi_set_named_property(env, object, property_name, full));
  napi_value property;
  NAPI_CALL(env, napi_get_named_property(env, object, "highName", &property));
  bool same = false;
  NAPI_CALL(env, napi_strict_equals(env, property, full, &same));
  CHECK_OR_FAIL(same, "property name read from memory above 2 GiB");

  // Optional out-pointers above 2 GiB must be written, not skipped.
  napi_value script;
  napi_value typed_array;
  NAPI_CALL(env, napi_create_string_utf8(
                     env, "new Uint8Array(new ArrayBuffer(8), 2, 4)",
                     NAPI_AUTO_LENGTH, &script));
  NAPI_CALL(env, napi_run_script(env, script, &typed_array));
  *ta_type = (napi_typedarray_type)-1;
  *ta_length = 0;
  *ta_data = NULL;
  *ta_arraybuffer = NULL;
  *ta_offset = 0;
  NAPI_CALL(env, napi_get_typedarray_info(env, typed_array, ta_type, ta_length,
                                          ta_data, ta_arraybuffer, ta_offset));
  CHECK_OR_FAIL(*ta_type == napi_uint8_array && *ta_length == 4 &&
                    *ta_offset == 2 && *ta_data != NULL &&
                    *ta_arraybuffer != NULL,
                "napi_get_typedarray_info outputs above 2 GiB");

  // A versioned in/out struct above 2 GiB.
  unofficial_napi_heap_statistics_init(stats);
  NAPI_CALL(env, unofficial_napi_get_heap_statistics(env, stats));
  CHECK_OR_FAIL(stats->valid_fields != 0 && stats->total_heap_size != 0,
                "unofficial_napi_get_heap_statistics struct above 2 GiB");

  return PrintSuccess("TEST_HIGH_GUEST_POINTERS");
}
