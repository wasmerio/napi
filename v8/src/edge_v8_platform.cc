#include "edge_v8_platform.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include <libplatform/libplatform.h>
#include <v8.h>

namespace {

constexpr size_t kWasmPageSize = size_t{64} * 1024;

// What a tracked page-allocator region holds.
enum class RegionKind : uint8_t {
  // Resizable ArrayBuffer / growable SharedArrayBuffer backing store, or a
  // wasm memory of a context without wasm accounting.
  kBuffer,
  // Wasm memory of a context with wasm accounting.
  kWasmMemory,
  // Wasm code space (compiled modules, import wrappers).
  kWasmCode,
};

std::atomic<uint64_t> wasm_memory_cap_denials{0};
std::atomic<uint64_t> wasm_codegen_denials{0};
std::atomic<uint64_t> wasm_code_limit_stops{0};
std::atomic<uint64_t> wasm_code_decommitted_bytes{0};
// Committed bytes in every tracked wasm code space, attributed or not.
std::atomic<uint64_t> wasm_code_committed{0};
std::atomic<uint64_t> wasm_process_code_budget{0};
// Metered wasm memories in the process: committed bytes and live count.
std::atomic<uint64_t> wasm_memory_committed{0};
std::atomic<uint64_t> wasm_memories{0};
std::atomic<bool> wasm_code_metering{false};

// Embedder-owned memory accountant for page-allocator commits. The context is
// a counted embedder reference, released exactly once when the last lane or
// tracked region holding this object drops it.
class PageAccountant {
 public:
  PageAccountant(void* context, bool (*charge)(void*, uint64_t),
                 void (*uncharge)(void*, uint64_t), void (*release)(void*))
      : context_(context), charge_(charge), uncharge_(uncharge),
        release_(release) {}
  ~PageAccountant() {
    if (release_ != nullptr) release_(context_);
  }
  // Leave the context reference with the caller.
  void Detach() { release_ = nullptr; }
  PageAccountant(const PageAccountant&) = delete;
  PageAccountant& operator=(const PageAccountant&) = delete;

  // Must deny without side effects: V8 turns a refused commit into a
  // RangeError (or a failed grow) and may retry after a GC.
  bool Charge(RegionKind kind, uint64_t bytes) {
    if (bytes == 0) return true;
    if (kind != RegionKind::kWasmMemory) return charge_(context_, bytes);
    if (!wasm_->charge_memory(context_, bytes)) return false;
    wasm_memory_committed.fetch_add(bytes, std::memory_order_relaxed);
    return true;
  }
  void Uncharge(RegionKind kind, uint64_t bytes) {
    if (bytes == 0) return;
    if (kind == RegionKind::kWasmMemory) {
      wasm_memory_committed.fetch_sub(bytes, std::memory_order_relaxed);
      wasm_->uncharge_memory(context_, bytes);
    } else {
      uncharge_(context_, bytes);
    }
  }

  // Published once, before the lane runs guest code; immutable afterwards.
  bool AttachWasm(const snapi_v8_wasm_accounting& accounting) {
    std::lock_guard<std::mutex> lock(wasm_mutex_);
    if (wasm_ != nullptr) return false;
    wasm_storage_ = accounting;
    wasm_ = &wasm_storage_;
    has_wasm_.store(true, std::memory_order_release);
    return true;
  }
  bool has_wasm() const { return has_wasm_.load(std::memory_order_acquire); }

  // Counts a new wasm memory reservation against the caps, or refuses it.
  bool AdmitWasmMemory(size_t length) {
    uint64_t count = wasm_memories_.load(std::memory_order_relaxed);
    do {
      if (count >= wasm_->max_memories) return false;
    } while (!wasm_memories_.compare_exchange_weak(count, count + 1,
                                                   std::memory_order_acq_rel));
    uint64_t reserved = wasm_reserved_.load(std::memory_order_relaxed);
    do {
      if (length > wasm_->max_reserved_bytes ||
          reserved > wasm_->max_reserved_bytes - length) {
        wasm_memories_.fetch_sub(1, std::memory_order_acq_rel);
        return false;
      }
    } while (!wasm_reserved_.compare_exchange_weak(reserved, reserved + length,
                                                   std::memory_order_acq_rel));
    wasm_memories.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  void ReleaseWasmReservation(size_t length, bool freed) {
    wasm_reserved_.fetch_sub(length, std::memory_order_acq_rel);
    if (freed) {
      wasm_memories_.fetch_sub(1, std::memory_order_acq_rel);
      wasm_memories.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  // Whether code committed while this context runs is charged to it.
  bool MetersCode() const {
    return has_wasm() && !lane_gone_.load(std::memory_order_acquire);
  }

  // Charges code V8 already committed. Returns whether the bytes were
  // charged; sets `*stop` to the reason when the context must now stop (once
  // per context; the caller reports it outside its locks).
  bool ChargeCode(uint64_t bytes, uint32_t* stop) {
    if (!wasm_->charge_code(context_, bytes)) {
      if (ClaimStop()) *stop = SNAPI_V8_WASM_CODE_LIMIT_MEMORY;
      return false;
    }
    const uint64_t committed =
        code_committed_.fetch_add(bytes, std::memory_order_acq_rel) + bytes;
    if (committed > wasm_->code_budget_bytes && ClaimStop()) {
      *stop = SNAPI_V8_WASM_CODE_LIMIT_BUDGET;
    }
    return true;
  }
  void UnchargeCode(uint64_t bytes) {
    code_committed_.fetch_sub(bytes, std::memory_order_acq_rel);
    wasm_->uncharge_code(context_, bytes);
  }
  // `on_current_isolate`: the commit ran on this context's JS thread, whose
  // isolate stops right away; the callback stops the others.
  void ReportCodeLimit(uint32_t reason, bool on_current_isolate) {
    wasm_code_limit_stops.fetch_add(1, std::memory_order_relaxed);
    if (on_current_isolate) {
      if (v8::Isolate* isolate = v8::Isolate::TryGetCurrent()) {
        isolate->TerminateExecution();
      }
    }
    const uint64_t limit = reason == SNAPI_V8_WASM_CODE_LIMIT_BUDGET
                               ? wasm_->code_budget_bytes
                               : 0;
    wasm_->on_code_limit(context_, reason,
                         code_committed_.load(std::memory_order_acquire), limit);
  }

  // New compilations are admitted while the context is under its budget and
  // has not been stopped for its code.
  bool AdmitsCodegen() const {
    return !code_stopped_.load(std::memory_order_acquire) &&
           code_committed_.load(std::memory_order_acquire) <
               wasm_->code_budget_bytes;
  }

  // The lane is gone: code committed from now on (a module outliving its
  // context) is no longer charged here.
  void MarkLaneGone() { lane_gone_.store(true, std::memory_order_release); }

  snapi_v8_wasm_usage Usage() const {
    return {wasm_memories_.load(std::memory_order_acquire),
            wasm_reserved_.load(std::memory_order_acquire),
            code_committed_.load(std::memory_order_acquire)};
  }

 private:
  bool ClaimStop() {
    return !code_stopped_.exchange(true, std::memory_order_acq_rel);
  }

  void* context_;
  bool (*charge_)(void*, uint64_t);
  void (*uncharge_)(void*, uint64_t);
  void (*release_)(void*);
  std::mutex wasm_mutex_;
  snapi_v8_wasm_accounting wasm_storage_{};
  // Set once under wasm_mutex_ and published by has_wasm_; read only after
  // has_wasm() returned true.
  const snapi_v8_wasm_accounting* wasm_ = nullptr;
  std::atomic<bool> has_wasm_{false};
  std::atomic<uint64_t> wasm_memories_{0};
  std::atomic<uint64_t> wasm_reserved_{0};
  std::atomic<uint64_t> code_committed_{0};
  std::atomic<bool> code_stopped_{false};
  std::atomic<bool> lane_gone_{false};
};

// Set once any lane carries an accountant: from then on this process meters
// page-backed buffers and refuses reservations it cannot attribute.
std::atomic<bool> g_page_accounting_managed{false};

// A lane belongs to one N-API context. Only its dedicated embedder-owned
// thread executes V8 background work. The bounded queue prevents a guest
// from accumulating unbounded host-side Task objects while the lane is busy.
class BackgroundLane {
 public:
  BackgroundLane(size_t max_queued_tasks,
                 void* scope_context, void* (*enter_scope)(void*),
                 bool (*leave_scope)(void*, void*),
                 void (*on_overload)(void*))
      : max_queued_tasks_(max_queued_tasks), scope_context_(scope_context), enter_scope_(enter_scope),
        leave_scope_(leave_scope), on_overload_(on_overload) {}

  bool Post(std::unique_ptr<v8::Task> task, double delay_seconds) {
    if (!task) return true;
    using Clock = std::chrono::steady_clock;
    const double bounded_delay = std::isfinite(delay_seconds)
        ? std::clamp(delay_seconds, 0.0, 86400.0) : 0.0;
    const auto delay = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(bounded_delay));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_) return false;
      if (queue_.size() < max_queued_tasks_) {
        Item item{Clock::now() + delay, std::move(task)};
        auto it = queue_.begin();
        while (it != queue_.end() && it->due <= item.due) ++it;
        queue_.insert(it, std::move(item));
        cv_.notify_one();
        return true;
      }
      // Stop admission before signaling the embedder. It may need to acquire
      // other V8 locks to terminate the isolates, so never call it under ours.
      overloaded_ = true;
      stopped_ = true;
      cv_.notify_all();
    }
    if (on_overload_ != nullptr) on_overload_(scope_context_);
    return false;
  }

