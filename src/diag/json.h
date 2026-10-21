// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "i18n/language.h"

namespace source {
class SourceManager;
}  // namespace source

namespace diag {

// The note a bag that dropped diagnostics gets: how many were lost and
// why, in the reader's language.
[[nodiscard]] std::string dropped_diagnostics_note(u32 dropped,
                                                   i18n::Language language);

// Appends one diagnostic as the JSON object the `--json` envelope's
// "diagnostics" array carries. `sources` resolves the span's file name; a
// null manager writes every span as null, which is what a diagnostic
// outside a file wants.
void append_diagnostic_json(std::string& out,
                            const Diagnostic& diagnostic,
                            const source::SourceManager* sources);

// Appends the whole array: every diagnostic in `bag`, the dropped note
// when the bag overflowed, and `extra` last when the caller has one more
// failure to report (the CLI's envelope does). This is the array the CLI
// writes, for a host that has no envelope to put around it.
void append_diagnostics_json(std::string& out,
                             const DiagBag* bag,
                             const source::SourceManager* sources,
                             i18n::Language language,
                             const Diagnostic* extra = nullptr);

}  // namespace diag
