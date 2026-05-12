#ifndef NAPI_QUICKJS_ALLOCATOR_H_
#define NAPI_QUICKJS_ALLOCATOR_H_

#include "napi_lifetime_macros.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <list>
#include <utility>
#include <vector>

template <typename T>
concept napi_allocator_payload__ =
    std::default_initializable<T> &&
    requires(T value) {
      { value.release() } -> std::same_as<void>;
    };

template <typename T, typename... Args>
concept napi_allocator_initializable_payload__ =
    napi_allocator_payload__<T> &&
    requires(T value, Args &&...args) {
      { value.initialize(static_cast<Args &&>(args)...) } -> std::same_as<void>;
    };

template <napi_allocator_payload__ T, size_t N = 256>
class napi_allocator__
{
  static_assert(N > 0, "N must be greater than zero");

private:
  static constexpr size_t next_power_of_two__(size_t value)
  {
    size_t result = 1;
    while (result < value)
      result <<= 1;
    return result;
  }

  struct slot__
  {
    T data;
    slot__ *next_free = nullptr;
    bool active = false;

    slot__() = default;
    slot__(const slot__ &) = delete;
    slot__ &operator=(const slot__ &) = delete;
  };

  struct block_layout__
  {
    std::array<slot__, N> slots;
    slot__ *first_free = nullptr;
    size_t active_count = 0;
    bool listed_available = false;
    bool listed_full = false;
  };

  static constexpr size_t block_alignment__ =
      next_power_of_two__(sizeof(block_layout__));
  static_assert((block_alignment__ & (block_alignment__ - 1)) == 0,
                "block alignment must be a power of two");

  struct alignas(block_alignment__) block__ : block_layout__
  {
    block__()
    {
      reset_free_list();
    }

    block__(const block__ &) = delete;
    block__ &operator=(const block__ &) = delete;

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
          slot.data.release();
          slot.active = false;
        }
      }

      this->active_count = 0;
      this->listed_available = false;
      this->listed_full = false;
      reset_free_list();
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
  napi_allocator__() = default;
  napi_allocator__(const napi_allocator__ &) = delete;
  napi_allocator__ &operator=(const napi_allocator__ &) = delete;

  napi_allocator__(napi_allocator__ &&other) noexcept
  {
    *this = static_cast<napi_allocator__ &&>(other);
  }

  napi_allocator__ &operator=(napi_allocator__ &&other) noexcept
  {
    if (this == &other)
      return *this;

    close();
    blocks_ = static_cast<std::list<block__> &&>(other.blocks_);
    available_blocks_ = static_cast<std::vector<block__ *> &&>(other.available_blocks_);
    full_blocks_ = static_cast<std::vector<block__ *> &&>(other.full_blocks_);
    return *this;
  }

  ~napi_allocator__()
  {
    close();
  }

  template <typename... Args>
    requires napi_allocator_initializable_payload__<T, Args...>
  T *allocate(Args &&...args)
  {
    block__ *block = first_available_block();
    if (block == nullptr)
      return nullptr;

    slot__ *slot = block->allocate();
    if (slot == nullptr)
      return nullptr;

    slot->data.initialize(static_cast<Args &&>(args)...);

    if (block->is_full())
    {
      block->listed_available = false;
      available_blocks_.pop_back();
      block->listed_full = true;
      full_blocks_.push_back(block);
    }

    return &slot->data;
  }

  T *get(T *handle)
  {
    slot__ *slot = slot_from_handle(handle);
    return slot != nullptr && slot->active ? &slot->data : nullptr;
  }

  const T *get(T *handle) const
  {
    const slot__ *slot = slot_from_handle(handle);
    return slot != nullptr && slot->active ? &slot->data : nullptr;
  }

  void release(T *handle)
  {
    slot__ *slot = slot_from_handle(handle);
    if (slot == nullptr || !slot->active)
      return;

    block__ *block = block_from_slot(slot);
    bool was_full = block != nullptr && block->is_full();
    slot->data.release();
    if (block != nullptr)
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
          fn(slot.data);
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

    blocks_.emplace_back();
    block__ *block = &blocks_.back();
    block->listed_available = true;
    available_blocks_.push_back(block);
    return block;
  }

  slot__ *slot_from_handle(T *handle)
  {
    if (handle == nullptr)
      return nullptr;

    auto *slot = reinterpret_cast<slot__ *>(
        reinterpret_cast<char *>(handle) - offsetof(slot__, data));
    return owns_slot(slot, handle) ? slot : nullptr;
  }

  const slot__ *slot_from_handle(T *handle) const
  {
    if (handle == nullptr)
      return nullptr;

    auto *slot = reinterpret_cast<const slot__ *>(
        reinterpret_cast<const char *>(handle) - offsetof(slot__, data));
    return owns_slot(slot, handle) ? slot : nullptr;
  }

  block__ *block_from_slot(slot__ *slot)
  {
    if (slot == nullptr)
      return nullptr;

    auto *block = reinterpret_cast<block__ *>(
        reinterpret_cast<uintptr_t>(slot) & ~(static_cast<uintptr_t>(block_alignment__) - 1));
    return owns_block(block) && slot_index(block, slot) < N ? block : nullptr;
  }

  const block__ *block_from_slot(const slot__ *slot) const
  {
    if (slot == nullptr)
      return nullptr;

    auto *block = reinterpret_cast<const block__ *>(
        reinterpret_cast<uintptr_t>(slot) & ~(static_cast<uintptr_t>(block_alignment__) - 1));
    return owns_block(block) && slot_index(block, slot) < N ? block : nullptr;
  }

  bool owns_slot(const slot__ *slot, const T *handle) const
  {
    if (slot == nullptr || handle == nullptr || &slot->data != handle)
      return false;

    const block__ *block = block_from_slot(slot);
    return block != nullptr && slot->active;
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

  std::list<block__> blocks_;
  std::vector<block__ *> available_blocks_;
  std::vector<block__ *> full_blocks_;
};

#endif // NAPI_QUICKJS_ALLOCATOR_H_
