// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cli/trace.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profile_event.h"
#include "fpag/debug/profiler/profiler.h"

namespace cli {

// A time-trace report: the recorded phases as a tree, with the reader's
// share attributed to each. The tree is built from the events' intervals,
// not from any call stack the compiler kept, so two runs of one command
// report the same shape for the same work.
struct TraceReport {
  // One tree node: an interval and the children nested inside it.
  struct Node {
    // The event this node covers. The name and the category are the
    // ids the profiler handed out, exactly as recorded.
    debug::ProfileEvent event;
    // Children nested in this interval, in start-time order. The vector
    // holds every child rather than a pruned few: pruning is the
    // renderer's, which decides per tree what is worth printing.
    //
    // A child's duration is inside its parent's already, because that is
    // what nesting means here, so a node's time is its own duration and
    // never the sum of what it holds. Two children that ran on two
    // threads can together exceed it, and the report is still right:
    // every figure is a share of the wall clock, not of the parent.
    std::vector<Node> children;
    // The process and the thread that ran the interval. A region the
    // frontend spreads over threads reads as several roots or siblings
    // here, named for their thread.
    u64 thread_id = 0;
    u32 process_id = 0;
  };

  // Roots in start-time order: the intervals no other interval contains.
  std::vector<Node> roots;
  // The whole command, from the earliest start to the latest end. Times
  // in the report are shares of this, which is what makes a child's
  // figure comparable to its sibling's however the two overlapped.
  u64 wall_ns = 0;
};

// Builds the tree from the events the trace captured and the profiler
// that resolves their names. Events outside the run - a global profiler
// left running, or two commands traced at once - read as several roots,
// which is the honest shape of what was recorded.
TraceReport build_trace_report(const TraceCapture& trace);

// The report as text: roots in start-time order, children under them in
// the same order, one row per node with its inclusive time and its share
// of the wall clock. Children past the reader's share collapse into one
// row, so a release build's hundreds of LLVM passes read as the pass
// pipeline and the passes that mattered rather than as a full listing.
std::string render_trace_text(const TraceReport& report,
                              const debug::Profiler& profiler);

}  // namespace cli
