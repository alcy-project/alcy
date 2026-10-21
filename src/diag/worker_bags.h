// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <vector>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"

namespace diag {

// The bags work spread over threads reports into: one arena per worker and one
// bag per unit of that work.
//
// Which worker takes a unit is not knowable until it takes one, so a unit's
// bag is built by the thread that claims it, over that worker's arena. The
// arena is one per worker rather than one per unit because a unit that reports
// nothing would still have reserved one of its own. A worker's arena is
// written by one thread only -- the worker that owns it -- so sharing it needs
// no synchronisation, and what shares it is ordered by that worker taking one
// unit at a time.
//
// A unit's bag has to survive until the merge, which reads the units in order:
// a run whose reporting is spread over threads then reports what a run that is
// not spread reports, because what is read back is ordered by unit and not by
// the order the work finished.
class WorkerBags {
 public:
  WorkerBags(i18n::Language language, usize workers, usize units);

  WorkerBags(const WorkerBags&) = delete;
  WorkerBags& operator=(const WorkerBags&) = delete;
  WorkerBags(WorkerBags&&) = delete;
  WorkerBags& operator=(WorkerBags&&) = delete;

  // The bag for `unit`, built on `worker`'s arena the first time the unit
  // reports anything. Called by the thread that owns `worker`, and only by it.
  [[nodiscard]] diag::DiagBag& bag_for(usize unit, usize worker);

  // Merges the first `units` units' bags into `into`, in unit order. A unit
  // that reported nothing has no bag and contributes nothing.
  void merge_into(diag::DiagBag& into, usize units) const;

 private:
  // What one worker's units report into.
  struct Worker {
    Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    mem::Arena arena;
  };

  i18n::Language language_;
  std::vector<Worker> workers_;
  std::vector<std::unique_ptr<diag::DiagBag>> units_;
};

}  // namespace diag
