// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/worker_bags.h"

#include <memory>
#include <utility>

#include "config/build_config.h"
#include "diag/bag.h"
#include "fpag/mem/page_allocator.h"
#include "i18n/language.h"

namespace diag {

namespace {

// One unit's worth of messages times a few, not times the units: a clean unit
// reports nothing, and a unit that reports past what is left has its excess
// dropped and counted, which the run reports, the same answer a per-unit arena
// gave.
#if BUILD_FLAG(IS_ARCH_64_BITS)
constexpr usize WORKER_DIAGNOSTIC_CAPACITY = 256ull << 10;
#else
constexpr usize WORKER_DIAGNOSTIC_CAPACITY = 128ull << 10;
#endif

}  // namespace

WorkerBags::Worker::Worker() {
  // The arena takes whole pages, and a page is not always the 4 KiB the
  // constant assumes: a wasm page is 64 KiB, so the reservation rounds up to
  // the host's.
  const usize page = mem::page_size();
  arena.reserve((WORKER_DIAGNOSTIC_CAPACITY + page - 1) & ~(page - 1));
}

WorkerBags::WorkerBags(i18n::Language language, usize workers, usize units)
    : language_(language), workers_(workers), units_(units) {}

diag::DiagBag& WorkerBags::bag_for(usize unit, usize worker) {
  std::unique_ptr<diag::DiagBag>& bag = units_[unit];
  if (bag == nullptr) {
    bag = std::make_unique<diag::DiagBag>(workers_[worker].arena, language_);
  }
  return *bag;
}

void WorkerBags::merge_into(diag::DiagBag& into, usize units) const {
  for (usize unit = 0; unit < units; ++unit) {
    if (units_[unit] != nullptr) {
      into.merge(*units_[unit]);
    }
  }
}

}  // namespace diag
