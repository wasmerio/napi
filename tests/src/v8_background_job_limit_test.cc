#include "../../v8/src/edge_v8_platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <v8-platform.h>

namespace {

// Every wait is bounded so a regression fails the test instead of hanging it.
constexpr auto kWaitTimeout = std::chrono::seconds(10);
// A job worker gives up on its own after this, so the job always finishes.
constexpr auto kJobTimeout = std::chrono::seconds(30);
// How long a job that was created but not started must stay idle.
constexpr auto kNotStartedWait = std::chrono::milliseconds(200);

struct Probe {
  std::mutex mutex;
  std::condition_variable cv;
  size_t running = 0;
  size_t peak = 0;
  bool other_task_ran = false;
  bool timed_out = false;
  // Read by GetMaxConcurrency(), which libplatform calls under its own lock.
  std::atomic<bool> released{false};
};

// A job that asks for more workers than the pool has, and keeps every worker
// it gets until the test releases it.
class BlockingJob final : public v8::JobTask {
 public:
  BlockingJob(std::shared_ptr<Probe> probe, size_t wanted)
      : probe_(std::move(probe)), wanted_(wanted) {}

  void Run(v8::JobDelegate* /*delegate*/) override {
    std::unique_lock<std::mutex> lock(probe_->mutex);
    ++probe_->running;
    probe_->peak = std::max(probe_->peak, probe_->running);
    probe_->cv.notify_all();
    if (!probe_->cv.wait_for(lock, kJobTimeout, [this] {
          return probe_->released.load(std::memory_order_acquire);
        })) {
      probe_->timed_out = true;
      probe_->released.store(true, std::memory_order_release);
      probe_->cv.notify_all();
    }
    --probe_->running;
  }

  size_t GetMaxConcurrency(size_t /*worker_count*/) const override {
    return probe_->released.load(std::memory_order_acquire) ? 0 : wanted_;
  }

 private:
  std::shared_ptr<Probe> probe_;
  size_t wanted_;
};

class OtherWorkerTask final : public v8::Task {
 public:
  explicit OtherWorkerTask(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {}

  void Run() override {
    std::lock_guard<std::mutex> lock(probe_->mutex);
    probe_->other_task_ran = true;
    probe_->cv.notify_all();
  }

 private:
  std::shared_ptr<Probe> probe_;
};

void Release(const std::shared_ptr<Probe>& probe) {
  std::lock_guard<std::mutex> lock(probe->mutex);
  probe->released.store(true, std::memory_order_release);
  probe->cv.notify_all();
}

bool WaitForRunning(const std::shared_ptr<Probe>& probe, size_t count) {
  std::unique_lock<std::mutex> lock(probe->mutex);
  return probe->cv.wait_for(lock, kWaitTimeout, [&] { return probe->running >= count; });
}

// One case: a job that wants more workers than the pool has, the number of
// workers it should hold at once, and whether a worker task posted while it
// holds them should still get a worker.
class JobCase {
 public:
  JobCase(EdgeV8Platform* platform, int workers, const char* name)
      : platform_(platform),
        workers_(workers),
        name_(name),
        probe_(std::make_shared<Probe>()) {}

  std::unique_ptr<v8::JobTask> NewJob() const {
    return std::make_unique<BlockingJob>(probe_, static_cast<size_t>(workers_) + 4);
  }

  // Waits until the job holds `count` workers.
  void ExpectRunning(size_t count) {
    if (!WaitForRunning(probe_, count)) {
      Fail("%zu running, expected %zu", Running(), count);
    }
  }

  // Checks that no worker picks up the job for a while.
  void ExpectNotStarted() {
    std::unique_lock<std::mutex> lock(probe_->mutex);
    if (probe_->cv.wait_for(lock, kNotStartedWait, [&] { return probe_->running > 0; })) {
      Fail("%zu running before the job was started, expected 0", probe_->running);
    }
  }

  // Posts a worker task and checks that it runs while the job holds its
  // workers.
  void ExpectOtherTaskToRun() {
    platform_->PostTaskOnWorkerThread(v8::TaskPriority::kUserVisible,
                                      std::make_unique<OtherWorkerTask>(probe_));
    other_task_posted_ = true;
    std::unique_lock<std::mutex> lock(probe_->mutex);
    if (!probe_->cv.wait_for(lock, kWaitTimeout, [&] { return probe_->other_task_ran; })) {
      Fail("a worker task posted while it ran did not run (%zu job workers running)",
           probe_->running);
    }
  }

  // Releases the job, joins it, and checks how many workers it held at most.
  bool Finish(std::unique_ptr<v8::JobHandle> handle, size_t expected_peak) {
    Release(probe_);
    handle->Join();
    return Check(expected_peak);
  }

  // Joins the job while it runs, releasing it once `expected_peak` threads,
  // the joining thread included, run it at once.
  bool JoinWhileRunning(std::unique_ptr<v8::JobHandle> handle, size_t expected_peak) {
    std::shared_ptr<Probe> probe = probe_;
    std::thread releaser([probe, expected_peak] {
      WaitForRunning(probe, expected_peak);
      Release(probe);
    });
    handle->Join();
    releaser.join();
    return Check(expected_peak);
  }

 private:
  size_t Running() {
    std::lock_guard<std::mutex> lock(probe_->mutex);
    return probe_->running;
  }

