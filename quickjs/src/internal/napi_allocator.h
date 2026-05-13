#ifndef NAPI_QUICKJS_ALLOCATOR_H_
#define NAPI_QUICKJS_ALLOCATOR_H_

#include "napi_lifetime_tracker.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>
#include <cassert>

template <typename T>
concept napi_allocator_payload__ = std::destructible<T>;

template <typename T>
concept napi_allocator_owner__ = std::is_class_v<T>;

template <class Handle, napi_allocator_payload__ T, napi_allocator_owner__ Owner, size_t N = 16>
class napi_allocator__
{
  static_assert(N > 0, "N must be greater than zero");

private:
  static constexpr size_t next_power_of_two__(size_t value)
  {
    size_t result = 1;

    while (result < value)
    {
      result <<= 1;
    }

    return result;
  }

  struct slot__
  {
    // Raw payload storage and allocator free-list linkage.
    alignas(T) std::byte storage[sizeof(T)];
    slot__ *next_free = nullptr;
    bool active = false;

    T *data()
    {
      return std::launder(reinterpret_cast<T *>(storage));
    }

    const T *data() const
    {
      return std::launder(reinterpret_cast<const T *>(storage));
    }

    template <typename... Args>
    T *construct(Args &&...args)
    {
      return new (static_cast<void *>(storage)) T(static_cast<Args &&>(args)...);
    }

    void destroy()
    {
      data()->~T();
#ifndef NDEBUG
      std::memset(storage, 0, sizeof(storage));
#endif
    }

    static Handle unsafe_handle_from_data(T *data)
    {
      return reinterpret_cast<Handle>(data);
    }

    static T *unsafe_data_from_handle(Handle handle)
    {
      return reinterpret_cast<T *>(handle);
    }

    static slot__ *unsafe_slot_from_handle(Handle handle)
    {
      if (handle == nullptr)
        return nullptr;

      return reinterpret_cast<slot__ *>(
          reinterpret_cast<char *>(handle) - offsetof(slot__, storage));
    }

    bool owns_handle(Handle handle) const
    {
      return (unsafe_handle_from_data(const_cast<T *>(data())) == handle) && this->active;
    }

    slot__() = default;
    slot__(const slot__ &) = delete;
    slot__(slot__ &&) = delete;
    slot__ &operator=(const slot__ &) = delete;
    slot__ &operator=(slot__ &&) = delete;
  };

  struct block_layout__
  {
    Owner *owner_ = nullptr;

    // Block occupancy and free-list state.
    slot__ *first_free = nullptr;
    size_t active_count = 0;

    // Membership flags for allocator side lists.
    bool listed_available = false;
    bool listed_full = false;

    // Stable storage for payload slots.
    std::array<slot__, N> slots;
  };

  static constexpr size_t block_alignment__ =
      next_power_of_two__(sizeof(block_layout__));

  static_assert((block_alignment__ & (block_alignment__ - 1)) == 0,
                "block alignment must be a power of two");

  struct alignas(block_alignment__) block__ : block_layout__
  {
    block__(Owner *owner) : block_layout__{owner}
    {
      reset_free_list();
    }

    block__(const block__ &) = delete;
    block__ &operator=(const block__ &) = delete;

    static block__ *unsafe_block_from_slot(slot__ *slot)
    {
      if (slot == nullptr)
        return nullptr;

      // regardless of which slot we're in, we can find start of the block__ by zeroing
      // last few bits of hte address, because we require that block__ are allocated at
      // memory locations alligned to sizeof block_layout__
      return reinterpret_cast<block__ *>(
          reinterpret_cast<uintptr_t>(slot) & ~(static_cast<uintptr_t>(block_alignment__) - 1));
    }

    static const block__ *unsafe_block_from_slot(const slot__ *slot)
    {
      if (slot == nullptr)
        return nullptr;

      return reinterpret_cast<const block__ *>(
          reinterpret_cast<uintptr_t>(slot) & ~(static_cast<uintptr_t>(block_alignment__) - 1));
    }

    slot__ *allocate()
    {
      if (this->first_free == nullptr)
        return nullptr;

      slot__ *slot = this->first_free;
      this->first_free = slot->next_free;
      slot->next_free = nullptr;
      slot->active = true;
      ++this->active_count;
      return slot;
    }

    void release(slot__ *slot)
    {
      if (slot == nullptr || !slot->active)
        return;

      slot->active = false;
      slot->next_free = this->first_free;
      this->first_free = slot;
      --this->active_count;
    }

    void close()
    {
      for (size_t i = N; i > 0; --i)
      {
        slot__ &slot = this->slots[i - 1];
        if (slot.active)
        {
          if constexpr (quickjs::detail::napi_lifetime_tracked__<T, Owner>)
            quickjs::detail::napi_lifetime__<T>::record_release(this->owner_, slot.data());
          slot.destroy();
          slot.active = false;
        }
      }

      this->active_count = 0;
      this->listed_available = false;
      this->listed_full = false;
      reset_free_list();
    }

    Owner *owner() const
    {
      return this->owner_;
    }

    bool is_full() const
    {
      return this->active_count == N;
    }

    bool is_available() const
    {
      return this->active_count < N;
    }

