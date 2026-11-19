// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/build_progress.h"

#include <string>
#include <string_view>

#include "cli/duration.h"
#include "cli/output.h"
#include "i18n/messages.h"
#include "pipeline/build.h"

namespace cli {

void BuildProgress::begin(std::string_view name,
                          usize count,
                          bool suite) const {
  if (!enabled_) {
    return;
  }
  std::string subject;
  subject += suite
                 ? i18n::text<i18n::Key::CliSuiteWord>(options_.language)
                 : i18n::text<i18n::Key::CliPackageSingular>(options_.language);
  subject += ' ';
  subject += name;
  subject += " (";
  subject += std::to_string(count);
  subject += ' ';
  if (suite) {
    subject +=
        count == 1
            ? i18n::text<i18n::Key::CliPackageSingular>(options_.language)
            : i18n::text<i18n::Key::CliPackagePlural>(options_.language);
  } else {
    subject += count == 1
                   ? i18n::text<i18n::Key::CliTargetSingular>(options_.language)
                   : i18n::text<i18n::Key::CliTargetPlural>(options_.language);
  }
  subject += ')';
  std::string line;
  render_labelled(line,
                  i18n::text<i18n::Key::CliVerbBuilding>(options_.language),
                  subject, true, options_.color, true);
  err_.block(line);
}

void BuildProgress::member_done(std::string_view member,
                                u64 duration_ns,
                                bool cached) const {
  if (!enabled_ || cached) {
    return;
  }
  std::string body = std::string(member);
  body += " (";
  body += format_duration(duration_ns);
  body += ')';
  std::string line;
  if (options_.color) {
    append_dim(line, "  " + body);
  } else {
    line = "  " + body;
  }
  line += '\n';
  err_.block(line);
}

namespace {

void begin_fn(void* ctx, std::string_view name, usize count, bool suite) {
  static_cast<const BuildProgress*>(ctx)->begin(name, count, suite);
}

void member_done_fn(void* ctx,
                    std::string_view member,
                    u64 duration_ns,
                    bool cached) {
  static_cast<const BuildProgress*>(ctx)->member_done(member, duration_ns,
                                                      cached);
}

}  // namespace

pipeline::BuildProgressObserver build_progress_observer(
    const BuildProgress& progress) {
  return pipeline::BuildProgressObserver{
      .ctx = const_cast<BuildProgress*>(&progress),
      .begin = &begin_fn,
      .member_done = &member_done_fn,
  };
}

}  // namespace cli
