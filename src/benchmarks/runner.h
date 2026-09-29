// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "benchmarks/statistics.h"
#include "fpag/base/numeric.h"

namespace bench {

// What a case is called. The names are the pipeline's own phase names, so
// a case and a `--time-trace` run describe the same work from two angles
// and can be checked against each other.
struct CaseId {
  std::string_view group;
  std::string_view name;

  constexpr bool operator==(const CaseId&) const = default;
};

// Selects cases by name. A group takes no part in it: two groups may
// both have a `parse`, and a record is identified by its full id, so a
// name that appears in any group selects it there.
struct CaseFilter {
  std::string_view names;

  bool wants(CaseId id) const {
    return names.empty() || names.find(id.name) != std::string_view::npos;
  }
};

// How long to run a case, and how much of each sample to trust.
//
// Sample count is not fixed across cases: a lexer's token and a linker's
// link differ by orders of magnitude, and a count that suits one makes
// the other either instant or needlessly long. A case sets a count, a
// duration, or both, and the runner goes until both are satisfied.
struct MeasurementPolicy {
  u32 warmup = 0;
  // Zero means the duration below decides instead.
  u32 samples = 0;
  // Zero means the count above decides instead.
  u64 min_duration_ns = 0;
  // A single operation shorter than this is repeated until the batch
  // reaches it. The clock costs tens of nanoseconds, so an operation
  // under the clock's own cost measures the clock.
  u64 min_batch_ns = 50000;
};

// Whether enough was collected to say anything. A percentile over a
// handful of samples is a claim about noise.
enum class Confidence : u8 {
  Ok,
  Low,
};

// One case's outcome, before it is written.
struct Measurement {
  std::vector<u64> samples_ns;
  // Operations per sample, so a batched number can be divided back out.
  u32 batch_size = 1;
  u32 warmup = 0;
  Confidence confidence = Confidence::Low;
  Stats stats;
};

// Whether a sample count supports a percentile.
Confidence classify(u32 samples) noexcept;

// Runs cases against a clock policy. The clock is a template parameter
// rather than a stored object: the measurement loop has to inline the
// read, and a test has to supply one that answers from a script.
template <typename Clock>
class Runner {
 public:
  explicit Runner(Clock clock) : clock_(clock) {}

  // Measures `run` under `policy`, timing only the calls to it.
  //
  // `prepare` leaves the work in a state `run` can repeat, and runs
  // before every sample rather than once: a case whose work consumes
  // state it cannot re-create has to hand that back between samples, and
  // the cost of doing so is setup the benchmark does not claim to
  // measure. `run` is the measurement.
  //
  // Batching and per-sample setup are exclusive. Within one batch the
  // state is shared, so a case that consumes it must ask for no batch:
  // `min_batch_ns = 0` says the operation is long enough on its own.
  template <typename Prepare, typename Run>
  Measurement measure(const MeasurementPolicy& policy,
                      Prepare&& prepare,
                      Run&& run) {
    for (u32 i = 0; i < policy.warmup; ++i) {
      prepare();
      run();
    }
    const u32 batch = find_batch(policy, prepare, run);

    Measurement measurement;
    measurement.batch_size = batch;
    measurement.warmup = policy.warmup;
    measurement.samples_ns.reserve(policy.samples > 0 ? policy.samples : 1024);
    u64 collected = 0;
    while (true) {
      prepare();
      const u64 started = clock_.now_ns();
      for (u32 i = 0; i < batch; ++i) {
        run();
      }
      const u64 elapsed = clock_.now_ns() - started;
      measurement.samples_ns.push_back(elapsed / batch);
      collected += elapsed;
      const bool enough_samples =
          policy.samples == 0 ||
          measurement.samples_ns.size() >= policy.samples;
      const bool long_enough = collected >= policy.min_duration_ns;
      // Either rule alone decides; with both set, the later one waits.
      if (enough_samples && long_enough) {
        break;
      }
    }
    measurement.confidence = classify(measurement.samples_ns.size());
    measurement.stats = summarize(measurement.samples_ns);
    return measurement;
  }

 private:
  // Doubles the batch until one sample is long enough to time. The
  // search is part of setup: it runs before any sample is kept, and its
  // own readings are discarded.
  template <typename Prepare, typename Run>
  u32 find_batch(const MeasurementPolicy& policy, Prepare& prepare, Run& run) {
    if (policy.min_batch_ns == 0) {
      return 1;
    }
    u32 batch = 1;
    while (true) {
      prepare();
      const u64 started = clock_.now_ns();
      for (u32 i = 0; i < batch; ++i) {
        run();
      }
      const u64 elapsed = clock_.now_ns() - started;
      if (elapsed >= policy.min_batch_ns || batch >= 1u << 20) {
        return batch;
      }
      batch *= 2;
    }
  }

  Clock clock_;
};

}  // namespace bench
