#include "../../v8/src/edge_v8_platform.h"

#include <cstdio>
#include <memory>
#include <utility>

#include <v8.h>

namespace {
class RepostingForegroundTask final : public v8::Task {
 public:
  RepostingForegroundTask(std::shared_ptr<v8::TaskRunner> runner, int* ran, int remaining)
      : runner_(std::move(runner)), ran_(ran), remaining_(remaining) {}

  void Run() override {
    ++*ran_;
    if (remaining_ > 1) {
      runner_->PostTask(std::make_unique<RepostingForegroundTask>(runner_, ran_, remaining_ - 1));
    }
  }

 private:
  std::shared_ptr<v8::TaskRunner> runner_;
  int* ran_;
  int remaining_;
};
}  // namespace

// Own V8 initialization and disposal in a separate process so this platform
// cannot interfere with the environments of other N-API test fixtures.
int main() {
  EdgeV8Platform::SetWorkerThreadCount(1);
  auto platform = EdgeV8Platform::Create();
  if (!platform) return 1;
  v8::V8::InitializePlatform(platform.get());
  if (!v8::V8::Initialize()) return 1;
  auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = allocator.get();
  v8::Isolate* isolate = v8::Isolate::New(params);
  if (!isolate) return 1;
  bool passed = true;
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    auto runner = platform->GetForegroundTaskRunner(isolate, v8::TaskPriority::kUserVisible);
    // Clear bootstrap tasks before introducing the deterministic task chain.
    for (int turn = 0; turn < 32; ++turn) platform->PumpPendingForegroundTasks(isolate);
    int ran = 0;
    runner->PostTask(std::make_unique<RepostingForegroundTask>(runner, &ran, 4));
    for (int turn = 1; turn <= 4; ++turn) {
      platform->PumpPendingForegroundTasks(isolate);
      if (ran != turn) {
        std::fprintf(stderr, "foreground turn %d ran %d tasks\n", turn, ran);
        passed = false;
      }
    }
    platform->PumpPendingForegroundTasks(isolate);
    passed = passed && ran == 4;
  }
  platform->DisposeIsolate(isolate);
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  return passed ? 0 : 1;
}
