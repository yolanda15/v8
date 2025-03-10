// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_SWEEPER_H_
#define V8_HEAP_SWEEPER_H_

#include <map>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/base/optional.h"
#include "src/base/platform/condition-variable.h"
#include "src/common/globals.h"
#include "src/flags/flags.h"
#include "src/heap/gc-tracer.h"
#include "src/heap/memory-allocator.h"
#include "src/heap/pretenuring-handler.h"
#include "src/heap/slot-set.h"
#include "src/tasks/cancelable-task.h"

namespace v8 {
namespace internal {

class MemoryChunk;
class NonAtomicMarkingState;
class Page;
class LargePage;
class PagedSpaceBase;
class Space;

enum class FreeSpaceTreatmentMode { kIgnoreFreeSpace, kZapFreeSpace };

class Sweeper {
 public:
  using SweepingList = std::vector<Page*>;
  using SweptList = std::vector<Page*>;

  enum class SweepingMode { kEagerDuringGC, kLazyOrConcurrent };

  // LocalSweeper holds local data structures required for sweeping and is used
  // to initiate sweeping and promoted page iteration on multiple threads. Each
  // thread should holds its own LocalSweeper. Once sweeping is done, all
  // LocalSweepers should be finalized on the main thread.
  //
  // LocalSweeper is not thread-safe and should not be concurrently by several
  // threads. The exceptions to this rule are allocations during parallel
  // evacuation and from concurrent allocators. In practice the data structures
  // in LocalSweeper are only actively used for new space sweeping. Since
  // parallel evacuators and concurrent allocators never try to allocate in new
  // space, they will never contribute to new space sweeping and thus can use
  // the main thread's local sweeper without risk of data races.
  class LocalSweeper final {
   public:
    explicit LocalSweeper(Sweeper* sweeper) : sweeper_(sweeper) {
      DCHECK_NOT_NULL(sweeper_);
    }
    ~LocalSweeper() = default;

    int ParallelSweepSpace(AllocationSpace identity, SweepingMode sweeping_mode,
                           int required_freed_bytes, int max_pages = 0);
    void ContributeAndWaitForPromotedPagesIteration();

   private:
    int ParallelSweepPage(Page* page, AllocationSpace identity,
                          SweepingMode sweeping_mode);

    void ParallelIterateAndSweepPromotedPages();
    void ParallelIterateAndSweepPromotedPage(MemoryChunk* chunk);

    Sweeper* const sweeper_;

    friend class Sweeper;
  };

  explicit Sweeper(Heap* heap);
  ~Sweeper();

  bool major_sweeping_in_progress() const {
    return major_sweeping_state_.in_progress();
  }
  bool minor_sweeping_in_progress() const {
    return minor_sweeping_state_.in_progress();
  }
  bool sweeping_in_progress() const {
    return minor_sweeping_in_progress() || major_sweeping_in_progress();
  }
  bool sweeping_in_progress_for_space(AllocationSpace space) const {
    if (space == NEW_SPACE) return minor_sweeping_in_progress();
    return major_sweeping_in_progress();
  }

  void TearDown();

  void AddPage(AllocationSpace space, Page* page);
  void AddNewSpacePage(Page* page);
  void AddPromotedPage(MemoryChunk* chunk);

  int ParallelSweepSpace(AllocationSpace identity, SweepingMode sweeping_mode,
                         int required_freed_bytes, int max_pages = 0);

  void EnsurePageIsSwept(Page* page);
  void WaitForPageToBeSwept(Page* page);

  // After calling this function sweeping is considered to be in progress
  // and the main thread can sweep lazily, but the background sweeper tasks
  // are not running yet.
  void StartMajorSweeping();
  void StartMinorSweeping();
  V8_EXPORT_PRIVATE void StartMajorSweeperTasks();
  V8_EXPORT_PRIVATE void StartMinorSweeperTasks();
  void EnsureMajorCompleted();
  void EnsureMinorCompleted();
  void DrainSweepingWorklistForSpace(AllocationSpace space);

  bool AreMinorSweeperTasksRunning();
  bool AreMajorSweeperTasksRunning();

  Page* GetSweptPageSafe(PagedSpaceBase* space);
  SweptList GetAllSweptPagesSafe(PagedSpaceBase* space);

  bool IsSweepingDoneForSpace(AllocationSpace space) const;

  GCTracer::Scope::ScopeId GetTracingScope(AllocationSpace space,
                                           bool is_joining_thread);

  bool IsIteratingPromotedPages() const;
  void ContributeAndWaitForPromotedPagesIteration();

  bool ShouldRefillFreelistForSpace(AllocationSpace space) const;

  void SweepEmptyNewSpacePage(Page* page);

 private:
  NonAtomicMarkingState* marking_state() const { return marking_state_; }

  int RawSweep(Page* p, FreeSpaceTreatmentMode free_space_treatment_mode,
               SweepingMode sweeping_mode, bool should_reduce_memory,
               bool is_promoted_page);

  void AddPageImpl(AllocationSpace space, Page* page);

  class ConcurrentMajorSweeper;
  class ConcurrentMinorSweeper;

  class MajorSweeperJob;
  class MinorSweeperJob;

  static constexpr int kNumberOfSweepingSpaces =
      LAST_SWEEPABLE_SPACE - FIRST_SWEEPABLE_SPACE + 1;

  template <typename Callback>
  void ForAllSweepingSpaces(Callback callback) const {
    if (v8_flags.minor_mc) {
      callback(NEW_SPACE);
    }
    callback(OLD_SPACE);
    callback(CODE_SPACE);
    callback(SHARED_SPACE);
  }

