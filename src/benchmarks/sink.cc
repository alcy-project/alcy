// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/sink.h"

#include <string>
#include <string_view>

#include "benchmarks/runner.h"
#include "benchmarks/statistics.h"
#include "fpag/base/numeric.h"
#include "text/json.h"

namespace bench {

namespace {

using text::append_json_number;
using text::append_json_string;

void append_stats(std::string& out, const Stats& stats) {
  out += R"("stats":{)";
  out += R"("count":)";
  append_json_number(out, stats.count);
  out += R"(,"min":)";
  append_json_number(out, stats.min);
  out += R"(,"p50":)";
  append_json_number(out, stats.p50);
  out += R"(,"p95":)";
  append_json_number(out, stats.p95);
  out += R"(,"max":)";
  append_json_number(out, stats.max);
  out += R"(,"mean":)";
  // A fractional figure, so it is written as one rather than truncated.
  append_json_number(out, stats.mean);
  out += R"(,"stddev":)";
  append_json_number(out, stats.stddev);
  out += '}';
}

}  // namespace

void ResultWriter::write(CaseId id,
                         const MeasurementPolicy& policy,
                         const Measurement& measurement) {
  std::string& out = out_;
  out += R"({"schema_version":)";
  append_json_number(out, SCHEMA_VERSION);
  out += R"(,"benchmark_id":)";
  append_json_string(out, std::string(id.group) + "/" + std::string(id.name));
  out += R"(,"kind":"micro","command":[],"group":)";
  append_json_string(out, id.group);
  out += R"(,"name":)";
  append_json_string(out, id.name);
  out += R"(,"fixture_digest":)";
  append_json_string(out, metadata_.fixture_digest);
  // The samples are per operation, so a batched case is divided back out
  // here rather than leaving the reader to notice.
  out += R"(,"sample_kind":)";
  append_json_string(out, measurement.batch_size > 1 ? "batch" : "operation");
  out += R"(,"samples_ns":[)";
  for (usize i = 0; i < measurement.samples_ns.size(); ++i) {
    if (i > 0) {
      out += ',';
    }
    append_json_number(out, measurement.samples_ns[i]);
  }
  out += "],";
  append_stats(out, measurement.stats);
  out += R"(,"policy":{)";
  out += R"("warmup":)";
  append_json_number(out, measurement.warmup);
  out += R"(,"requested_samples":)";
  append_json_number(out, policy.samples);
  out += R"(,"min_duration_ns":)";
  append_json_number(out, policy.min_duration_ns);
  out += R"(,"min_batch_ns":)";
  append_json_number(out, policy.min_batch_ns);
  out += R"(,"batch_size":)";
  append_json_number(out, measurement.batch_size);
  out += '}';
  out += R"(,"confidence":)";
  append_json_string(out,
                     measurement.confidence == Confidence::Ok ? "ok" : "low");
  out += R"(,"metadata":{)";
  out += R"("target":)";
  append_json_string(out, metadata_.target);
  out += R"(,"build_subdir":)";
  append_json_string(out, metadata_.build_subdir);
  out += R"(,"build_mode":)";
  append_json_string(out, metadata_.build_mode);
  out += R"(,"clock":)";
  append_json_string(out, metadata_.clock);
  out += R"(,"clock_resolution_ns":)";
  append_json_number(out, metadata_.clock_resolution_ns);
  out += R"(,"clock_overhead_ns":)";
  append_json_number(out, metadata_.clock_overhead_ns);
  out += "}}\n";
}

}  // namespace bench
