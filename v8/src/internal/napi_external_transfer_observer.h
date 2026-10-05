#ifndef NAPI_V8_EXTERNAL_TRANSFER_OBSERVER_H_
#define NAPI_V8_EXTERNAL_TRANSFER_OBSERVER_H_

// Stack-only private observer used by the Wasm guest-heap bridge. Nested
// native calls restore their caller's observer without an allocation.
struct napi_v8_external_transfer_observer {
  void* finalize_hint = nullptr;
  int* transferred = nullptr;
  napi_v8_external_transfer_observer* previous = nullptr;
};

extern "C" void napi_v8_begin_external_transfer_observation(
    napi_v8_external_transfer_observer* observer, void* finalize_hint,
    int* transferred);
extern "C" void napi_v8_end_external_transfer_observation(
    napi_v8_external_transfer_observer* observer);

#endif  // NAPI_V8_EXTERNAL_TRANSFER_OBSERVER_H_
