#ifndef NAPI_PRIVATE_HEAP_LIMIT_H_
#define NAPI_PRIVATE_HEAP_LIMIT_H_

#include "js_native_api.h"

// Provider-private heap accounting hooks used by the WASIX host bridge.
// These are not part of the native module API or the guest import surface.
#ifdef __cplusplus
extern "C" {
#endif

// Committed bytes of the environment's old generation: every heap space the
// engine's hard heap limit governs (everything but the young generation and
// read-only space). Intended for near-heap-limit callbacks, which must return
// a limit that covers this figure; the engine admits the first large young
// object without consulting the limit and aborts the process after the next
// collection if the limit still does not cover the promoted object. Engines
// that do not expose the figure report zero.
napi_status NAPI_CDECL snapi_private_get_heap_committed_old_generation(
    napi_env env,
    size_t* result);

// Runs a full collection if the environment's heap holds committed memory
// beyond its hard limit that no collection has examined yet. The engine admits
// the first large young object without consulting the heap limit, so after a
// script returns the heap may exceed the limit with no near-heap-limit
// callback having run; an idle environment would keep that memory
// unaccounted for indefinitely. Call from the environment's thread after
// running JavaScript. `old_generation_limit` is the engine's current hard
// old-generation limit as the embedder last set or answered it (the figure the
// engine's own post-collection check uses); zero means unknown, in which case
// the engine's reported total heap limit is used instead, which under-triggers
// by the young-generation allowance. `*collected` reports whether a collection
// ran. Engines without the behaviour report `false`.
napi_status NAPI_CDECL snapi_private_collect_garbage_if_over_heap_limit(
    napi_env env,
    size_t old_generation_limit,
    bool* collected);
#ifdef __cplusplus
}
#endif

#endif  // NAPI_PRIVATE_HEAP_LIMIT_H_