  void Run() {
    v8::ThreadIsolatedAllocator::SetDefaultPermissionsForSignalHandler();
    std::unique_lock<std::mutex> lock(mutex_);
    if (running_) return;
    running_ = true;
    cv_.notify_all();
    while (!stopped_) {
      if (queue_.empty()) {
        cv_.wait(lock, [&] { return stopped_ || !queue_.empty(); });
        continue;
      }
      auto due = queue_.front().due;
      if (std::chrono::steady_clock::now() < due) {
        cv_.wait_until(lock, due);
        continue;
      }
      auto task = std::move(queue_.front().task);
      queue_.pop_front();
      lock.unlock();
      void* scope = enter_scope_ != nullptr ? enter_scope_(scope_context_) : nullptr;
      if (enter_scope_ != nullptr && scope == nullptr) {
        SignalOverload();
        lock.lock();
        break;
      }
      // The lane can be created before V8 allocates its Linux JIT pkey. New
      // pkeys default to access-disabled on an already-running thread.
      v8::ThreadIsolatedAllocator::SetDefaultPermissionsForSignalHandler();
      task->Run();
      if (leave_scope_ != nullptr && !leave_scope_(scope_context_, scope)) {
        SignalOverload();
        lock.lock();
        break;
      }
      lock.lock();
    }
    queue_.clear();
    running_ = false;
  }

  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    cv_.notify_all();
  }

  bool IsRunning() {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
  }

  bool IsOverloaded() {
    std::lock_guard<std::mutex> lock(mutex_);
    return overloaded_;
  }

  void SignalOverload() {
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!overloaded_) {
        overloaded_ = true;
        notify = true;
      }
      stopped_ = true;
      cv_.notify_all();
    }
    if (notify && on_overload_ != nullptr) on_overload_(scope_context_);
  }

  // First accountant wins; every environment sharing a lane shares a budget.
  bool SetPageAccountant(std::shared_ptr<PageAccountant> accountant) {
    std::lock_guard<std::mutex> lock(accountant_mutex_);
    if (page_accountant_ != nullptr) return false;
    page_accountant_ = std::move(accountant);
    return true;
  }

  std::shared_ptr<PageAccountant> page_accountant() {
    std::lock_guard<std::mutex> lock(accountant_mutex_);
    return page_accountant_;
  }

 private:
  using Clock = std::chrono::steady_clock;
  struct Item {
    Clock::time_point due;
    std::unique_ptr<v8::Task> task;
  };
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Item> queue_;
  bool running_ = false;
  bool stopped_ = false;
  bool overloaded_ = false;
  size_t max_queued_tasks_ = 0;
  void* scope_context_ = nullptr;
  void* (*enter_scope_)(void*) = nullptr;
  bool (*leave_scope_)(void*, void*) = nullptr;
  void (*on_overload_)(void*) = nullptr;
  // Separate from mutex_: page reservations must never wait on queue state.
  std::mutex accountant_mutex_;
  std::shared_ptr<PageAccountant> page_accountant_;
};

thread_local BackgroundLane* current_background_lane = nullptr;
thread_local int page_attribution_pause_depth = 0;
std::atomic<uint64_t> fallback_worker_posts{0};
std::atomic<uint64_t> unattributed_worker_posts{0};
std::atomic<uint64_t> page_charge_denials{0};
std::atomic<uint64_t> unattributed_page_reservations{0};

// Wraps the platform page allocator so commits of tenant-reachable buffers
// are charged to the reserving context's accountant before the kernel call.
//
// V8 allocates resizable ArrayBuffers, growable SharedArrayBuffers and
// WebAssembly memories by reserving an inaccessible region with page (4 KiB)
// or wasm-page (64 KiB) alignment and committing a prefix of it with
// SetPermissions; shrinking decommits a suffix. Those bytes bypass the
// ArrayBuffer::Allocator. Every other caller of this allocator either
// reserves with a much larger alignment (pointer and cppgc cages), reserves
// executable ranges, or runs during runtime or isolate setup, where page
// attribution is paused. Such regions pass through unmetered.
//
// Only committed bytes are charged: reservations are bounded by V8's maximum
// buffer sizes, not by the memory limit. A tracked region keeps its
// accountant alive, so the charge is returned on FreePages even after the
// reserving lane is gone.
//
// With wasm code metering enabled, wasm code spaces (page-aligned
// reservations made for JIT code outside runtime/isolate setup) are tracked
// too. V8 commits code with RecommitPages and returns it with DecommitPages,
// in disjoint page ranges, and aborts the process if either fails, so code
// commits are never refused: each committed range is charged after the fact
// to the context running when it was committed (an import-wrapper space is
// shared by every context) and returned to that same context.
class MeteringPageAllocator final : public v8::PageAllocator {
 public:
  explicit MeteringPageAllocator(v8::PageAllocator* inner) : inner_(inner) {}

  size_t AllocatePageSize() override { return inner_->AllocatePageSize(); }
  size_t CommitPageSize() override { return inner_->CommitPageSize(); }
  void SetRandomMmapSeed(int64_t seed) override { inner_->SetRandomMmapSeed(seed); }
  void* GetRandomMmapAddr() override { return inner_->GetRandomMmapAddr(); }

