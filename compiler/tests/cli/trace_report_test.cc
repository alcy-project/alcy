// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/trace_report.h"

#include <string>
#include <string_view>
#include <vector>

#include "cli/trace.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profiler.h"
#include "fpag/str/string_pool_id.h"

namespace cli {

namespace {

// One event in a case-built profiler, with the trace pointing at it.
// The profiler starts so the events it records land; the trace borrows
// nothing, because the report reads it synchronously. Each addition
// refreshes the trace, which is also why the report never needs to know
// which profiler recorded what.
struct Recorded {
  debug::Profiler profiler;
  std::vector<debug::ProfileEvent> events;
  TraceCapture trace;

  Recorded() {
    profiler.start();
    trace.profiler = &profiler;
  }
};

void add_event(Recorded& recorded,
               std::string_view name,
               std::string_view category,
               u64 start_ns,
               u64 duration_ns,
               u64 thread_id = 0) {
  debug::ProfileEvent event;
  event.name = recorded.profiler.intern(name);
  event.category = recorded.profiler.intern(category);
  event.start_time_ns = start_ns;
  event.duration_ns = duration_ns;
  event.thread_id = thread_id;
  recorded.events.push_back(event);
}

}  // namespace

// The tree nests by containment and orders by start time: a phase inside
// another is its child wherever it was recorded, and siblings read in the
// order they ran.
TEST_CASE("A trace report builds a tree in start-time order") {
  Recorded recorded;
  add_event(recorded, "analyze", "frontend", 30, 20);
  add_event(recorded, "load", "frontend", 10, 10);
  add_event(recorded, "lower", "frontend", 60, 10);
  // A child of analyze, whichever order the recorder saw it in.
  add_event(recorded, "bodies", "analyze", 32, 8);

  recorded.trace.events = recorded.events;
  const TraceReport report = build_trace_report(recorded.trace);
  CHECK(report.wall_ns == 60);
  CHECK(report.roots.size() == 3);
  if (report.roots.size() != 3) {
    return;
  }
  CHECK(recorded.profiler.name(report.roots[0].event.name) == "load");
  CHECK(recorded.profiler.name(report.roots[1].event.name) == "analyze");
  CHECK(recorded.profiler.name(report.roots[2].event.name) == "lower");
  CHECK(report.roots[1].children.size() == 1);
  if (report.roots[1].children.size() != 1) {
    return;
  }
  CHECK(recorded.profiler.name(report.roots[1].children[0].event.name) ==
        "bodies");
  // A node's time is its own interval: the child is inside it already,
  // so nothing is added to it.
  CHECK(report.roots[1].event.duration_ns == 20);
}

// Two regions can hold the same wall time on two threads, so the report
// answers every share against the wall clock rather than the parent: the
// shares can sum past one hundred percent, and the tree still reads.
TEST_CASE("A trace report shares overlapping regions against the wall") {
  Recorded recorded;
  add_event(recorded, "parse", "frontend", 10, 40, 1);
  add_event(recorded, "parse", "frontend", 10, 40, 2);

  recorded.trace.events = recorded.events;
  const TraceReport report = build_trace_report(recorded.trace);
  CHECK(report.roots.size() == 2);
  if (report.roots.size() != 2) {
    return;
  }
  CHECK(report.wall_ns == 40);
  CHECK(report.roots[0].event.duration_ns == 40);
  CHECK(report.roots[1].event.duration_ns == 40);
}

// Equal intervals read as siblings whatever the schedule did: nesting
// them by time would read differently run to run, and record order is
// the tiebreak the builder promises.
TEST_CASE("A trace report keeps equal intervals as siblings") {
  Recorded recorded;
  add_event(recorded, "tokenize", "frontend", 10, 10);
  add_event(recorded, "parse", "frontend", 10, 10);

  recorded.trace.events = recorded.events;
  const TraceReport report = build_trace_report(recorded.trace);
  CHECK(report.roots.size() == 2);
}

// A child worth showing on its own is one at or above half a percent of
// the wall: its size against the whole run, not against its parent, is
// what decides. Everything else collapses into one row that says how
// many events it stands for and what they cost together.
TEST_CASE("A trace report shows what matters and collapses the rest") {
  Recorded recorded;
  // The wall is 1000 by construction; the floor is 5.
  add_event(recorded, "run", "frontend", 0, 1000);
  add_event(recorded, "big", "frontend", 10, 900);
  add_event(recorded, "tiny-a", "frontend", 20, 2);
  add_event(recorded, "tiny-b", "frontend", 30, 2);
  add_event(recorded, "tiny-c", "frontend", 40, 1);

  recorded.trace.events = recorded.events;
  const TraceReport report = build_trace_report(recorded.trace);
  CHECK(report.roots.size() == 1);
  if (report.roots.size() != 1) {
    return;
  }
  const std::string text = render_trace_text(report, recorded.profiler);
  CHECK(text.find("big") != std::string::npos);
  CHECK(text.find("tiny-a") == std::string::npos);
  CHECK(text.find("3 more") != std::string::npos);
  // The run row carries the wall as its total, and every row carries its
  // share of it.
  CHECK(text.find("100.0%") != std::string::npos);
}

// The 95% cover rule keeps the shape without reading every child: the
// largest children that together cover ninety-five percent of the
// parent's own time stay, and a long tail of equal small ones becomes
// one row even when each clears the floor on its own.
TEST_CASE("A trace report collapses a long tail past the cover") {
  Recorded recorded;
  add_event(recorded, "run", "frontend", 0, 1000);
  // Thirty children of 40 each: every one clears the floor on its own,
  // and the largest that fit inside the parent's ninety-five percent
  // cover are twenty-four of them, so six fall outside.
  for (u32 i = 0; i < 30; ++i) {
    add_event(recorded, "child", "frontend", 10 + i * 30, 40);
  }

  recorded.trace.events = recorded.events;
  const TraceReport report = build_trace_report(recorded.trace);
  CHECK(report.roots.size() == 1);
  if (report.roots.size() != 1) {
    return;
  }
  const std::string text = render_trace_text(report, recorded.profiler);
  CHECK(text.find("6 more") != std::string::npos);
}

// The rows stay a timeline: children read in start-time order no matter
// how the sizes ran, and a child that starts earlier never sorts below
// a larger one that starts later.
TEST_CASE("A trace report orders rows by start time, not by size") {
  Recorded recorded;
  add_event(recorded, "run", "frontend", 0, 1000);
  add_event(recorded, "small", "frontend", 10, 10);
  add_event(recorded, "large", "frontend", 100, 800);

  recorded.trace.events = recorded.events;
  const TraceReport report = build_trace_report(recorded.trace);
  const std::string text = render_trace_text(report, recorded.profiler);
  CHECK(text.find("small") < text.find("large"));
}

}  // namespace cli
