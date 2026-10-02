// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/trace_report.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli/trace.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profile_event.h"
#include "fpag/debug/profiler/profiler.h"
#include "fpag/str/string_pool_id.h"

namespace cli {

namespace {

// Whether `inner` runs inside `outer`: it starts no earlier and ends no
// later. Equal intervals nest by record order rather than by time, so two
// events that start and end on the same nanosecond read as siblings -
// which is what per-file phases in a single-threaded run are.
bool contains(const debug::ProfileEvent& outer,
              const debug::ProfileEvent& inner) {
  if (&outer == &inner) {
    return false;
  }
  return outer.start_time_ns <= inner.start_time_ns &&
         inner.start_time_ns + inner.duration_ns <=
             outer.start_time_ns + outer.duration_ns;
}

u64 end_ns(const debug::ProfileEvent& event) {
  return event.start_time_ns + event.duration_ns;
}

// A node's own name, which is what the rows say. An event the profiler
// never interned reports nothing, which is also what the flat totals
// below used to print for one.
std::string_view node_name(const debug::Profiler& profiler,
                           const TraceReport::Node& node) {
  return node.event.name == str::INVALID_STRING_POOL_ID
             ? std::string_view()
             : profiler.name(node.event.name);
}

std::string_view node_category(const debug::Profiler& profiler,
                               const TraceReport::Node& node) {
  return node.event.category == str::INVALID_STRING_POOL_ID
             ? std::string_view()
             : profiler.name(node.event.category);
}

struct RenderedRow {
  std::string text;
  // Children past the reader's share collapse into one row, so a flat
  // count of what collapsed rides along: a collapsed row that says how
  // many events it stands for is a summary, and one that does not is a
  // cut.
  u64 collapsed = 0;
  u64 collapsed_ns = 0;
};

// A child worth showing on its own: the reader's share of the wall
// clock, at or above the floor. The root's share of the wall is what
// every child answers against, so a phase that matters in a small
// program and a phase that matters in a large one clear the same bar.
bool worth_showing(u64 child_ns, u64 wall_ns) {
  if (wall_ns == 0) {
    return true;
  }
  // Half a percent: at or above it a phase is a phase, and below it a
  // phase is a line item. Fixed rather than adaptive, so two runs of
  // one command agree about what they show.
  return child_ns * 200 >= wall_ns;
}

// Children worth naming, in start-time order, and what collapsed past
// them. The 95% cover rule keeps the shape: the largest children that
// together cover ninety-five percent of the parent's own time stay, and
// the rest - however many of them there are - become one row. A parent
// whose children are all alike therefore reads as its largest few and
// their remainder, and a parent with one dominant child reads as that
// child.
void split_children(const TraceReport::Node& node,
                    u64 wall_ns,
                    std::vector<const TraceReport::Node*>& shown,
                    std::vector<const TraceReport::Node*>& collapsed) {
  struct Candidate {
    const TraceReport::Node* node;
    u64 ns;
  };
  std::vector<Candidate> by_size;
  for (const TraceReport::Node& child : node.children) {
    if (worth_showing(child.event.duration_ns, wall_ns)) {
      by_size.push_back({&child, child.event.duration_ns});
    } else {
      collapsed.push_back(&child);
    }
  }
  std::sort(by_size.begin(), by_size.end(),
            [](const Candidate& a, const Candidate& b) {
              if (a.ns != b.ns) {
                return a.ns > b.ns;
              }
              return a.node->event.start_time_ns < b.node->event.start_time_ns;
            });
  // Keep the largest while they cover less than ninety-five percent of
  // the parent's own time: what is left over - the tail, and everything
  // below the floor - is the remainder.
  u64 kept = 0;
  const u64 cover = node.event.duration_ns * 95 / 100;
  for (const Candidate& candidate : by_size) {
    if (kept >= cover && !shown.empty()) {
      collapsed.push_back(candidate.node);
      continue;
    }
    shown.push_back(candidate.node);
    kept += candidate.ns;
  }
  // The rows stay in start-time order whatever the sizes were: the tree
  // is a timeline, and sorting by size would unmake it.
  std::sort(shown.begin(), shown.end(),
            [](const TraceReport::Node* a, const TraceReport::Node* b) {
              return a->event.start_time_ns < b->event.start_time_ns;
            });
}

void append_row(std::string& out,
                const debug::Profiler& profiler,
                const TraceReport::Node& node,
                u64 wall_ns,
                usize depth) {
  // LLVM pass names carry their template signature -
  // `PassManager<LazyCallGraph::SCC, ...>` - and an unbounded column
  // would push every time and share off the line a reader is scanning.
  // The head of the name is the part worth reading, so the tail goes.
  std::string name{node_name(profiler, node)};
  if (name.size() > 34) {
    name = name.substr(0, 33) + "\u2026";
  }
  const std::string category{node_category(profiler, node)};
  const double ms = static_cast<double>(node.event.duration_ns) / 1.0e6;
  const double share = wall_ns == 0
                           ? 0.0
                           : static_cast<double>(node.event.duration_ns) *
                                 100.0 / static_cast<double>(wall_ns);
  std::string indent(depth * 2, ' ');
  fmt::format_to(std::back_inserter(out),
                 "{}{:<34} {:>8.3} ms  {:>5.1f}%  {}\n", indent, name, ms,
                 share, category);
}

void append_collapsed(std::string& out,
                      const std::vector<const TraceReport::Node*>& collapsed,
                      usize depth) {
  u64 ns = 0;
  for (const TraceReport::Node* node : collapsed) {
    ns += node->event.duration_ns;
  }
  const double ms = static_cast<double>(ns) / 1.0e6;
  std::string indent(depth * 2, ' ');
  fmt::format_to(std::back_inserter(out), "{}\u2026 {} more, {:>8.3} ms\n",
                 indent, collapsed.size(), ms);
}

void append_node(std::string& out,
                 const debug::Profiler& profiler,
                 const TraceReport::Node& node,
                 u64 wall_ns,
                 usize depth);

void append_children(std::string& out,
                     const debug::Profiler& profiler,
                     const TraceReport::Node& node,
                     u64 wall_ns,
                     usize depth) {
  std::vector<const TraceReport::Node*> shown;
  std::vector<const TraceReport::Node*> collapsed;
  split_children(node, wall_ns, shown, collapsed);
  for (const TraceReport::Node* child : shown) {
    append_node(out, profiler, *child, wall_ns, depth + 1);
  }
  if (!collapsed.empty()) {
    append_collapsed(out, collapsed, depth + 1);
  }
}

void append_node(std::string& out,
                 const debug::Profiler& profiler,
                 const TraceReport::Node& node,
                 u64 wall_ns,
                 usize depth) {
  append_row(out, profiler, node, wall_ns, depth);
  append_children(out, profiler, node, wall_ns, depth);
}

}  // namespace

TraceReport build_trace_report(const TraceCapture& trace) {
  TraceReport report;
  if (trace.events.empty()) {
    return report;
  }
  // Deterministic input: recording order is finish order, which threads
  // decide, so the build starts from start-time order with record order
  // as the tiebreak. Nesting equal intervals by time would read
  // differently run to run; nesting them by record order reads the same
  // way twice.
  std::vector<const debug::ProfileEvent*> ordered;
  ordered.reserve(trace.events.size());
  for (const debug::ProfileEvent& event : trace.events) {
    ordered.push_back(&event);
  }
  std::stable_sort(
      ordered.begin(), ordered.end(),
      [](const debug::ProfileEvent* a, const debug::ProfileEvent* b) {
        return a->start_time_ns < b->start_time_ns;
      });
  // Each event nests under the smallest interval containing it, and an
  // event no interval contains is a root. Smallest first is what puts a
  // pass inside the pass that ran it rather than inside the whole run:
  // the first container found walking up from the innermost is the
  // parent, and walking outward again never unfinds it.
  std::vector<TraceReport::Node> nodes;
  nodes.reserve(ordered.size());
  std::vector<int> parent(ordered.size(), -1);
  std::vector<u64> span(ordered.size(), 0);
  for (usize i = 0; i < ordered.size(); ++i) {
    span[i] = ordered[i]->duration_ns;
  }
  for (usize i = 0; i < ordered.size(); ++i) {
    int best = -1;
    for (usize j = 0; j < ordered.size(); ++j) {
      if (i == j || !contains(*ordered[j], *ordered[i])) {
        continue;
      }
      // Equal spans contain each other, and nothing in the events says
      // which wrapped which. They read as siblings, which is also the
      // rule that keeps a threaded region's shape independent of the
      // schedule.
      if (span[j] == span[i]) {
        continue;
      }
      if (best < 0 || span[j] < span[best]) {
        best = static_cast<int>(j);
      }
    }
    parent[i] = best;
  }
  for (const debug::ProfileEvent* event : ordered) {
    TraceReport::Node node;
    node.event = *event;
    node.thread_id = event->thread_id;
    node.process_id = event->process_id;
    nodes.push_back(std::move(node));
  }
  std::vector<std::vector<int>> children(ordered.size());
  for (usize i = 0; i < ordered.size(); ++i) {
    if (parent[i] >= 0) {
      children[static_cast<usize>(parent[i])].push_back(static_cast<int>(i));
    }
  }
  // The nodes vector is in start-time order, so linking children in that
  // order keeps every level a timeline without sorting anything twice.
  std::function<void(TraceReport::Node&, int)> link =
      [&](TraceReport::Node& node, int index) {
        for (int child : children[static_cast<usize>(index)]) {
          TraceReport::Node subtree =
              std::move(nodes[static_cast<usize>(child)]);
          link(subtree, child);
          node.children.push_back(std::move(subtree));
        }
      };
  for (usize i = 0; i < ordered.size(); ++i) {
    if (parent[i] < 0) {
      TraceReport::Node root = std::move(nodes[i]);
      link(root, static_cast<int>(i));
      report.roots.push_back(std::move(root));
    }
  }
  // The wall clock is the run from its earliest start to its latest end,
  // which is what every share answers against - including two regions
  // that held the same wall time on two threads.
  u64 first = ordered.front()->start_time_ns;
  u64 last = first;
  for (const debug::ProfileEvent* event : ordered) {
    last = std::max(last, end_ns(*event));
  }
  report.wall_ns = last - first;
  return report;
}

std::string render_trace_text(const TraceReport& report,
                              const debug::Profiler& profiler) {
  std::string out;
  for (const TraceReport::Node& root : report.roots) {
    append_node(out, profiler, root, report.wall_ns, 0);
  }
  return out;
}

}  // namespace cli