  void* AllocatePages(void* hint, size_t length, size_t alignment,
                      Permission access) override {
    std::shared_ptr<PageAccountant> accountant;
    RegionKind kind = RegionKind::kBuffer;
    bool track = false;
    if (page_attribution_pause_depth == 0) {
      if (IsBufferReservation(alignment, access)) {
        if (current_background_lane != nullptr) {
          accountant = current_background_lane->page_accountant();
        }
        if (accountant == nullptr) {
          unattributed_page_reservations.fetch_add(1, std::memory_order_relaxed);
          // Fail closed in a metered process. A standalone pool means this
          // process also runs unmetered environments without a lane.
          if (g_page_accounting_managed.load(std::memory_order_acquire) &&
              !StandalonePoolCreated()) {
            return nullptr;
          }
        } else {
          track = true;
          if (IsWasmMemoryReservation(alignment) && accountant->has_wasm()) {
            kind = RegionKind::kWasmMemory;
            if (!accountant->AdmitWasmMemory(length)) {
              wasm_memory_cap_denials.fetch_add(1, std::memory_order_relaxed);
              return nullptr;
            }
          }
        }
      } else if (IsCodeReservation(alignment, access) &&
                 wasm_code_metering.load(std::memory_order_acquire)) {
        // Never refused: a failed code reservation aborts the process.
        kind = RegionKind::kWasmCode;
        track = true;
        if (current_background_lane != nullptr) {
          accountant = current_background_lane->page_accountant();
        }
      }
    }
    void* result = inner_->AllocatePages(hint, length, alignment, access);
    if (result == nullptr) {
      if (kind == RegionKind::kWasmMemory) {
        accountant->ReleaseWasmReservation(length, true);
      }
      return nullptr;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(result);
    if (track) {
      if (!Track(base, length, kind, accountant)) {
        // Without bookkeeping a code space would commit unmetered; that is
        // still better than refusing it, which aborts.
        if (kind == RegionKind::kWasmCode) return result;
        if (kind == RegionKind::kWasmMemory) {
          accountant->ReleaseWasmReservation(length, true);
        }
        inner_->FreePages(result, length);
        return nullptr;
      }
    } else if (alignment >= kLargeAlignment ||
               (length >= kLargeLength && !IsBufferReservation(alignment, access))) {
      // Unattributed (standalone) buffers stay out of the table: they would
      // only crowd out the cages and code range.
      AddExcluded(base, length);
    }
    return result;
  }

  bool FreePages(void* address, size_t length) override {
    std::shared_ptr<Region> region;
    if (tracked_regions_.load(std::memory_order_acquire) != 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = regions_.find(reinterpret_cast<uintptr_t>(address));
      if (it != regions_.end()) {
        region = std::move(it->second);
        regions_.erase(it);
        tracked_regions_.fetch_sub(1, std::memory_order_release);
      }
    }
    if (region == nullptr) {
      // Before the kernel can hand the range to a tracked buffer.
      TrimExcluded(reinterpret_cast<uintptr_t>(address));
      return inner_->FreePages(address, length);
    }
    std::lock_guard<std::mutex> lock(region->mutex);
    const bool ok = inner_->FreePages(address, length);
    // A failed free is fatal in V8; the charge is returned either way.
    if (region->kind == RegionKind::kWasmCode) {
      UnchargeCode(*region, region->base, UINTPTR_MAX);
      return ok;
    }
    region->accountant->Uncharge(region->kind, region->committed);
    region->committed = 0;
    if (region->kind == RegionKind::kWasmMemory) {
      region->accountant->ReleaseWasmReservation(
          region->length.load(std::memory_order_relaxed), true);
    }
    return ok;
  }

  bool ReleasePages(void* address, size_t length, size_t new_length) override {
    std::shared_ptr<Region> region = Find(address);
    if (region == nullptr || region->base != reinterpret_cast<uintptr_t>(address)) {
      TrimExcluded(reinterpret_cast<uintptr_t>(address) + new_length);
      return inner_->ReleasePages(address, length, new_length);
    }
    std::lock_guard<std::mutex> lock(region->mutex);
    if (!inner_->ReleasePages(address, length, new_length)) return false;
    size_t old_length;
    {
      std::lock_guard<std::mutex> map_lock(mutex_);
      old_length = region->length.load(std::memory_order_relaxed);
      region->length.store(std::min(old_length, new_length),
                           std::memory_order_relaxed);
    }
    if (region->kind == RegionKind::kWasmCode) {
      UnchargeCode(*region, region->base + new_length, UINTPTR_MAX);
      return true;
    }
    if (region->kind == RegionKind::kWasmMemory && old_length > new_length) {
      region->accountant->ReleaseWasmReservation(old_length - new_length, false);
    }
    if (region->committed > new_length) {
      region->accountant->Uncharge(region->kind, region->committed - new_length);
      region->committed = new_length;
    }
    return true;
  }

  bool SetPermissions(void* address, size_t length, Permission access) override {
    std::shared_ptr<Region> region = MaybeFind(address);
    // Code spaces are made accessible as a whole when reserved (without
    // memory protection keys) and committed with RecommitPages.
    if (region == nullptr || region->kind == RegionKind::kWasmCode) {
      return inner_->SetPermissions(address, length, access);
    }
    return Update(*region, address, length, access, [&] {
      return inner_->SetPermissions(address, length, access);
    });
  }

  bool RecommitPages(void* address, size_t length, Permission access) override {
    std::shared_ptr<Region> region = MaybeFind(address);
    if (region == nullptr) return inner_->RecommitPages(address, length, access);
    if (region->kind == RegionKind::kWasmCode) {
      // Charged after the fact: refusing would abort the process.
      if (!inner_->RecommitPages(address, length, access)) return false;
      if (access != kNoAccess && access != kNoAccessWillJitLater) {
        CommitCode(region, reinterpret_cast<uintptr_t>(address), length);
      }
      return true;
    }
    return Update(*region, address, length, access, [&] {
      return inner_->RecommitPages(address, length, access);
    });
  }

  bool DecommitPages(void* address, size_t size) override {
    std::shared_ptr<Region> region = MaybeFind(address);
    if (region == nullptr) return inner_->DecommitPages(address, size);
    if (region->kind == RegionKind::kWasmCode) {
      std::lock_guard<std::mutex> lock(region->mutex);
      const bool ok = inner_->DecommitPages(address, size);
      // A failed decommit is fatal in V8; the charge is returned either way.
      const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
      wasm_code_decommitted_bytes.fetch_add(
          UnchargeCode(*region, begin, begin + size), std::memory_order_relaxed);
      return ok;
    }
    return Update(*region, address, size, kNoAccess,
                  [&] { return inner_->DecommitPages(address, size); });
  }

  // Discarded pages stay committed (accessible), so the charge stays.
  bool DiscardSystemPages(void* address, size_t size) override {
    return inner_->DiscardSystemPages(address, size);
  }
  bool SealPages(void* address, size_t length) override {
    return inner_->SealPages(address, length);
  }
  bool ReserveForSharedMemoryMapping(void* address, size_t size) override {
    return inner_->ReserveForSharedMemoryMapping(address, size);
  }
  std::unique_ptr<SharedMemory> AllocateSharedPages(
      size_t length, const void* original_address) override {
    return inner_->AllocateSharedPages(length, original_address);
  }
  bool CanAllocateSharedPages() override { return inner_->CanAllocateSharedPages(); }

  v8::PageAllocator* inner() { return inner_; }

  // Returns every code charge `accountant` still holds (code spaces shared
  // across contexts, such as import wrappers, outlive the context) and
  // leaves those ranges uncharged.
  void DetachCodeOwner(const PageAccountant* accountant) {
    std::vector<std::shared_ptr<Region>> code_regions;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& entry : regions_) {
        if (entry.second->kind == RegionKind::kWasmCode) {
          code_regions.push_back(entry.second);
        }
      }
    }
    for (auto& region : code_regions) {
      std::lock_guard<std::mutex> lock(region->mutex);
      ReleaseOwnerSpans(*region, accountant);
    }
  }

 private:
  // Buffer reservations use the OS page size or the 64 KiB wasm page size.
  static constexpr size_t kMaxBufferAlignment = kWasmPageSize;
  // V8's own cages are aligned to (at least) their 4 GiB size.
  static constexpr size_t kLargeAlignment = size_t{1} << 30;
  static constexpr size_t kLargeLength = size_t{64} << 20;
  static constexpr size_t kMaxExcluded = 16;

  // A committed range of a code space and the context it is charged to
  // (none if the commit had no metered context or its charge was refused).
  struct CodeSpan {
    uintptr_t end;
    std::shared_ptr<PageAccountant> owner;
  };
  using SpanMap = std::map<uintptr_t, CodeSpan>;

  struct Region {
    std::mutex mutex;  // serializes commits; GSABs grow from several threads
    uintptr_t base = 0;
    // Written under both locks (ReleasePages); read under either.
    std::atomic<size_t> length{0};
    RegionKind kind = RegionKind::kBuffer;
    size_t committed = 0;  // charged prefix (buffers), guarded by `mutex`
    // Buffers and wasm memories: the reserving context, charged for every
    // commit. Code spaces: unset (each span has its own owner).
    std::shared_ptr<PageAccountant> accountant;
    // Code spaces: the reserving context, charged for commits made while no
    // metered context is bound. Weak: it must not outlive its context.
    std::weak_ptr<PageAccountant> reserver;
    SpanMap spans;  // code spaces only, by start
  };

  bool Track(uintptr_t base, size_t length, RegionKind kind,
             const std::shared_ptr<PageAccountant>& accountant) {
    try {
      auto region = std::make_shared<Region>();
      region->base = base;
      region->length = length;
      region->kind = kind;
      if (kind == RegionKind::kWasmCode) {
        region->reserver = accountant;
      } else {
        region->accountant = accountant;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      if (!regions_.emplace(base, std::move(region)).second) return false;
      tracked_regions_.fetch_add(1, std::memory_order_release);
      return true;
    } catch (const std::bad_alloc&) {
      return false;
    }
  }

  bool IsBufferReservation(size_t alignment, Permission access) const {
    return access == kNoAccess &&
           alignment <= std::max(kMaxBufferAlignment, inner_->AllocatePageSize());
  }

  // Wasm memories are reserved with the wasm page size as alignment; other
  // page-backed buffers with the OS allocation page size. Where the two
  // coincide, every such buffer is treated as a wasm memory.
  bool IsWasmMemoryReservation(size_t alignment) const {
    return alignment == kWasmPageSize;
  }

  // WasmCodeManager reserves code spaces page-aligned for JIT. V8's own code
  // range has a larger alignment or is reserved during isolate setup, where
  // attribution is paused.
  bool IsCodeReservation(size_t alignment, Permission access) const {
    return access == kNoAccessWillJitLater &&
           alignment <= inner_->AllocatePageSize();
  }

  static bool StandalonePoolCreated();

  // The context a code commit on this thread is charged to: the bound lane's
  // if it meters wasm (`*bound` = true), else the context that reserved the
  // space.
  static std::shared_ptr<PageAccountant> CodeOwner(const Region& region,
                                                   bool* bound) {
    *bound = false;
    if (current_background_lane != nullptr) {
      std::shared_ptr<PageAccountant> accountant =
          current_background_lane->page_accountant();
      if (accountant != nullptr && accountant->MetersCode()) {
        *bound = true;
        return accountant;
      }
    }
    std::shared_ptr<PageAccountant> reserver = region.reserver.lock();
    if (reserver != nullptr && reserver->MetersCode()) return reserver;
    return nullptr;
  }

  // Records [begin, begin + length) as committed; only bytes not committed
  // yet are counted and charged.
  void CommitCode(const std::shared_ptr<Region>& region, uintptr_t begin,
                  size_t length) {
    bool bound = false;
    std::shared_ptr<PageAccountant> owner = CodeOwner(*region, &bound);
    uint32_t stop = 0;
    try {
      std::lock_guard<std::mutex> lock(region->mutex);
      const size_t region_length = region->length.load(std::memory_order_relaxed);
      if (begin < region->base || begin - region->base >= region_length) return;
      const uintptr_t end =
          begin + std::min(length, region_length - (begin - region->base));
      std::vector<std::pair<uintptr_t, uintptr_t>> fresh;
      auto it = region->spans.upper_bound(begin);
      uintptr_t cursor = begin;
      if (it != region->spans.begin()) {
        auto previous = std::prev(it);
        if (previous->second.end > cursor) cursor = previous->second.end;
      }
      while (cursor < end) {
        const uintptr_t next =
            it == region->spans.end() ? end : std::min(it->first, end);
        if (cursor < next) fresh.emplace_back(cursor, next);
        if (it == region->spans.end() || it->first >= end) break;
        cursor = std::max(cursor, it->second.end);
        ++it;
      }
      for (const auto& [from, to] : fresh) {
        // Allocate the bookkeeping before charging, so a charge is never
        // left without a span to return it from.
        SpanMap scratch;
        scratch.emplace(from, CodeSpan{to, nullptr});
        SpanMap::node_type node = scratch.extract(scratch.begin());
        const uint64_t bytes = to - from;
        wasm_code_committed.fetch_add(bytes, std::memory_order_acq_rel);
        if (owner != nullptr && owner->ChargeCode(bytes, &stop)) {
          node.mapped().owner = owner;
        }
        InsertSpan(*region, std::move(node));
      }
      // The owner's lane may have been deleted meanwhile; its teardown walk
      // either saw these spans or ran before them, so return them here.
      if (owner != nullptr && !owner->MetersCode()) {
        ReleaseOwnerSpans(*region, owner.get());
      }
    } catch (const std::bad_alloc&) {
      // Out of host memory for bookkeeping: the rest of this commit stays
      // unmetered, which is still better than aborting inside V8.
    }
    if (stop != 0) owner->ReportCodeLimit(stop, bound);
  }

  // Returns the charges `owner` holds in `region` and leaves those spans
  // committed but uncharged. Caller holds region.mutex.
  static void ReleaseOwnerSpans(Region& region, const PageAccountant* owner) {
    for (auto& span : region.spans) {
      if (span.second.owner.get() != owner) continue;
      span.second.owner->UnchargeCode(span.second.end - span.first);
      span.second.owner.reset();
    }
  }

  // Adds a span, merging it with adjacent spans of the same owner. Never
  // allocates.
  static void InsertSpan(Region& region, SpanMap::node_type node) {
    const uintptr_t from = node.key();
    uintptr_t to = node.mapped().end;
    const std::shared_ptr<PageAccountant>& owner = node.mapped().owner;
    auto next = region.spans.lower_bound(from);
    if (next != region.spans.end() && next->first == to &&
        next->second.owner == owner) {
      to = next->second.end;
      next = region.spans.erase(next);
    }
    if (next != region.spans.begin()) {
      auto previous = std::prev(next);
      if (previous->second.end == from && previous->second.owner == owner) {
        previous->second.end = to;
        return;
      }
    }
    node.mapped().end = to;
    region.spans.insert(next, std::move(node));
  }

  // Removes committed ranges inside [begin, end), returning each owner's
  // charge. Returns the number of committed bytes removed. Caller holds
  // region.mutex.
  static uint64_t UnchargeCode(Region& region, uintptr_t begin, uintptr_t end) {
    uint64_t removed = 0;
    auto it = region.spans.upper_bound(begin);
    if (it != region.spans.begin() && std::prev(it)->second.end > begin) --it;
    while (it != region.spans.end() && it->first < end) {
      const uintptr_t start = it->first;
      const uintptr_t span_end = it->second.end;
      const uintptr_t from = std::max(start, begin);
      const uintptr_t to = std::min(span_end, end);
      std::shared_ptr<PageAccountant> owner = it->second.owner;
      if (start < from && to < span_end) {
        // Splitting needs a node; without one the span keeps its charge.
        try {
          region.spans.emplace_hint(std::next(it), to, CodeSpan{span_end, owner});
        } catch (const std::bad_alloc&) {
          ++it;
          continue;
        }
        it->second.end = from;
      } else if (start < from) {
        it->second.end = from;
      } else if (to < span_end) {
        // Re-key the remainder in place; extract/insert does not allocate.
        auto node = region.spans.extract(it);
        node.key() = to;
        region.spans.insert(std::move(node));
      } else {
        region.spans.erase(it);
      }
      const uint64_t bytes = to - from;
      removed += bytes;
      wasm_code_committed.fetch_sub(bytes, std::memory_order_acq_rel);
      if (owner != nullptr) owner->UnchargeCode(bytes);
      it = region.spans.lower_bound(to);
    }
    return removed;
  }

  // Lock-free filter for the hot path: V8 heap and code pages live in a few
  // large process-wide reservations and never take the allocator lock.
  std::shared_ptr<Region> MaybeFind(void* address) {
    if (tracked_regions_.load(std::memory_order_acquire) == 0) return nullptr;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
    for (size_t i = 0; i < kMaxExcluded; ++i) {
      const uintptr_t begin = excluded_begin_[i].load(std::memory_order_acquire);
      if (begin != 0 && addr >= begin &&
          addr < excluded_end_[i].load(std::memory_order_acquire)) {
        return nullptr;
      }
    }
    return Find(address);
  }

  std::shared_ptr<Region> Find(void* address) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = regions_.upper_bound(addr);
    if (it == regions_.begin()) return nullptr;
    --it;
    if (addr - it->first >= it->second->length.load(std::memory_order_relaxed)) {
      return nullptr;
    }
    return it->second;
  }

  // V8 commits prefixes (grow passes the buffer start and the new total) and
  // decommits suffixes (ResizeInPlace shrink). Anything else is charged
  // conservatively: a commit charges up to its end, a decommit that does not
  // reach the committed end keeps its charge until the region is freed.
  template <typename Op>
  bool Update(Region& region, void* address, size_t length, Permission access,
              Op op) {
    std::lock_guard<std::mutex> lock(region.mutex);
    const size_t offset = reinterpret_cast<uintptr_t>(address) - region.base;
    const size_t region_length = region.length.load(std::memory_order_relaxed);
    if (offset >= region_length) return op();  // trimmed away meanwhile
    const size_t end = offset + std::min(length, region_length - offset);
    if (access == kNoAccess || access == kNoAccessWillJitLater) {
      if (!op()) return false;
      if (offset < region.committed && end >= region.committed) {
        region.accountant->Uncharge(region.kind, region.committed - offset);
        region.committed = offset;
      }
      return true;
    }
    const size_t delta = end > region.committed ? end - region.committed : 0;
    if (!region.accountant->Charge(region.kind, delta)) {
      page_charge_denials.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (!op()) {
      region.accountant->Uncharge(region.kind, delta);
      return false;
    }
    region.committed += delta;
    return true;
  }

  void AddExcluded(uintptr_t base, size_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < kMaxExcluded; ++i) {
      if (excluded_begin_[i].load(std::memory_order_relaxed) != 0) continue;
      // Publish the end before the begin; readers check begin first.
      excluded_end_[i].store(base + length, std::memory_order_release);
      excluded_begin_[i].store(base, std::memory_order_release);
      return;
    }
    // Full: such regions then take the slow path, which is still correct.
  }

  // An excluded range must never cover memory the kernel may reuse: every
  // entry containing `released_from` ends there (or is dropped). Called
  // before the range is unmapped.
  void TrimExcluded(uintptr_t released_from) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < kMaxExcluded; ++i) {
      const uintptr_t begin = excluded_begin_[i].load(std::memory_order_relaxed);
      const uintptr_t end = excluded_end_[i].load(std::memory_order_relaxed);
      if (begin == 0 || released_from < begin || released_from >= end) continue;
      if (released_from == begin) {
        excluded_begin_[i].store(0, std::memory_order_release);
        excluded_end_[i].store(0, std::memory_order_release);
      } else {
        excluded_end_[i].store(released_from, std::memory_order_release);
      }
    }
  }

  v8::PageAllocator* inner_;
  std::atomic<size_t> tracked_regions_{0};
  std::atomic<uintptr_t> excluded_begin_[kMaxExcluded] = {};
  std::atomic<uintptr_t> excluded_end_[kMaxExcluded] = {};
  std::mutex mutex_;
  std::map<uintptr_t, std::shared_ptr<Region>> regions_;
};

