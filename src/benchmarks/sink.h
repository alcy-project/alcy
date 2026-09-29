// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "benchmarks/runner.h"
#include "fpag/base/numeric.h"

namespace bench {

// Schema version of a record, so a later change to the format is
// detectable rather than silent. Shared with the process runner's
// records: the envelope is the same and `kind` tells them apart.
inline constexpr u32 SCHEMA_VERSION = 1;

// What every record of one run says about the run, so a number can be
// traced to the conditions it was taken under. Absolute figures are only
// comparable within one host, one build, and one fixture.
struct RunMetadata {
  std::string_view target;
  std::string_view build_subdir;
  // Debug or release. A debug build carries assertions and sanitizers,
  // so its figures describe that build rather than the compiler, and a
  // consumer that forgets to check is reading noise.
  std::string_view build_mode;
  // Digest of the input the cases were generated from, or of the suite
  // they were read from.
  std::string_view fixture_digest;
  std::string_view clock;
  u64 clock_resolution_ns = 0;
  u64 clock_overhead_ns = 0;
};

// Collects one case per line. The lines go to a file or to standard
// output, which is what the comparison in `tools/run_benchmarks.py`
// reads.
class ResultWriter {
 public:
  explicit ResultWriter(RunMetadata metadata) : metadata_(metadata) {}

  void write(CaseId id,
             const MeasurementPolicy& policy,
             const Measurement& measurement);

  const std::string& text() const { return out_; }

 private:
  RunMetadata metadata_;
  std::string out_;
};

// What a case calls once it has measured. A plain call rather than a
// callback through an interface: the harness has no subclasses to choose
// between, and the cases are templates so the call inlines.
struct Emitter {
  ResultWriter* writer = nullptr;
  // How many cases actually ran, so a filter that matched nothing is a
  // complaint rather than an empty file.
  mutable u32 count = 0;

  void operator()(CaseId id,
                  const MeasurementPolicy& policy,
                  const Measurement& measurement) const {
    if (writer == nullptr) {
      return;
    }
    ++count;
    writer->write(id, policy, measurement);
  }
};

}  // namespace bench
