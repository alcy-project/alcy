// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cmath>
#include <vector>

#include "benchmarks/runner.h"
#include "benchmarks/statistics.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"

namespace bench {

namespace {

// A clock that answers from a script instead of the host, so a duration
// is a fact about the test rather than about how fast the machine is.
//
// The state is held apart from the clock and shared, because a case has
// to advance it from inside the timed region: the runner owns its own
// copy of the clock, and a case that reached a different one would have
// its work charged to a clock nobody reads. A real case never needs this
// — it runs compiler code and lets the runner do the reading — which is
// why the state is a separate thing rather than a counter on the clock.
struct Script {
  u64 now = 0;
  u64 read_ns = 0;
  u64 work_ns = 0;
};

struct ScriptedClock {
  Script* script = nullptr;

  u64 now_ns() {
    script->now += script->read_ns;
    return script->now;
  }
};

}  // namespace

// The expected values below are what Python's `statistics` produces for
// the same samples: median for p50 and `quantiles(method="inclusive")`
// for p95. Pinning them here is what keeps this implementation and the
// process runner's from becoming two different statistics that share a
// name.
TEST_CASE("Summarize matches the process runner's percentile rule") {
  const std::array<u64, 5> odd{10, 20, 30, 40, 50};
  const Stats odd_stats = summarize(odd);
  CHECK(odd_stats.count == 5);
  CHECK(odd_stats.min == 10);
  CHECK(odd_stats.max == 50);
  CHECK(odd_stats.p50 == 30.0);
  CHECK(odd_stats.p95 == 48.0);
  CHECK(std::abs(odd_stats.mean - 30.0) < 1e-9);
  CHECK(std::abs(odd_stats.stddev - 14.1421356) < 1e-6);

  // An even count interpolates, which is what makes this the same
  // median Python's statistics.median reports, fractional result and
  // all: truncating it here would make this a different statistic.
  const std::array<u64, 4> even{10, 20, 30, 40};
  const Stats even_stats = summarize(even);
  CHECK(even_stats.p50 == 25.0);
  CHECK(even_stats.p95 == 38.5);
  CHECK(std::abs(even_stats.mean - 25.0) < 1e-9);
  CHECK(std::abs(even_stats.stddev - 11.1803399) < 1e-6);
}

TEST_CASE("Summarize answers for a degenerate sample set") {
  const Stats empty = summarize({});
  CHECK(empty.count == 0);
  CHECK(empty.p50 == 0.0);

  const std::array<u64, 1> single{42};
  const Stats one = summarize(single);
  CHECK(one.count == 1);
  CHECK(one.min == 42);
  CHECK(one.max == 42);
  CHECK(one.p50 == 42.0);
  CHECK(one.p95 == 42.0);
  CHECK(std::abs(one.stddev) < 1e-9);
}

TEST_CASE("Summarize does not depend on the order it is given") {
  const std::array<u64, 5> ordered{10, 20, 30, 40, 50};
  const std::array<u64, 5> shuffled{40, 10, 50, 20, 30};
  const Stats a = summarize(ordered);
  const Stats b = summarize(shuffled);
  CHECK(a.p50 == b.p50);
  CHECK(a.p95 == b.p95);
  CHECK(a.mean == b.mean);
  CHECK(a.stddev == b.stddev);
}

TEST_CASE("Confidence turns on sample count") {
  CHECK(classify(0) == Confidence::Low);
  CHECK(classify(29) == Confidence::Low);
  CHECK(classify(30) == Confidence::Ok);
}

TEST_CASE("Runner reports a scripted duration") {
  Script script;
  script.read_ns = 100;
  Runner<ScriptedClock> runner(ScriptedClock{&script});
  const MeasurementPolicy policy{
      .warmup = 2,
      .samples = 5,
      .min_duration_ns = 0,
      .min_batch_ns = 0,
  };
  u32 ran = 0;
  const Measurement measured = runner.measure(policy, [] {}, [&] { ++ran; });
  // Two reads bracket the sample and nothing is charged, so a sample is
  // exactly one read's worth: the clock, not the work.
  CHECK(measured.samples_ns.size() == 5);
  CHECK(measured.samples_ns[0] == 100);
  CHECK(measured.batch_size == 1);
  CHECK(ran == 7);
  CHECK(measured.confidence == Confidence::Low);
  CHECK(measured.stats.p50 == 100.0);
}

TEST_CASE("Runner runs until the duration is met") {
  Script script;
  script.read_ns = 100;
  Runner<ScriptedClock> runner(ScriptedClock{&script});
  // No sample count, so the duration decides: each sample contributes
  // 100ns, and 1000ns wants ten of them.
  const MeasurementPolicy policy{
      .warmup = 0,
      .samples = 0,
      .min_duration_ns = 1000,
      .min_batch_ns = 0,
  };
  const Measurement measured = runner.measure(policy, [] {}, [] {});
  CHECK(measured.samples_ns.size() == 10);
}

TEST_CASE("Runner batches an operation shorter than the clock") {
  Script script;
  script.read_ns = 10;
  script.work_ns = 2;
  Runner<ScriptedClock> runner(ScriptedClock{&script});
  const MeasurementPolicy policy{
      .warmup = 0,
      .samples = 4,
      .min_duration_ns = 0,
      .min_batch_ns = 20,
  };
  const Measurement measured =
      runner.measure(policy, [] {}, [&] { script.now += script.work_ns; });

  // What the runner's own interval would hold: the closing read plus one
  // operation per repetition. The contract is the smallest power of two
  // that reaches the floor, so this recomputes it rather than pinning a
  // constant that would only agree by accident.
  const auto scripted = [&](u64 operations) {
    return script.read_ns + operations * script.work_ns;
  };
  CHECK((measured.batch_size & (measured.batch_size - 1)) == 0);
  CHECK(scripted(measured.batch_size) >= policy.min_batch_ns);
  if (measured.batch_size > 1) {
    CHECK(scripted(measured.batch_size / 2) < policy.min_batch_ns);
  }
  // The sample is per operation, so the batch divides back out.
  CHECK(measured.samples_ns[0] ==
        scripted(measured.batch_size) / measured.batch_size);
  CHECK(measured.samples_ns.size() == 4);
  CHECK(measured.confidence == Confidence::Low);
}

TEST_CASE("Runner keeps setup out of the sample") {
  Script script;
  script.read_ns = 10;
  script.work_ns = 40;
  Runner<ScriptedClock> runner(ScriptedClock{&script});
  // A per-sample setup that costs real time, which must not reach the
  // measurement: that is the whole reason prepare exists.
  const MeasurementPolicy policy{
      .warmup = 0,
      .samples = 2,
      .min_duration_ns = 0,
      .min_batch_ns = 0,
  };
  const Measurement measured = runner.measure(
      policy, [&] { script.now += 1000 * script.work_ns; },
      [&] { script.now += script.work_ns; });
  // The interval runs from the first read to the second, so what it
  // holds is the closing read and the operation, and none of the setup.
  CHECK(measured.samples_ns[0] == 50);
  CHECK(measured.samples_ns[1] == 50);
}

}  // namespace bench
