// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/maglev/maglev-concurrent-dispatcher.h"

#include "src/codegen/compiler.h"
#include "src/compiler/compilation-dependencies.h"
#include "src/compiler/js-heap-broker.h"
#include "src/execution/isolate.h"
#include "src/flags/flags.h"
#include "src/handles/persistent-handles.h"
#include "src/maglev/maglev-compilation-info.h"
#include "src/maglev/maglev-compiler.h"
#include "src/maglev/maglev-graph-labeller.h"
#include "src/maglev/maglev-pipeline-statistics.h"
#include "src/objects/js-function-inl.h"
#include "src/utils/identity-map.h"
#include "src/utils/locked-queue-inl.h"

namespace v8 {
namespace internal {

namespace compiler {

void JSHeapBroker::AttachLocalIsolateForMaglev(
    maglev::MaglevCompilationInfo* info, LocalIsolate* local_isolate) {
  DCHECK_NULL(local_isolate_);
  local_isolate_ = local_isolate;
  DCHECK_NOT_NULL(local_isolate_);
  local_isolate_->heap()->AttachPersistentHandles(
      info->DetachPersistentHandles());
}

void JSHeapBroker::DetachLocalIsolateForMaglev(
    maglev::MaglevCompilationInfo* info) {
  DCHECK_NULL(ph_);
  DCHECK_NOT_NULL(local_isolate_);
  std::unique_ptr<PersistentHandles> ph =
      local_isolate_->heap()->DetachPersistentHandles();
  local_isolate_ = nullptr;
  info->set_persistent_handles(std::move(ph));
}

}  // namespace compiler

namespace maglev {

namespace {

constexpr char kMaglevCompilerName[] = "Maglev";

// LocalIsolateScope encapsulates the phase where persistent handles are
// attached to the LocalHeap inside {local_isolate}.
class V8_NODISCARD LocalIsolateScope final {
 public:
  explicit LocalIsolateScope(MaglevCompilationInfo* info,
                             LocalIsolate* local_isolate)
      : info_(info) {
    info_->broker()->AttachLocalIsolateForMaglev(info_, local_isolate);
  }

  ~LocalIsolateScope() { info_->broker()->DetachLocalIsolateForMaglev(info_); }