  // Helper function for RawSweep. Depending on the FreeListRebuildingMode and
  // FreeSpaceTreatmentMode this function may add the free memory to a free
  // list, make the memory iterable, clear it, and return the free memory to
  // the operating system.
  size_t FreeAndProcessFreedMemory(
      Address free_start, Address free_end, Page* page, Space* space,
      FreeSpaceTreatmentMode free_space_treatment_mode,
      bool should_reduce_memory);

  // Helper function for RawSweep. Handle remembered set entries in the freed
  // memory which require clearing.
  void CleanupRememberedSetEntriesForFreedMemory(
      Address free_start, Address free_end, Page* page, bool record_free_ranges,
      TypedSlotSet::FreeRangesMap* free_ranges_map, SweepingMode sweeping_mode);

  // Helper function for RawSweep. Clears invalid typed slots in the given free
  // ranges.
  void CleanupTypedSlotsInFreeMemory(
      Page* page, const TypedSlotSet::FreeRangesMap& free_ranges_map,
      SweepingMode sweeping_mode);

  // Helper function for RawSweep. Clears the mark bits and ensures consistency
  // of live bytes.
  void ClearMarkBitsAndHandleLivenessStatistics(Page* page, size_t live_bytes);

  // Can only be called on the main thread when no tasks are running.
  bool IsDoneSweeping() const {
    bool is_done = true;
    ForAllSweepingSpaces([this, &is_done](AllocationSpace space) {
      DCHECK_EQ(IsSweepingDoneForSpace(space),
                sweeping_list_[GetSweepSpaceIndex(space)].empty());
      if (!sweeping_list_[GetSweepSpaceIndex(space)].empty()) is_done = false;
    });
    return is_done;
  }

  size_t ConcurrentMinorSweepingPageCount();
  size_t ConcurrentMajorSweepingPageCount();

  Page* GetSweepingPageSafe(AllocationSpace space);
  MemoryChunk* GetPromotedPageSafe();
  std::vector<MemoryChunk*> GetAllPromotedPagesForIterationSafe();
  bool TryRemoveSweepingPageSafe(AllocationSpace space, Page* page);
  bool TryRemovePromotedPageSafe(MemoryChunk* chunk);

  void PrepareToBeSweptPage(AllocationSpace space, Page* page);

  static bool IsValidSweepingSpace(AllocationSpace space) {
    return space >= FIRST_SWEEPABLE_SPACE && space <= LAST_SWEEPABLE_SPACE;
  }

  static int GetSweepSpaceIndex(AllocationSpace space) {
    DCHECK(IsValidSweepingSpace(space));
    return space - FIRST_SWEEPABLE_SPACE;
  }

  void NotifyPromotedPageIterationFinished(MemoryChunk* chunk);
  void NotifyPromotedPagesIterationFinished();

  void AddSweptPage(Page* page, AllocationSpace identity);

  enum class SweepingScope { kMinor, kMajor };
  template <SweepingScope scope>
  class SweepingState {
    using ConcurrentSweeper =
        typename std::conditional<scope == SweepingScope::kMinor,
                                  ConcurrentMinorSweeper,
                                  ConcurrentMajorSweeper>::type;
    using SweeperJob =
        typename std::conditional<scope == SweepingScope::kMinor,
                                  MinorSweeperJob, MajorSweeperJob>::type;

   public:
    explicit SweepingState(Sweeper* sweeper);
    ~SweepingState();

    void StartSweeping();
    void StartConcurrentSweeping();
    void StopConcurrentSweeping();
    void FinishSweeping();

    bool HasValidJob() const;
    bool HasActiveJob() const;

    bool in_progress() const { return in_progress_; }
    bool should_reduce_memory() const { return should_reduce_memory_; }
    std::vector<ConcurrentSweeper>& concurrent_sweepers() {
      return concurrent_sweepers_;
    }

   private:
    Sweeper* sweeper_;
    // Main thread can finalize sweeping, while background threads allocation
    // slow path checks this flag to see whether it could support concurrent
    // sweeping.
    std::atomic<bool> in_progress_{false};
    std::unique_ptr<JobHandle> job_handle_;
    std::vector<ConcurrentSweeper> concurrent_sweepers_;
    bool should_reduce_memory_ = false;
  };

  Heap* const heap_;
  NonAtomicMarkingState* const marking_state_;
  base::Mutex mutex_;
  base::ConditionVariable cv_page_swept_;
  SweptList swept_list_[kNumberOfSweepingSpaces];
  SweepingList sweeping_list_[kNumberOfSweepingSpaces];
  std::atomic<bool> has_sweeping_work_[kNumberOfSweepingSpaces]{false};
  std::atomic<bool> has_swept_pages_[kNumberOfSweepingSpaces]{false};
  std::vector<MemoryChunk*> sweeping_list_for_promoted_page_iteration_;
  LocalSweeper main_thread_local_sweeper_;
  SweepingState<SweepingScope::kMajor> major_sweeping_state_{this};
  SweepingState<SweepingScope::kMinor> minor_sweeping_state_{this};

  // The following fields are used for maintaining an order between iterating
  // promoted pages and sweeping array buffer extensions.
  size_t promoted_pages_for_iteration_count_ = 0;
  std::atomic<size_t> iterated_promoted_pages_count_{0};
  base::Mutex promoted_pages_iteration_notification_mutex_;
  base::ConditionVariable promoted_pages_iteration_notification_variable_;
  std::atomic<bool> promoted_page_iteration_in_progress_{false};
  bool should_iterate_promoted_pages_ = false;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_SWEEPER_H_