  bool Check(size_t expected_peak) {
    std::unique_lock<std::mutex> lock(probe_->mutex);
    if (other_task_posted_ &&
        !probe_->cv.wait_for(lock, kWaitTimeout, [&] { return probe_->other_task_ran; })) {
      Fail("the other worker task did not run after the job finished");
    }
    if (probe_->peak != expected_peak) {
      Fail("at most %zu ran at once, expected %zu", probe_->peak, expected_peak);
    }
    if (probe_->timed_out) Fail("a job worker timed out");
    return passed_;
  }

#if defined(__GNUC__)
  __attribute__((format(printf, 2, 3)))
#endif
  void Fail(const char* format, ...) {
    std::fprintf(stderr, "%s, %d workers: ", name_, workers_);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    passed_ = false;
  }

  EdgeV8Platform* platform_;
  int workers_;
  const char* name_;
  std::shared_ptr<Probe> probe_;
  bool other_task_posted_ = false;
  bool passed_ = true;
};

}  // namespace

// Usage: napi_v8_background_job_limit_test [worker-count]
//
// Own the platform in a separate process: the worker pool is process-wide and
// its size is fixed when the platform is built.
int main(int argc, char** argv) {
  const int workers = argc > 1 ? std::atoi(argv[1]) : 4;
  if (workers < 1) {
    std::fprintf(stderr, "worker count must be at least 1\n");
    return 1;
  }
  EdgeV8Platform::SetWorkerThreadCount(workers);
  auto platform = EdgeV8Platform::Create();
  if (!platform) return 1;
  if (platform->NumberOfWorkerThreads() != workers) {
    std::fprintf(stderr, "platform has %d workers, asked for %d\n",
                 platform->NumberOfWorkerThreads(), workers);
    return 1;
  }

  const size_t all = static_cast<size_t>(workers);
  // A job nobody waits for leaves one worker for other work. With a single
  // worker it still gets that worker: a job limited to zero workers would only
  // run once a thread joins it, and V8 joins some jobs (Maglev's compile job)
  // only when it flushes them.
  const size_t background = workers > 1 ? all - 1 : 1;
  const bool other_task_runs = workers > 1;
  bool passed = true;

  for (v8::TaskPriority priority :
       {v8::TaskPriority::kBestEffort, v8::TaskPriority::kUserVisible}) {
    JobCase c(platform.get(), workers,
              priority == v8::TaskPriority::kBestEffort ? "kBestEffort job" : "kUserVisible job");
    auto handle = platform->PostJob(priority, c.NewJob());
    c.ExpectRunning(background);
    if (other_task_runs) c.ExpectOtherTaskToRun();
    passed = c.Finish(std::move(handle), background) && passed;
  }

  // Work someone waits for keeps the whole pool.
  {
    JobCase c(platform.get(), workers, "kUserBlocking job");
    auto handle = platform->PostJob(v8::TaskPriority::kUserBlocking, c.NewJob());
    c.ExpectRunning(all);
    passed = c.Finish(std::move(handle), all) && passed;
  }

  // V8 raises a running job to kUserBlocking when it starts waiting for it,
  // as it does with concurrent marking in a mark-compact pause. The job then
  // gets the whole pool without being joined.
  {
    JobCase c(platform.get(), workers, "kUserVisible job raised to kUserBlocking");
    auto handle = platform->PostJob(v8::TaskPriority::kUserVisible, c.NewJob());
    c.ExpectRunning(background);
    handle->UpdatePriority(v8::TaskPriority::kUserBlocking);
    c.ExpectRunning(all);
    passed = c.Finish(std::move(handle), all) && passed;
  }

  // Raising a job from CreateJob() doesn't start it: it starts on
  // NotifyConcurrencyIncrease(), with the whole pool.
  {
    JobCase c(platform.get(), workers, "created kUserVisible job raised before it starts");
    auto handle = platform->CreateJob(v8::TaskPriority::kUserVisible, c.NewJob());
    handle->UpdatePriority(v8::TaskPriority::kUserBlocking);
    c.ExpectNotStarted();
    handle->NotifyConcurrencyIncrease();
    c.ExpectRunning(all);
    passed = c.Finish(std::move(handle), all) && passed;
  }

  // A job lowered from kUserBlocking before it starts leaves a worker free.
  // (Lowering a running job takes effect as its workers return from Run(),
  // which these jobs never do while they are held.)
  {
    JobCase c(platform.get(), workers, "created kUserBlocking job lowered before it starts");
    auto handle = platform->CreateJob(v8::TaskPriority::kUserBlocking, c.NewJob());
    handle->UpdatePriority(v8::TaskPriority::kUserVisible);
    handle->NotifyConcurrencyIncrease();
    c.ExpectRunning(background);
    if (other_task_runs) c.ExpectOtherTaskToRun();
    passed = c.Finish(std::move(handle), background) && passed;
  }

  // A thread that joins a background job gets the whole pool to help it, as
  // before the limit: every worker plus the joining thread.
  {
    JobCase c(platform.get(), workers, "joined kUserVisible job");
    auto handle = platform->PostJob(v8::TaskPriority::kUserVisible, c.NewJob());
    c.ExpectRunning(background);
    passed = c.JoinWhileRunning(std::move(handle), all + 1) && passed;
  }

  platform.reset();
  if (passed) std::printf("background job limit with %d workers: ok\n", workers);
  return passed ? 0 : 1;
}
