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

std::string render(const Recorded& recorded,
                   u64 wall_ns = 0,
                   bool color = false) {
  const TraceReport report = build_trace_report(recorded.trace);
  return render_trace_text(report, recorded.profiler, wall_ns, color);
}

// The rows of a rendered report, header included.
std::vector<std::string_view> rows(std::string_view text) {
  std::vector<std::string_view> lines;
  usize start = 0;
  while (start < text.size()) {
    const usize end = text.find('\n', start);
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

// One row of a report whose single event has this duration. Used to pin
// down the units without a tree around them.
std::string one_row(u64 duration_ns) {
  Recorded recorded;
  add_event(recorded, "phase", "frontend", 0, duration_ns);
  recorded.trace.events = recorded.events;
  return render(recorded);
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
  CHECK(report.event_count == 4);
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

// The header names the amount of input the rows were pruned from, so a
// reader can tell a quiet run from a pruned one.
TEST_CASE("A trace report says how many events it read") {
  CHECK(one_row(10).starts_with("time trace (1 event)\n"));

  Recorded many;
  add_event(many, "run", "frontend", 0, 10);
  add_event(many, "child", "frontend", 1, 2);
  add_event(many, "other", "frontend", 5, 2);
  many.trace.events = many.events;
  CHECK(render(many).starts_with("time trace (3 events)\n"));
}

// The figures are a table: every row starts its duration at the same
// byte whatever the name's length or the row's depth, and its share
// after it. A reader scanning down a column reads durations without
// re-finding the column on every line.
TEST_CASE("A trace report puts every figure in the same column") {
  Recorded recorded;
  add_event(recorded, "run", "backend", 0, 1000);
  add_event(recorded, "a-child-with-a-name-longer-than-the-column", "backend",
            10, 500);
  add_event(recorded, "gc", "llvm-pass", 20, 100);
  add_event(recorded, "tiny", "backend", 30, 1);

  recorded.trace.events = recorded.events;
  const std::string text = render(recorded);
  const std::vector<std::string_view> lines = rows(text);
  CHECK(lines.size() == 5);
  if (lines.size() != 5) {
    return;
  }
  for (usize i = 1; i < lines.size(); ++i) {
    CHECK(lines[i].size() >= 57);
    if (lines[i].size() < 57) {
      continue;
    }
    CHECK(lines[i].substr(41, 8).find_first_not_of(' ') !=
          std::string_view::npos);
    CHECK(lines[i].substr(51, 6).ends_with('%'));
  }
}

// A duration keeps three significant digits in whatever unit is largest
// enough to hold them: the point does not wander, and a sub-microsecond
// phase does not round away to zero.
TEST_CASE("A trace report picks the unit for each duration") {
  CHECK(one_row(999).find("999 ns") != std::string::npos);
  CHECK(one_row(4620).find("4.62 us") != std::string::npos);
  CHECK(one_row(337000).find("337 us") != std::string::npos);
  CHECK(one_row(4620000).find("4.62 ms") != std::string::npos);
  CHECK(one_row(57900000).find("57.9 ms") != std::string::npos);
  CHECK(one_row(1520000000).find("1.52 s") != std::string::npos);
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
  const std::string text = render(recorded);
  CHECK(text.find("big") != std::string::npos);
  CHECK(text.find("tiny-a") == std::string::npos);
  CHECK(text.find("... 3 more") != std::string::npos);
  // The run row carries the wall as its total, and every row carries its
  // share of it; the collapsed row carries the sum of what it stands for.
  CHECK(text.find("100.0%") != std::string::npos);
  CHECK(text.find("0.5%") != std::string::npos);
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
  CHECK(render(recorded).find("... 6 more") != std::string::npos);
}

// A share under a tenth of a percent is not a zero, and the collapsed
// row that carries it says so rather than rounding it away.
TEST_CASE("A trace report prints a share below the floor as a bound") {
  Recorded recorded;
  add_event(recorded, "run", "frontend", 0, 100000);
  for (u32 i = 0; i < 10; ++i) {
    add_event(recorded, "speck", "frontend", 10 + i * 2, 1);
  }

  recorded.trace.events = recorded.events;
  const std::string text = render(recorded);
  CHECK(text.find("... 10 more") != std::string::npos);
  CHECK(text.find("<0.1%") != std::string::npos);
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
  const std::string text = render(recorded);
  CHECK(text.find("small") < text.find("large"));
}

// A category is an annotation on a change: a phase that stays inside
// its parent's category does not repeat it on every line, and the line
// where the work moves somewhere else says where.
TEST_CASE("A trace report prints a category only where it changes") {
  Recorded recorded;
  add_event(recorded, "run", "frontend", 0, 1000);
  add_event(recorded, "parse", "frontend", 10, 800);
  add_event(recorded, "emit", "backend", 20, 100);

  recorded.trace.events = recorded.events;
  const std::string text = render(recorded);
  const std::vector<std::string_view> lines = rows(text);
  CHECK(lines.size() == 4);
  if (lines.size() != 4) {
    return;
  }
  CHECK(lines[1].find("frontend") != std::string_view::npos);
  // The same category as the parent's: the row ends at the share.
  CHECK(lines[2].size() == 57);
  CHECK(lines[3].find("backend") != std::string_view::npos);
}

// The shares divide by the wall the caller hands over - the invocation's
// wall, which the result line prints - so the two blocks agree; a zero
// falls back to the report's own span.
TEST_CASE("A trace report divides by the wall it is handed") {
  Recorded recorded;
  add_event(recorded, "run", "frontend", 0, 1000);

  recorded.trace.events = recorded.events;
  const TraceReport report = build_trace_report(recorded.trace);
  const std::string doubled =
      render_trace_text(report, recorded.profiler, 2000, false);
  CHECK(doubled.find("50.0%") != std::string::npos);
  const std::string span =
      render_trace_text(report, recorded.profiler, 0, false);
  CHECK(span.find("100.0%") != std::string::npos);
}

// Colour marks the annotations, not the data: a share and a category
// dim, because the duration is what a reader scans for; without colour
// the text is plain.
TEST_CASE("A trace report dims annotations only when colour is on") {
  Recorded recorded;
  add_event(recorded, "run", "backend", 0, 1000);
  add_event(recorded, "child", "backend", 10, 500);

  recorded.trace.events = recorded.events;
  const std::string plain = render(recorded, 0, false);
  const std::string colored = render(recorded, 0, true);
  CHECK(plain.find("\x1b[") == std::string::npos);
  CHECK(colored.find("\x1b[2m") != std::string::npos);
  CHECK(colored.find("\x1b[0m") != std::string::npos);
}

}  // namespace cli
