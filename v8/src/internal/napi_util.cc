#include "internal/napi_util.h"

#include "internal/napi_v8_env.h"

void napi_v8_drain_finalizer_queue_microtask(void* data) {
  auto* env = static_cast<napi_env>(data);
  if (env != nullptr) {
    env->DrainFinalizerQueue();
  }
}
