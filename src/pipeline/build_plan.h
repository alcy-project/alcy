// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "pipeline/link_options.h"
#include "pipeline/profile.h"

namespace pipeline {

// The settings one build runs with, resolved once at the command's entry
// and threaded as one value: the profile's switches and what a link would
// be handed. A stage reads the field it needs rather than taking the
// command line apart, and nothing re-derives a setting from another.
struct BuildPlan {
  BuildProfile profile;
  LinkOptions link;
};

}  // namespace pipeline