std::atomic<MeteringPageAllocator*> g_metering_page_allocator{nullptr};

struct WorkerWarmupState {
  std::mutex mutex;
  std::condition_variable cv;
  size_t pending = 0;
};

class WorkerWarmupTask final : public v8::Task {
 public:
  explicit WorkerWarmupTask(WorkerWarmupState* state) : state_(state) {}

  void Run() override {
    if (state_ == nullptr) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->pending > 0) {
      state_->pending -= 1;
    }
    if (state_->pending == 0) {
      state_->cv.notify_all();
    }
  }

 private:
  WorkerWarmupState* state_ = nullptr;
};

void WarmUpFallbackWorkerThreads(v8::Platform* fallback) {
  if (fallback == nullptr) return;

  const int worker_threads = fallback->NumberOfWorkerThreads();
  if (worker_threads <= 0) return;

  WorkerWarmupState state;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pending = static_cast<size_t>(worker_threads) + 1;
  }

  for (int i = 0; i < worker_threads; ++i) {
    fallback->PostTaskOnWorkerThread(
        v8::TaskPriority::kUserVisible,
        std::make_unique<WorkerWarmupTask>(&state));
  }
  fallback->PostDelayedTaskOnWorkerThread(
      v8::TaskPriority::kUserVisible,
      std::make_unique<WorkerWarmupTask>(&state),
      0);

  std::unique_lock<std::mutex> lock(state.mutex);
  while (state.pending != 0) {
    state.cv.wait(lock);
  }
}