  private:
    void reset_free_list()
    {
      this->first_free = nullptr;
      for (size_t i = N; i > 0; --i)
      {
        slot__ &slot = this->slots[i - 1];
        slot.next_free = this->first_free;
        this->first_free = &slot;
      }
    }
  };
  static_assert(sizeof(block__) == block_alignment__,
                "napi_allocator__ block must fit exactly in its alignment region");

public:
  explicit napi_allocator__(Owner *owner) : owner_(owner) {}
  napi_allocator__(const napi_allocator__ &) = delete;
  napi_allocator__(napi_allocator__ &&other) = delete;
  napi_allocator__ &operator=(const napi_allocator__ &) = delete;
  napi_allocator__ &operator=(napi_allocator__ &&other) = delete;

  ~napi_allocator__()
  {
    close();
  }

  template <typename... Args>
    requires std::constructible_from<T, Args...>
  T *allocate(Args &&...args)
  {
    block__ *block = first_available_block();
    if (block == nullptr)
      return nullptr;

    slot__ *slot = block->allocate();
    if (slot == nullptr)
      return nullptr;

    T *data = slot->construct(static_cast<Args &&>(args)...);
    if constexpr (quickjs::detail::napi_lifetime_tracked__<T, Owner>)
      quickjs::detail::napi_lifetime__<T>::record_create(owner_, data);

    if (block->is_full())
    {
      block->listed_available = false;
      available_blocks_.pop_back();
      block->listed_full = true;
      full_blocks_.push_back(block);
    }

    return data;
  }

  static Handle unsafe_handle_from_data(T *data)
  {
    return slot__::unsafe_handle_from_data(data);
  }

  static T *unsafe_data_from_handle(const Handle handle)
  {
    return slot__::unsafe_data_from_handle(handle);
  }

  static std::pair<T *, Owner *> unsafe_data_with_owner_from_handle(Handle handle)
  {
    slot__ *slot = slot__::unsafe_slot_from_handle(handle);
    const block__ *block = block__::unsafe_block_from_slot(slot);

    assert(slot->owns_handle(handle));

    return {slot->data(), block->owner()};
  }

  bool owns_handle(Handle handle) const
  {
    slot__ *slot = slot__::unsafe_slot_from_handle(handle);
    block__ *block = block__::unsafe_block_from_slot(slot);

    return owns_block(block) && (slot_index(block, slot) < N) && slot->owns_handle(handle);
  }

  void release(Handle handle)
  {
    slot__ *slot = slot__::unsafe_slot_from_handle(handle);
    block__ *block = block__::unsafe_block_from_slot(slot);

    assert(owns_block(block));
    assert(slot_index(block, slot) < N);
    assert(slot->owns_handle(handle));

    if (!owns_block(block) || slot_index(block, slot) >= N || !slot->owns_handle(handle))
      return;

    if constexpr (quickjs::detail::napi_lifetime_tracked__<T, Owner>)
      quickjs::detail::napi_lifetime__<T>::record_release(owner_, slot->data());

    bool was_full = block->is_full();
    slot->destroy();
    block->release(slot);

    if (was_full)
      move_full_block_to_available(block);
  }

  void close()
  {
    for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it)
      it->close();
    blocks_.clear();
    available_blocks_.clear();
    full_blocks_.clear();
  }

  void reserve_prefix(size_t count)
  {
    (void)count;
  }

  size_t slot_count() const
  {
    return storage_slot_count();
  }

  size_t storage_slot_count() const
  {
    return blocks_.size() * N;
  }

  size_t active_count() const
  {
    size_t count = 0;
    for (const auto &block : blocks_)
      count += block.active_count;
    return count;
  }

  template <typename Fn>
  void for_each_active(Fn fn) const
  {
    for (const auto &block : blocks_)
    {
      for (const auto &slot : block.slots)
      {
        if (slot.active)
          fn(*slot.data());
      }
    }
  }

private:
  block__ *first_available_block()
  {
    while (!available_blocks_.empty())
    {
      block__ *block = available_blocks_.back();
      if (block != nullptr && block->is_available())
        return block;

      available_blocks_.pop_back();
      if (block != nullptr)
        block->listed_available = false;
    }

    blocks_.emplace_back(owner_);
    block__ *block = &blocks_.back();
    block->listed_available = true;
    available_blocks_.push_back(block);
    return block;
  }

  static size_t slot_index(const block__ *block, const slot__ *slot)
  {
    if (block == nullptr || slot == nullptr)
      return N;

    const slot__ *begin = block->slots.data();
    const slot__ *end = begin + N;
    if (slot < begin || slot >= end)
      return N;

    return static_cast<size_t>(slot - begin);
  }

  bool owns_block(const block__ *block) const
  {
    if (block == nullptr)
      return false;

    for (const auto &owned : blocks_)
    {
      if (&owned == block)
        return true;
    }
    return false;
  }

  void move_full_block_to_available(block__ *block)
  {
    if (block == nullptr || block->listed_available)
      return;

    if (block->listed_full)
    {
      for (auto it = full_blocks_.begin(); it != full_blocks_.end(); ++it)
      {
        if (*it == block)
        {
          full_blocks_.erase(it);
          break;
        }
      }
      block->listed_full = false;
    }

    block->listed_available = true;
    available_blocks_.push_back(block);
  }

  // Stable slot blocks owned by this allocator.
  std::list<block__> blocks_;

  // Cached block lists used to find reusable slots quickly.
  std::vector<block__ *> available_blocks_;
  std::vector<block__ *> full_blocks_;

  // Owner used by lifetime hooks to attribute slot churn.
  Owner *owner_ = nullptr;
};

#endif // NAPI_QUICKJS_ALLOCATOR_H_
