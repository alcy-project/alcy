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
  // How many events the trace held. The rows show what survived
  // pruning, so the header says what the pruning read through.
  usize event_count = 0;
};

// Builds the tree from the events the trace captured and the profiler
// that resolves their names. Events outside the run - a global profiler
// left running, or two commands traced at once - read as several roots,
// which is the honest shape of what was recorded.
TraceReport build_trace_report(const TraceCapture& trace);

// The report as text: a header with the event count, then roots in
// start-time order with their children under them, one row per node.
// Every row puts its name at a fixed column, then its duration and its
// share of the wall; the columns start at the same byte whatever the
// depth, so the figures scan as a table. A share below a tenth of a
// percent reads `<0.1%`, a statement about the measurement rather than a
// rounded zero.
//
// Children past the reader's share collapse into one row - `... 48 more`
// - carrying their count and their total, so the tree stays a summary
// without silently dropping time. A category is printed only where it
// differs from the parent's. With `color`, shares and categories dim,
// and a collapsed row dims whole: they annotate the data, they are not
// it.
//
// `wall_ns` is what every share and every pruning floor answers against;
// zero falls back to the report's own span. The caller passes the
// envelope's wall, so the percentages agree with the duration printed on
// the result line above them.
std::string render_trace_text(const TraceReport& report,
                              const debug::Profiler& profiler,
                              u64 wall_ns,
                              bool color);

}  // namespace cli
