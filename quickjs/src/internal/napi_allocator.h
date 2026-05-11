#ifndef NAPI_QUICKJS_ALLOCATOR_H_
#define NAPI_QUICKJS_ALLOCATOR_H_

#include "napi_lifetime_macros.h"

#include <cstdint>
#include <cstddef>
#include <vector>

template <typename T>
class napi_allocator__
{
public:
#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS)
  explicit napi_allocator__(quickjs::detail::napi_lifetime_slot_kind slot_kind)
      : slot_kind_(slot_kind)
  {
  }
#else
  napi_allocator__() = default;
#endif

  template <typename... Args>
  T *allocate(Args &&...args)
  {
    size_t index;
    bool grew = false;
    if (!free_indexes_.empty())
    {
      index = free_indexes_.back();
      free_indexes_.pop_back();
    }
    else
    {
      index = entries_.size();
      entries_.emplace_back();
      grew = true;
    }

    entries_[index].initialize(static_cast<Args &&>(args)...);
#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS)
    record_slot_delta(grew ? 1 : 0, 1);
#endif
    return handle_from_index(base_index_ + index);
  }

  T *get(T *handle)
  {
    size_t index = 0;
    if (!local_index_from_handle(handle, &index))
      return nullptr;
    T &entry = entries_[index];
    return entry.is_active() ? &entry : nullptr;
  }

  const T *get(T *handle) const
  {
    size_t index = 0;
    if (!local_index_from_handle(handle, &index))
      return nullptr;
    const T &entry = entries_[index];
    return entry.is_active() ? &entry : nullptr;
  }

  void release(T *handle)
  {
    size_t index = 0;
    if (!local_index_from_handle(handle, &index))
      return;
    T &entry = entries_[index];
    if (!entry.is_active())
      return;
    entry.release();
    free_indexes_.push_back(index);
#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS)
    record_slot_delta(0, -1);
#endif
  }

  void close()
  {
    size_t total = entries_.size();
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
    {
      if (it->is_active())
      {
        it->release();
#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS)
        record_slot_delta(0, -1);
#endif
      }
    }
    entries_.clear();
    free_indexes_.clear();
#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS)
    record_slot_delta(-static_cast<std::ptrdiff_t>(total), 0);
#endif
  }

  void reserve_prefix(size_t count)
  {
    if (base_index_ < count)
      base_index_ = count;
  }

  size_t slot_count() const
  {
    return base_index_ + entries_.size();
  }

private:
  static T *handle_from_index(size_t index)
  {
    return reinterpret_cast<T *>(static_cast<uintptr_t>(index + 1));
  }

  static bool index_from_handle(T *handle, size_t *index)
  {
    uintptr_t raw = reinterpret_cast<uintptr_t>(handle);
    if (raw == 0)
      return false;
    if (index != nullptr)
      *index = static_cast<size_t>(raw - 1);
    return true;
  }

  bool local_index_from_handle(T *handle, size_t *index) const
  {
    size_t global_index = 0;
    if (!index_from_handle(handle, &global_index))
      return false;
    if (global_index < base_index_)
      return false;
    size_t local_index = global_index - base_index_;
    if (local_index >= entries_.size())
      return false;
    if (index != nullptr)
      *index = local_index;
    return true;
  }

#if defined(NAPI_QUICKJS_ENABLE_LIFETIME_TRACKER) && defined(NAPI_QUICKJS_ENABLE_LIFETIME_PERIODIC_STATS)
  void record_slot_delta(std::ptrdiff_t total_delta, std::ptrdiff_t active_delta)
  {
    NAPI_QUICKJS_LIFETIME_SLOT_DELTA(slot_kind_, total_delta, active_delta);
  }

  quickjs::detail::napi_lifetime_slot_kind slot_kind_;
#endif
  std::vector<T> entries_;
  std::vector<size_t> free_indexes_;
  size_t base_index_ = 0;
};

#endif // NAPI_QUICKJS_ALLOCATOR_H_