struct ForegroundTaskRecord {
  std::shared_ptr<EdgeV8Platform::IsolateState> isolate_state;
  std::unique_ptr<v8::Task> task;
};

void RunForegroundTaskRecord(napi_env /*env*/, void* data);
void CleanupForegroundTaskRecord(napi_env /*env*/, void* data);

}  // namespace

extern "C" void* snapi_v8_lane_new(size_t max_queued_tasks,
                                     void* scope_context,
                                     void* (*enter_scope)(void*),
                                     bool (*leave_scope)(void*, void*),
                                     void (*on_overload)(void*)) {
  if (max_queued_tasks == 0) return nullptr;
  return new (std::nothrow)
      BackgroundLane(max_queued_tasks, scope_context, enter_scope, leave_scope, on_overload);
}

extern "C" void snapi_v8_lane_run(void* handle) {
  if (handle == nullptr) return;
  auto* lane = static_cast<BackgroundLane*>(handle);
  auto* previous = current_background_lane;
  current_background_lane = lane;
  lane->Run();
  current_background_lane = previous;
}

extern "C" void snapi_v8_lane_stop(void* handle) {
  if (handle != nullptr) static_cast<BackgroundLane*>(handle)->Stop();
}

extern "C" void snapi_v8_lane_delete(void* handle) {
  auto* lane = static_cast<BackgroundLane*>(handle);
  if (lane == nullptr) return;
  if (std::shared_ptr<PageAccountant> accountant = lane->page_accountant()) {
    accountant->MarkLaneGone();
    if (auto* allocator = g_metering_page_allocator.load(std::memory_order_acquire)) {
      allocator->DetachCodeOwner(accountant.get());
    }
  }
  delete lane;
}

extern "C" void* snapi_v8_lane_swap_current(void* handle) {
  auto* previous = current_background_lane;
  current_background_lane = static_cast<BackgroundLane*>(handle);
  return previous;
}

extern "C" void* snapi_v8_lane_current() {
  return current_background_lane;
}

extern "C" bool snapi_v8_lane_is_running(void* handle) {
  return handle != nullptr && static_cast<BackgroundLane*>(handle)->IsRunning();
}

extern "C" bool snapi_v8_lane_overloaded(void* handle) {
  return handle != nullptr && static_cast<BackgroundLane*>(handle)->IsOverloaded();
}

extern "C" bool snapi_v8_lane_set_page_accountant(
    void* handle, void* context, bool (*charge)(void*, uint64_t),
    void (*uncharge)(void*, uint64_t), void (*release)(void*)) {
  if (handle == nullptr || charge == nullptr || uncharge == nullptr) return false;
  std::shared_ptr<PageAccountant> accountant;
  try {
    accountant = std::make_shared<PageAccountant>(context, charge, uncharge, release);
  } catch (const std::bad_alloc&) {
    return false;  // the caller still owns `context`
  }
  if (!static_cast<BackgroundLane*>(handle)->SetPageAccountant(accountant)) {
    accountant->Detach();
    return false;
  }
  g_page_accounting_managed.store(true, std::memory_order_release);
  return true;
}

extern "C" bool snapi_v8_lane_set_wasm_accounting(
    void* handle, const snapi_v8_wasm_accounting* accounting) {
  if (handle == nullptr || accounting == nullptr ||
      accounting->size < sizeof(snapi_v8_wasm_accounting) ||
      accounting->charge_memory == nullptr ||
      accounting->uncharge_memory == nullptr ||
      accounting->charge_code == nullptr || accounting->uncharge_code == nullptr ||
      accounting->on_code_limit == nullptr) {
    return false;
  }
  std::shared_ptr<PageAccountant> accountant =
      static_cast<BackgroundLane*>(handle)->page_accountant();
  return accountant != nullptr && accountant->AttachWasm(*accounting);
}

extern "C" void snapi_v8_lane_wasm_usage(void* handle,
                                         snapi_v8_wasm_usage* out) {
  if (out == nullptr) return;
  *out = {};
  if (handle == nullptr) return;
  std::shared_ptr<PageAccountant> accountant =
      static_cast<BackgroundLane*>(handle)->page_accountant();
  if (accountant != nullptr && accountant->has_wasm()) *out = accountant->Usage();
}

extern "C" void snapi_v8_wasm_process_stats(snapi_v8_wasm_stats* out) {
  if (out == nullptr) return;
  out->code_committed_bytes = wasm_code_committed.load(std::memory_order_acquire);
  out->code_budget_bytes = wasm_process_code_budget.load(std::memory_order_acquire);
  out->memory_cap_denials = wasm_memory_cap_denials.load(std::memory_order_acquire);
  out->codegen_denials = wasm_codegen_denials.load(std::memory_order_acquire);
  out->code_limit_stops = wasm_code_limit_stops.load(std::memory_order_acquire);
  out->code_decommitted_bytes =
      wasm_code_decommitted_bytes.load(std::memory_order_acquire);
  out->memory_committed_bytes = wasm_memory_committed.load(std::memory_order_acquire);
  out->memories = wasm_memories.load(std::memory_order_acquire);
}

void EdgeV8EnableWasmCodeMetering(uint64_t process_code_budget_bytes) {
  wasm_process_code_budget.store(process_code_budget_bytes,
                                 std::memory_order_release);
  wasm_code_metering.store(true, std::memory_order_release);
}

bool EdgeV8WasmMeteringReady() {
  if (!wasm_code_metering.load(std::memory_order_acquire) ||
      current_background_lane == nullptr) {
    return false;
  }
  std::shared_ptr<PageAccountant> accountant =
      current_background_lane->page_accountant();
  return accountant != nullptr && accountant->has_wasm();
}