 private:
  MaglevCompilationInfo* const info_;
};

}  // namespace

Zone* ExportedMaglevCompilationInfo::zone() const { return info_->zone(); }

void ExportedMaglevCompilationInfo::set_canonical_handles(
    std::unique_ptr<CanonicalHandlesMap>&& canonical_handles) {
  info_->set_canonical_handles(std::move(canonical_handles));
}

// static
std::unique_ptr<MaglevCompilationJob> MaglevCompilationJob::New(
    Isolate* isolate, Handle<JSFunction> function, BytecodeOffset osr_offset) {
  auto info = maglev::MaglevCompilationInfo::New(isolate, function, osr_offset);
  return std::unique_ptr<MaglevCompilationJob>(
      new MaglevCompilationJob(isolate, std::move(info)));
}

namespace {

MaglevPipelineStatistics* CreatePipelineStatistics(
    Isolate* isolate, MaglevCompilationInfo* compilation_info,
    compiler::ZoneStats* zone_stats) {
  MaglevPipelineStatistics* pipeline_stats = nullptr;
  bool tracing_enabled;
  TRACE_EVENT_CATEGORY_GROUP_ENABLED(TRACE_DISABLED_BY_DEFAULT("v8.maglev"),
                                     &tracing_enabled);
  if (tracing_enabled || v8_flags.maglev_stats || v8_flags.maglev_stats_nvp) {
    pipeline_stats = new MaglevPipelineStatistics(
        compilation_info, isolate->GetMaglevStatistics(), zone_stats);
  }
  return pipeline_stats;
}

}  // namespace

MaglevCompilationJob::MaglevCompilationJob(
    Isolate* isolate, std::unique_ptr<MaglevCompilationInfo>&& info)
    : OptimizedCompilationJob(kMaglevCompilerName, State::kReadyToPrepare),
      info_(std::move(info)),
      zone_stats_(isolate->allocator()),
      pipeline_statistics_(
          CreatePipelineStatistics(isolate, info_.get(), &zone_stats_)) {
  DCHECK(maglev::IsMaglevEnabled());
}

MaglevCompilationJob::~MaglevCompilationJob() = default;

CompilationJob::Status MaglevCompilationJob::PrepareJobImpl(Isolate* isolate) {
  BeginPhaseKind("V8.MaglevPrepareJob");
  if (info()->collect_source_positions()) {
    SharedFunctionInfo::EnsureSourcePositionsAvailable(
        isolate,
        info()->toplevel_compilation_unit()->shared_function_info().object());
  }
  EndPhaseKind();
  // TODO(v8:7700): Actual return codes.
  return CompilationJob::SUCCEEDED;
}

CompilationJob::Status MaglevCompilationJob::ExecuteJobImpl(
    RuntimeCallStats* stats, LocalIsolate* local_isolate) {
  BeginPhaseKind("V8.MaglevExecuteJob");
  LocalIsolateScope scope{info(), local_isolate};
  if (!maglev::MaglevCompiler::Compile(local_isolate, info())) {
    return CompilationJob::FAILED;
  }
  EndPhaseKind();
  // TODO(v8:7700): Actual return codes.
  return CompilationJob::SUCCEEDED;
}

CompilationJob::Status MaglevCompilationJob::FinalizeJobImpl(Isolate* isolate) {
  BeginPhaseKind("V8.MaglevFinalizeJob");
  Handle<Code> code;
  if (!maglev::MaglevCompiler::GenerateCode(isolate, info()).ToHandle(&code)) {
    return CompilationJob::FAILED;
  }
  info()->set_code(code);
  EndPhaseKind();
  return CompilationJob::SUCCEEDED;
}

MaybeHandle<Code> MaglevCompilationJob::code() const {
  return info_->get_code();
}

Handle<JSFunction> MaglevCompilationJob::function() const {
  return info_->toplevel_function();
}

BytecodeOffset MaglevCompilationJob::osr_offset() const {
  return info_->toplevel_osr_offset();
}

bool MaglevCompilationJob::is_osr() const { return info_->toplevel_is_osr(); }

bool MaglevCompilationJob::specialize_to_function_context() const {
  return info_->specialize_to_function_context();
}

void MaglevCompilationJob::RecordCompilationStats(Isolate* isolate) const {
  // Don't record samples from machines without high-resolution timers,
  // as that can cause serious reporting issues. See the thread at
  // http://g/chrome-metrics-team/NwwJEyL8odU/discussion for more details.
  if (base::TimeTicks::IsHighResolution()) {
    Counters* const counters = isolate->counters();
    counters->maglev_optimize_prepare()->AddSample(
        static_cast<int>(time_taken_to_prepare_.InMicroseconds()));
    counters->maglev_optimize_execute()->AddSample(
        static_cast<int>(time_taken_to_execute_.InMicroseconds()));
    counters->maglev_optimize_finalize()->AddSample(
        static_cast<int>(time_taken_to_finalize_.InMicroseconds()));
    counters->maglev_optimize_total_time()->AddSample(
        static_cast<int>(ElapsedTime().InMicroseconds()));
  }
  if (v8_flags.trace_opt_stats) {
    static double compilation_time = 0.0;
    static int compiled_functions = 0;
    static int code_size = 0;

    compilation_time += (time_taken_to_prepare_.InMillisecondsF() +
                         time_taken_to_execute_.InMillisecondsF() +
                         time_taken_to_finalize_.InMillisecondsF());
    compiled_functions++;
    code_size += function()->shared().SourceSize();
    PrintF(
        "[maglev] Compiled: %d functions with %d byte source size in %fms.\n",
        compiled_functions, code_size, compilation_time);
  }
}

void MaglevCompilationJob::BeginPhaseKind(const char* name) {
  if (V8_UNLIKELY(pipeline_statistics_ != nullptr)) {
    pipeline_statistics_->BeginPhaseKind(name);
  }
}

void MaglevCompilationJob::EndPhaseKind() {
  if (V8_UNLIKELY(pipeline_statistics_ != nullptr)) {
    pipeline_statistics_->EndPhaseKind();
  }
}

// The JobTask is posted to V8::GetCurrentPlatform(). It's responsible for
// processing the incoming queue on a worker thread.
class MaglevConcurrentDispatcher::JobTask final : public v8::JobTask {
 public:
  explicit JobTask(MaglevConcurrentDispatcher* dispatcher)
      : dispatcher_(dispatcher) {}

  void Run(JobDelegate* delegate) override {
    LocalIsolate local_isolate(isolate(), ThreadKind::kBackground);
    DCHECK(local_isolate.heap()->IsParked());

    while (!incoming_queue()->IsEmpty() && !delegate->ShouldYield()) {
      std::unique_ptr<MaglevCompilationJob> job;
      if (!incoming_queue()->Dequeue(&job)) break;
      DCHECK_NOT_NULL(job);
      TRACE_EVENT_WITH_FLOW0(
          TRACE_DISABLED_BY_DEFAULT("v8.compile"), "V8.MaglevBackground",
          job.get(), TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT);
      RCS_SCOPE(&local_isolate,
                RuntimeCallCounterId::kOptimizeBackgroundMaglev);
      CompilationJob::Status status =
          job->ExecuteJob(local_isolate.runtime_call_stats(), &local_isolate);
      if (status == CompilationJob::SUCCEEDED) {
        outgoing_queue()->Enqueue(std::move(job));
      }
    }
    isolate()->stack_guard()->RequestInstallMaglevCode();
  }

