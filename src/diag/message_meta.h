// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <array>

#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
// The catalog again, for the metadata its entries carry: a list can be
// included once per adapter, and this is the layer that names the
// severity and the phase.
#include "i18n/messages.def"

namespace diag {

// The severity and the phase a message is reported with: the catalog's
// own facts, so a call site names the message rather than the phase that
// prints it. A message only the cli renders carries `Note` and the
// pipeline phase, which nothing emits and so nothing reads.
struct MessageMeta {
  Severity severity = Severity::Error;
  Stage stage = Stage::Pipeline;
};

// One row per key, in key order, generated from the same list the key
// enum comes from, so the two cannot disagree.
#define ALCY_I18N_META(Name, Sev, Stg, Text) \
  MessageMeta{Severity::Sev, Stage::Stg},
inline constexpr std::array<MessageMeta, i18n::KEY_COUNT> MESSAGE_META = {
    ALCY_I18N_FOREACH_KEY_EN_US(ALCY_I18N_META)
#undef ALCY_I18N_META
};

}  // namespace diag