bool EdgeV8AdmitWasmCodegen() {
  if (current_background_lane == nullptr) return true;
  std::shared_ptr<PageAccountant> accountant =
      current_background_lane->page_accountant();
  if (accountant == nullptr || !accountant->has_wasm()) return true;
  // Compilations can overshoot by one module's code (bounded by the module
  // size cap); denying before V8 reaches its own process-wide code limit
  // keeps that limit, which aborts the process, out of reach.
  if (!accountant->AdmitsCodegen() ||
      wasm_code_committed.load(std::memory_order_acquire) >=
          wasm_process_code_budget.load(std::memory_order_acquire)) {
    wasm_codegen_denials.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

EdgeV8PageAttributionPause::EdgeV8PageAttributionPause() {
  ++page_attribution_pause_depth;
}

EdgeV8PageAttributionPause::~EdgeV8PageAttributionPause() {
  --page_attribution_pause_depth;
}

extern "C" uint64_t snapi_v8_page_charge_denials() {
  return page_charge_denials.load(std::memory_order_acquire);
}

extern "C" uint64_t snapi_v8_unattributed_page_reservations() {
  return unattributed_page_reservations.load(std::memory_order_acquire);
}

// Native-only probes (not guest imports) for classification tests and the
// hot-path micro-benchmark. `metered == false` bypasses the wrapper.
extern "C" void* snapi_v8_test_page_reserve(void* hint, size_t length,
                                            size_t alignment) {
  auto* allocator = g_metering_page_allocator.load(std::memory_order_acquire);
  if (allocator == nullptr) return nullptr;
  return allocator->AllocatePages(hint, length, alignment,
                                  v8::PageAllocator::kNoAccess);
}

extern "C" bool snapi_v8_test_page_set_permissions(void* address, size_t length,
                                                   int access, bool metered) {
  auto* allocator = g_metering_page_allocator.load(std::memory_order_acquire);
  if (allocator == nullptr) return false;
  v8::PageAllocator* target = metered ? allocator : allocator->inner();
  return target->SetPermissions(address, length,
                                static_cast<v8::PageAllocator::Permission>(access));
}

extern "C" void* snapi_v8_test_code_reserve(size_t length) {
  auto* allocator = g_metering_page_allocator.load(std::memory_order_acquire);
  if (allocator == nullptr) return nullptr;
  return allocator->AllocatePages(nullptr, length, allocator->AllocatePageSize(),
                                  v8::PageAllocator::kNoAccessWillJitLater);
}

// `commit` recommits read-write (the probe never executes the pages), else
// decommits.
extern "C" bool snapi_v8_test_code_commit(void* address, size_t length,
                                          bool commit) {
  auto* allocator = g_metering_page_allocator.load(std::memory_order_acquire);
  if (allocator == nullptr) return false;
  return commit ? allocator->RecommitPages(address, length,
                                           v8::PageAllocator::kReadWrite)
                : allocator->DecommitPages(address, length);
}

extern "C" bool snapi_v8_test_page_free(void* address, size_t length) {
  auto* allocator = g_metering_page_allocator.load(std::memory_order_acquire);
  return allocator != nullptr && allocator->FreePages(address, length);
}

extern "C" uint64_t snapi_v8_fallback_worker_posts() {
  return fallback_worker_posts.load(std::memory_order_acquire);
}

extern "C" uint64_t snapi_v8_unattributed_worker_posts() {
  return unattributed_worker_posts.load(std::memory_order_acquire);
}

// Native-only regression hook. It is not reachable through guest imports.
// Exercising the actual V8 Task queue catches accidental process-wide pool
// routing that a Rust-only state-machine test cannot detect.
extern "C" bool snapi_v8_lane_post_test_task(void* handle,
                                               void (*callback)(void*), void* data) {
  if (handle == nullptr || callback == nullptr) return false;
  class CallbackTask final : public v8::Task {
   public:
    CallbackTask(void (*callback)(void*), void* data)
        : callback_(callback), data_(data) {}
    void Run() override { callback_(data_); }
   private:
    void (*callback_)(void*);
    void* data_;
  };
  return static_cast<BackgroundLane*>(handle)->Post(
      std::make_unique<CallbackTask>(callback, data), 0.0);
}

struct EdgeV8Platform::FinishedCallback {
  void (*callback)(void*) = nullptr;
  void* data = nullptr;
};

struct EdgeV8Platform::IsolateState {
  IsolateState(EdgeV8Platform* platform_in,
               v8::Isolate* isolate_in,
               std::shared_ptr<ForegroundTaskRunner> runner_in,
               bool standalone_workers_in)
      : platform(platform_in),
        isolate(isolate_in),
        runner(std::move(runner_in)),
        standalone_workers(standalone_workers_in) {}

  EdgeV8Platform* platform = nullptr;
  v8::Isolate* isolate = nullptr;
  std::shared_ptr<ForegroundTaskRunner> runner;
  bool standalone_workers = false;
  std::mutex mutex;
  size_t pending_foreground_tasks = 0;
  bool shutdown_started = false;
  bool finished = false;
  std::vector<FinishedCallback> finished_callbacks;
};

namespace {

class CountedForegroundTask final : public v8::Task {
 public:
  CountedForegroundTask(std::shared_ptr<EdgeV8Platform::IsolateState> isolate_state,
                        std::unique_ptr<v8::Task> task)
      : isolate_state_(std::move(isolate_state)),
        task_(std::move(task)) {}

  ~CountedForegroundTask() override { Finish(); }

  void Run() override {
    if (task_) {
      task_->Run();
      task_.reset();
    }
    Finish();
  }

 private:
  void Finish() {
    if (finished_.exchange(true, std::memory_order_acq_rel)) return;
    std::shared_ptr<EdgeV8Platform::IsolateState> isolate_state =
        std::move(isolate_state_);
    task_.reset();
    if (isolate_state && isolate_state->platform != nullptr) {
      isolate_state->platform->CompletePendingForegroundTask(isolate_state);
    }
  }

  std::shared_ptr<EdgeV8Platform::IsolateState> isolate_state_;
  std::unique_ptr<v8::Task> task_;
  std::atomic<bool> finished_ {false};
};

void RunForegroundTaskRecord(napi_env /*env*/, void* data) {
  auto* record = static_cast<ForegroundTaskRecord*>(data);
  if (record != nullptr && record->task) {
    record->task->Run();
  }
}

void CleanupForegroundTaskRecord(napi_env /*env*/, void* data) {
  auto* record = static_cast<ForegroundTaskRecord*>(data);
  if (record == nullptr) {
    delete record;
    return;
  }
  record->task.reset();
  std::shared_ptr<EdgeV8Platform::IsolateState> isolate_state =
      std::move(record->isolate_state);
  if (isolate_state && isolate_state->platform != nullptr) {
    isolate_state->platform->CompletePendingForegroundTask(isolate_state);
  }
  delete record;
}

}  // namespace

class EdgeV8Platform::ForegroundTaskRunner final : public v8::TaskRunner {
 public:
  ForegroundTaskRunner(std::shared_ptr<IsolateState> isolate_state,
                       v8::Isolate* isolate,
                       v8::Platform* fallback)
      : isolate_state_(std::move(isolate_state)),
        isolate_(isolate),
        fallback_(fallback) {}

  bool IdleTasksEnabled() override { return false; }
  bool NonNestableTasksEnabled() const override { return true; }
  bool NonNestableDelayedTasksEnabled() const override { return true; }

 protected:
  void PostTaskImpl(std::unique_ptr<v8::Task> task,
                    const v8::SourceLocation& location) override {
    PostTaskCommon(std::move(task), 0, location);
  }

  void PostNonNestableTaskImpl(std::unique_ptr<v8::Task> task,
                               const v8::SourceLocation& location) override {
    PostTaskCommon(std::move(task), 0, location);
  }

  void PostDelayedTaskImpl(std::unique_ptr<v8::Task> task,
                           double delay_in_seconds,
                           const v8::SourceLocation& location) override {
    uint64_t delay_ms = 0;
    if (delay_in_seconds > 0) {
      delay_ms = static_cast<uint64_t>(std::llround(delay_in_seconds * 1000.0));
    }
    PostTaskCommon(std::move(task), delay_ms, location);
  }

  void PostNonNestableDelayedTaskImpl(
      std::unique_ptr<v8::Task> task,
      double delay_in_seconds,
      const v8::SourceLocation& location) override {
    PostDelayedTaskImpl(std::move(task), delay_in_seconds, location);
  }

  void PostIdleTaskImpl(std::unique_ptr<v8::IdleTask> /*task*/,
                        const v8::SourceLocation& /*location*/) override {}

 private:
  void PostTaskCommon(std::unique_ptr<v8::Task> task,
                      uint64_t delay_ms,
                      const v8::SourceLocation& location) {
    if (!task) return;
    if (shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    std::shared_ptr<IsolateState> isolate_state = isolate_state_.lock();

    unofficial_napi_enqueue_foreground_task_callback enqueue =
        target_enqueue_.load(std::memory_order_acquire);
    void* target = target_data_.load(std::memory_order_acquire);
    if (enqueue != nullptr && target != nullptr) {
      ForegroundTaskRecord* record = new (std::nothrow) ForegroundTaskRecord();
      if (record != nullptr) {
        // The embedder may run and clean up the task before enqueue returns.
        // Publish all record state and charge pending work first.
        if (isolate_state != nullptr && isolate_state->platform != nullptr) {
          isolate_state->platform->AddPendingForegroundTask(isolate_state);
          record->isolate_state = isolate_state;
        }
        record->task = std::move(task);
        if (enqueue(target,
                    RunForegroundTaskRecord,
                    record,
                    CleanupForegroundTaskRecord,
                    delay_ms) == napi_ok) {
          return;
        }
        task = std::move(record->task);
        std::shared_ptr<IsolateState> pending_state =
            std::move(record->isolate_state);
        delete record;
        if (pending_state != nullptr && pending_state->platform != nullptr) {
          pending_state->platform->CompletePendingForegroundTask(pending_state);
        }
      }
    }

    auto runner = fallback_ != nullptr ? fallback_->GetForegroundTaskRunner(isolate_) : nullptr;
    if (runner) {
      used_fallback_runner_.store(true, std::memory_order_release);
      std::unique_ptr<v8::Task> counted_task = std::move(task);
      if (isolate_state != nullptr && isolate_state->platform != nullptr) {
        isolate_state->platform->AddPendingForegroundTask(isolate_state);
        counted_task =
            std::make_unique<CountedForegroundTask>(isolate_state, std::move(counted_task));
      }
      if (delay_ms == 0) {
        runner->PostTask(std::move(counted_task), location);
      } else {
        runner->PostDelayedTask(std::move(counted_task), delay_ms / 1000.0, location);
      }
    }
  }

  std::weak_ptr<IsolateState> isolate_state_;
  v8::Isolate* isolate_ = nullptr;
  v8::Platform* fallback_ = nullptr;
  std::atomic<napi_env> target_env_ {nullptr};
  std::atomic<unofficial_napi_enqueue_foreground_task_callback> target_enqueue_ {nullptr};
  std::atomic<void*> target_data_ {nullptr};
  std::atomic<bool> shutting_down_ {false};
  std::atomic<bool> used_fallback_runner_ {false};

 public:
  ~ForegroundTaskRunner() override = default;

  void BindTarget(napi_env env,
                  unofficial_napi_enqueue_foreground_task_callback callback,
                  void* target) {
    target_data_.store(target, std::memory_order_release);
    target_enqueue_.store(callback, std::memory_order_release);
    target_env_.store(env, std::memory_order_release);
  }

  void ClearTarget(napi_env env) {
    if (target_env_.load(std::memory_order_acquire) != env) return;
    target_env_.store(nullptr, std::memory_order_release);
    target_enqueue_.store(nullptr, std::memory_order_release);
    target_data_.store(nullptr, std::memory_order_release);
  }

  void NotifyIsolateShutdown() {
    shutting_down_.store(true, std::memory_order_release);
    target_env_.store(nullptr, std::memory_order_release);
    target_enqueue_.store(nullptr, std::memory_order_release);
    target_data_.store(nullptr, std::memory_order_release);
  }

  bool used_fallback_runner() const {
    return used_fallback_runner_.load(std::memory_order_acquire);
  }
};

namespace {
// Worker threads for the shared platform: 0 means "let V8 decide". Read once,
// when the platform is built.
std::atomic<int> g_worker_thread_count{0};
std::atomic<bool> g_platform_created{false};
std::atomic<bool> g_standalone_pool_created{false};
}  // namespace

extern "C" bool snapi_v8_platform_created() {
  return g_platform_created.load(std::memory_order_acquire);
}

extern "C" bool snapi_v8_standalone_pool_created() {
  return g_standalone_pool_created.load(std::memory_order_acquire);
}

namespace {
bool MeteringPageAllocator::StandalonePoolCreated() {
  return g_standalone_pool_created.load(std::memory_order_acquire);
}
}  // namespace

bool EdgeV8Platform::SetWorkerThreadCount(int count) {
  const int requested = count < 0 ? 0 : count;
  if (g_platform_created.load(std::memory_order_acquire)) {
    // The pool is already built. Asking for what is already in place is not an
    // error -- several embedder components may configure the runtime from the
    // same setting -- but asking for anything else cannot be honoured.
    return g_worker_thread_count.load(std::memory_order_acquire) == requested;
  }
  g_worker_thread_count.store(requested, std::memory_order_release);
  return true;
}

std::unique_ptr<EdgeV8Platform> EdgeV8Platform::Create(bool standalone_workers) {
  g_platform_created.store(true, std::memory_order_release);
  // This delegate supplies foreground runners, clocks, and allocators. V8
  // sees EdgeV8Platform (not this delegate), which implements every worker/job
  // entry point. The delegate's documented --single-threaded requirement is
  // for passing it directly to V8; here the outer platform supplies workers.
  // Its own pool must stay absent in a managed Edge process: those tasks
  // belong to the instance lane and its CPU/memory accounting.
  std::unique_ptr<v8::Platform> fallback =
      v8::platform::NewSingleThreadedDefaultPlatform();
  if (!fallback) return nullptr;
  auto platform = std::unique_ptr<EdgeV8Platform>(new EdgeV8Platform(std::move(fallback)));
  if (standalone_workers && !platform->EnableStandaloneWorkers()) return nullptr;
  return platform;
}

EdgeV8Platform::EdgeV8Platform(std::unique_ptr<v8::Platform> fallback)
    : fallback_(std::move(fallback)) {
  v8::PageAllocator* inner =
      fallback_ != nullptr ? fallback_->GetPageAllocator() : nullptr;
  if (inner != nullptr) {
    auto allocator = std::make_unique<MeteringPageAllocator>(inner);
    // V8 caches the platform page allocator for the process lifetime, and
    // this platform is never destroyed while V8 is initialized.
    g_metering_page_allocator.store(allocator.get(), std::memory_order_release);
    page_allocator_ = std::move(allocator);
  }
}

EdgeV8Platform::~EdgeV8Platform() = default;

bool EdgeV8Platform::EnableStandaloneWorkers() {
  std::lock_guard<std::mutex> lock(standalone_workers_mutex_);
  if (standalone_workers_) return true;
  auto workers = v8::platform::NewDefaultPlatform(
      g_worker_thread_count.load(std::memory_order_acquire));
  if (!workers) return false;
  WarmUpFallbackWorkerThreads(workers.get());
  standalone_workers_ = std::move(workers);
  standalone_workers_ptr_.store(standalone_workers_.get(),
                                std::memory_order_release);
  g_standalone_pool_created.store(true, std::memory_order_release);
  return true;
}

v8::Platform* EdgeV8Platform::StandaloneWorkers() {
  return standalone_workers_ptr_.load(std::memory_order_acquire);
}

std::shared_ptr<EdgeV8Platform::IsolateState> EdgeV8Platform::GetState(v8::Isolate* isolate) {
  if (isolate == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = isolates_.find(isolate);
  return it != isolates_.end() ? it->second : nullptr;
}

std::shared_ptr<EdgeV8Platform::IsolateState> EdgeV8Platform::EnsureState(v8::Isolate* isolate) {
  if (isolate == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = isolates_.find(isolate);
  if (it != isolates_.end()) return it->second;
  if (fallback_ != nullptr) {
    // libplatform's NotifyIsolateShutdown() assumes a foreground runner entry
    // exists for every isolate it tears down.
    (void)fallback_->GetForegroundTaskRunner(isolate);
  }
  const bool standalone_workers = current_background_lane == nullptr &&
                                  StandaloneWorkers() != nullptr;
  if (standalone_workers) {
    // libplatform's shutdown helper expects an isolate runner to exist.
    (void)StandaloneWorkers()->GetForegroundTaskRunner(isolate);
  }
  auto state = std::make_shared<IsolateState>(this, isolate, nullptr,
                                              standalone_workers);
  state->runner = std::make_shared<ForegroundTaskRunner>(state, isolate, fallback_.get());
  isolates_.emplace(isolate, state);
  return state;
}

std::shared_ptr<EdgeV8Platform::ForegroundTaskRunner> EdgeV8Platform::EnsureRunner(v8::Isolate* isolate) {
  std::shared_ptr<IsolateState> state = EnsureState(isolate);
  return state != nullptr ? state->runner : nullptr;
}

bool EdgeV8Platform::RegisterIsolate(v8::Isolate* isolate) { return EnsureRunner(isolate) != nullptr; }

void EdgeV8Platform::AddPendingForegroundTask(const std::shared_ptr<IsolateState>& state) {
  if (!state) return;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->finished) return;
  state->pending_foreground_tasks += 1;
}

void EdgeV8Platform::CompletePendingForegroundTask(const std::shared_ptr<IsolateState>& state) {
  if (!state) return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->pending_foreground_tasks > 0) {
      state->pending_foreground_tasks -= 1;
    }
  }
  MaybeFinishIsolate(state, false);
}

void EdgeV8Platform::BeginShutdown(const std::shared_ptr<IsolateState>& state) {
  if (!state) return;
  std::lock_guard<std::mutex> lock(state->mutex);
  state->shutdown_started = true;
}

void EdgeV8Platform::MaybeFinishIsolate(const std::shared_ptr<IsolateState>& state,
                                        bool begin_shutdown) {
  if (!state) return;

  std::vector<FinishedCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (begin_shutdown) {
      state->shutdown_started = true;
    }
    if (state->finished ||
        !state->shutdown_started ||
        state->pending_foreground_tasks != 0) {
      return;
    }
    state->finished = true;
    callbacks.swap(state->finished_callbacks);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolates_.find(state->isolate);
    if (it != isolates_.end() && it->second.get() == state.get()) {
      isolates_.erase(it);
    }
  }

  for (const FinishedCallback& callback : callbacks) {
    if (callback.callback != nullptr) {
      callback.callback(callback.data);
    }
  }
}

void EdgeV8Platform::AddIsolateFinishedCallback(v8::Isolate* isolate,
                                                void (*callback)(void*),
                                                void* data) {
  if (callback == nullptr) return;
  std::shared_ptr<IsolateState> state;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolates_.find(isolate);
    if (it != isolates_.end()) {
      state = it->second;
    }
  }
  if (!state) {
    callback(data);
    return;
  }

  bool run_now = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->finished) {
      run_now = true;
    } else {
      state->finished_callbacks.push_back(FinishedCallback{callback, data});
      run_now = state->shutdown_started && state->pending_foreground_tasks == 0;
    }
  }

  if (run_now) {
    MaybeFinishIsolate(state, false);
  }
}