  size_t GetMaxConcurrency(size_t worker_count) const override {
    size_t num_tasks = incoming_queue()->size() + worker_count;
    size_t max_threads = v8_flags.concurrent_maglev_max_threads;
    if (max_threads > 0) {
      return std::min(max_threads, num_tasks);
    }
    return num_tasks;
  }

 private:
  Isolate* isolate() const { return dispatcher_->isolate_; }
  QueueT* incoming_queue() const { return &dispatcher_->incoming_queue_; }
  QueueT* outgoing_queue() const { return &dispatcher_->outgoing_queue_; }

  MaglevConcurrentDispatcher* const dispatcher_;
  const Handle<JSFunction> function_;
};

MaglevConcurrentDispatcher::MaglevConcurrentDispatcher(Isolate* isolate)
    : isolate_(isolate) {
  bool enable = v8_flags.concurrent_recompilation && maglev::IsMaglevEnabled();
  if (enable) {
    bool is_tracing =
        v8_flags.print_maglev_code || v8_flags.trace_maglev_graph_building ||
        v8_flags.trace_maglev_inlining || v8_flags.print_maglev_deopt_verbose ||
        v8_flags.print_maglev_graph || v8_flags.print_maglev_graphs ||
        v8_flags.trace_maglev_phi_untagging || v8_flags.trace_maglev_regalloc;

    if (is_tracing) {
      PrintF("Concurrent maglev has been disabled for tracing.\n");
      enable = false;
    }
  }
  if (enable) {
    TaskPriority priority = v8_flags.concurrent_maglev_high_priority_threads
                                ? TaskPriority::kUserBlocking
                                : TaskPriority::kUserVisible;
    job_handle_ = V8::GetCurrentPlatform()->PostJob(
        priority, std::make_unique<JobTask>(this));
    DCHECK(is_enabled());
  } else {
    DCHECK(!is_enabled());
  }
}

MaglevConcurrentDispatcher::~MaglevConcurrentDispatcher() {
  if (is_enabled() && job_handle_->IsValid()) {
    // Wait for the job handle to complete, so that we know the queue
    // pointers are safe.
    job_handle_->Cancel();
  }
}

void MaglevConcurrentDispatcher::EnqueueJob(
    std::unique_ptr<MaglevCompilationJob>&& job) {
  DCHECK(is_enabled());
  incoming_queue_.Enqueue(std::move(job));
  job_handle_->NotifyConcurrencyIncrease();
}

void MaglevConcurrentDispatcher::FinalizeFinishedJobs() {
  HandleScope handle_scope(isolate_);
  while (!outgoing_queue_.IsEmpty()) {
    std::unique_ptr<MaglevCompilationJob> job;
    outgoing_queue_.Dequeue(&job);
    TRACE_EVENT_WITH_FLOW0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
                           "V8.MaglevConcurrentFinalize", job.get(),
                           TRACE_EVENT_FLAG_FLOW_IN);
    RCS_SCOPE(isolate_,
              RuntimeCallCounterId::kOptimizeConcurrentFinalizeMaglev);
    Compiler::FinalizeMaglevCompilationJob(job.get(), isolate_);
  }
}

void MaglevConcurrentDispatcher::AwaitCompileJobs() {
  // Use Join to wait until there are no more queued or running jobs.
  job_handle_->Join();
  // Join kills the job handle, so drop it and post a new one.
  job_handle_ = V8::GetCurrentPlatform()->PostJob(
      TaskPriority::kUserVisible, std::make_unique<JobTask>(this));
  DCHECK(incoming_queue_.IsEmpty());
}

void MaglevConcurrentDispatcher::Flush(BlockingBehavior behavior) {
  while (!incoming_queue_.IsEmpty()) {
    std::unique_ptr<MaglevCompilationJob> job;
    incoming_queue_.Dequeue(&job);
  }
  if (behavior == BlockingBehavior::kBlock) {
    job_handle_->Cancel();
    job_handle_ = V8::GetCurrentPlatform()->PostJob(
        TaskPriority::kUserVisible, std::make_unique<JobTask>(this));
  }
  while (!outgoing_queue_.IsEmpty()) {
    std::unique_ptr<MaglevCompilationJob> job;
    outgoing_queue_.Dequeue(&job);
  }
}

}  // namespace maglev
}  // namespace internal
}  // namespace v8