void EdgeV8Platform::NotifyIsolateShutdown(v8::Isolate* isolate) {
  if (isolate == nullptr) return;

  std::shared_ptr<IsolateState> state = GetState(isolate);
  std::shared_ptr<ForegroundTaskRunner> runner = state != nullptr ? state->runner : nullptr;
  if (runner) {
    runner->NotifyIsolateShutdown();
  }
  // The foreground delegate owns the fallback runner used before guest
  // callbacks are bound. A standalone isolate also needs its worker pool's
  // shutdown notification; managed background tasks belong to its lane.
  if (fallback_ != nullptr) {
    v8::platform::NotifyIsolateShutdown(fallback_.get(), isolate);
  }
  if (state != nullptr && state->standalone_workers) {
    auto* workers = StandaloneWorkers();
    v8::platform::NotifyIsolateShutdown(workers, isolate);
  }
  BeginShutdown(state);
  MaybeFinishIsolate(state, false);
}

void EdgeV8Platform::DisposeIsolate(v8::Isolate* isolate) {
  if (isolate == nullptr) return;

  std::shared_ptr<IsolateState> state = GetState(isolate);
  NotifyIsolateShutdown(isolate);

  // Keep the isolate registered while it is being disposed because V8 may
  // still post tasks during teardown, then drop the map entry before the
  // address can be reused for a new isolate.
  isolate->Dispose();
  UnregisterIsolate(isolate);
  MaybeFinishIsolate(state, false);
}

void EdgeV8Platform::UnregisterIsolate(v8::Isolate* isolate) {
  if (isolate == nullptr) return;
  std::lock_guard<std::mutex> lock(mutex_);
  isolates_.erase(isolate);
}

bool EdgeV8Platform::BindForegroundTaskTarget(
    v8::Isolate* isolate,
    napi_env env,
    unofficial_napi_enqueue_foreground_task_callback callback,
    void* target) {
  std::shared_ptr<ForegroundTaskRunner> runner = EnsureRunner(isolate);
  if (!runner) return false;
  runner->BindTarget(env, callback, target);
  return true;
}

void EdgeV8Platform::ClearForegroundTaskTarget(v8::Isolate* isolate, napi_env env) {
  std::shared_ptr<IsolateState> state;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = isolates_.find(isolate);
    if (it == isolates_.end()) return;
    state = it->second;
  }
  if (!state || !state->runner) return;
  state->runner->ClearTarget(env);
}

void EdgeV8Platform::PumpPendingForegroundTasks(v8::Isolate* isolate) {
  if (fallback_ == nullptr || isolate == nullptr) return;
  // ForegroundTaskRunner::PostTaskCommon forwards to the guest's bound
  // enqueue callback when one is set, but falls back to fallback_'s own
  // GetForegroundTaskRunner() when no guest target is bound (e.g. the guest
  // drives everything through the provider event-loop checkpoint and never
  // calls BindForegroundTaskTarget). Nothing else pumps that fallback
  // runner's queue, so tasks routed there -- including V8-internal work like
  // Heap::PostFinalizationRegistryCleanupTaskIfNeeded's cleanup task -- would
  // otherwise be posted and never run. Drain it explicitly.
  while (v8::platform::PumpMessageLoop(fallback_.get(), isolate)) {
  }
}

int EdgeV8Platform::NumberOfWorkerThreads() {
  if (current_background_lane != nullptr) return 1;
  auto* workers = StandaloneWorkers();
  return workers != nullptr ? workers->NumberOfWorkerThreads() : 0;
}

std::shared_ptr<v8::TaskRunner> EdgeV8Platform::GetForegroundTaskRunner(
    v8::Isolate* isolate,
    v8::TaskPriority /*priority*/) {
  return EnsureRunner(isolate);
}

bool EdgeV8Platform::IdleTasksEnabled(v8::Isolate* isolate) {
  (void)isolate;
  return false;
}

double EdgeV8Platform::MonotonicallyIncreasingTime() {
  if (fallback_ != nullptr) return fallback_->MonotonicallyIncreasingTime();
  using clock = std::chrono::steady_clock;
  const auto now = clock::now().time_since_epoch();
  return std::chrono::duration<double>(now).count();
}

double EdgeV8Platform::CurrentClockTimeMillis() {
  return fallback_ != nullptr ? fallback_->CurrentClockTimeMillis()
                              : v8::Platform::SystemClockTimeMillis();
}

v8::TracingController* EdgeV8Platform::GetTracingController() {
  return fallback_ != nullptr ? fallback_->GetTracingController() : nullptr;
}

v8::PageAllocator* EdgeV8Platform::GetPageAllocator() {
  return page_allocator_.get();
}

v8::ThreadIsolatedAllocator* EdgeV8Platform::GetThreadIsolatedAllocator() {
  return fallback_ != nullptr ? fallback_->GetThreadIsolatedAllocator() : nullptr;
}

void EdgeV8Platform::OnCriticalMemoryPressure() {
  if (fallback_ != nullptr) fallback_->OnCriticalMemoryPressure();
}

void EdgeV8Platform::DumpWithoutCrashing() {
  if (fallback_ != nullptr) fallback_->DumpWithoutCrashing();
}

v8::HighAllocationThroughputObserver* EdgeV8Platform::GetHighAllocationThroughputObserver() {
  if (fallback_ != nullptr) return fallback_->GetHighAllocationThroughputObserver();
  static v8::HighAllocationThroughputObserver observer;
  return &observer;
}

v8::Platform::StackTracePrinter EdgeV8Platform::GetStackTracePrinter() {
  return fallback_ != nullptr ? fallback_->GetStackTracePrinter() : nullptr;
}

std::unique_ptr<v8::ScopedBlockingCall> EdgeV8Platform::CreateBlockingScope(
    v8::BlockingType blocking_type) {
  return fallback_ != nullptr ? fallback_->CreateBlockingScope(blocking_type) : nullptr;
}

std::unique_ptr<v8::JobHandle> EdgeV8Platform::CreateJobImpl(
    v8::TaskPriority priority,
    std::unique_ptr<v8::JobTask> job_task,
    const v8::SourceLocation& location) {
  (void)location;
  return v8::platform::NewDefaultJobHandle(this, priority, std::move(job_task),
                                           static_cast<size_t>(std::max(1, NumberOfWorkerThreads())));
}

void EdgeV8Platform::PostTaskOnWorkerThreadImpl(v8::TaskPriority priority,
                                               std::unique_ptr<v8::Task> task,
                                               const v8::SourceLocation& location) {
  if (current_background_lane != nullptr) {
    current_background_lane->Post(std::move(task), 0.0);
    return;
  }
  if (auto* workers = StandaloneWorkers()) {
    fallback_worker_posts.fetch_add(1, std::memory_order_relaxed);
    workers->PostTaskOnWorkerThread(priority, std::move(task), location);
  } else {
    // A managed worker task without a bound lane has no safe tenant to bill.
    // Discard it rather than starting an unmetered global worker.
    unattributed_worker_posts.fetch_add(1, std::memory_order_relaxed);
  }
}

void EdgeV8Platform::PostDelayedTaskOnWorkerThreadImpl(
    v8::TaskPriority priority,
    std::unique_ptr<v8::Task> task,
    double delay_in_seconds,
    const v8::SourceLocation& location) {
  if (current_background_lane != nullptr) {
    current_background_lane->Post(std::move(task), delay_in_seconds);
    return;
  }
  if (auto* workers = StandaloneWorkers()) {
    fallback_worker_posts.fetch_add(1, std::memory_order_relaxed);
    workers->PostDelayedTaskOnWorkerThread(priority, std::move(task), delay_in_seconds, location);
  } else {
    unattributed_worker_posts.fetch_add(1, std::memory_order_relaxed);
  }
}
