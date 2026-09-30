// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/output.h"

#include <string>
#include <utility>

#include "diag/render.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "i18n/language.h"

namespace cli {

namespace {

Envelope built(std::string path, u64 bytes, u64 wall_ns) {
  Envelope envelope;
  envelope.command = "build";
  envelope.status = Status::Ok;
  envelope.outcome = Outcome::Built;
  envelope.output_path = std::move(path);
  envelope.output_bytes = bytes;
  envelope.wall_ns = wall_ns;
  return envelope;
}

std::string text(const Envelope& envelope, bool color = false) {
  return render_text(envelope, diag::RenderOptions{.color = color});
}

// The column the subject starts at, which is the point of padding the
// verb at all. Read from the rendered line rather than from the layout
// code, so a change to either shows up here.
usize subject_column(const std::string& line) {
  return line.find_first_not_of(' ');
}

}  // namespace

TEST_CASE("Result lines start their subject in one column") {
  const Envelope compiled = [] {
    Envelope envelope;
    envelope.command = "compile";
    envelope.status = Status::Ok;
    envelope.outcome = Outcome::Compiled;
    envelope.output_path = "main";
    envelope.output_bytes = 1;
    envelope.wall_ns = 1;
    return envelope;
  }();

  Envelope checked;
  checked.command = "check";
  checked.status = Status::Ok;
  checked.outcome = Outcome::Checked;
  checked.file_count = 1;

  const std::string built_line = text(built("out/demo", 1, 1));
  const std::string compiled_line = text(compiled);
  const std::string checked_line = text(checked);

  CHECK(subject_column(built_line) == subject_column(compiled_line));
  CHECK(subject_column(built_line) == subject_column(checked_line));
  CHECK(built_line.starts_with("Built"));
  CHECK(compiled_line.starts_with("Compiled"));
  CHECK(checked_line.starts_with("Checked"));
}

TEST_CASE("A run reports no result line, having announced itself already") {
  // The announcement carries the name, and it is printed before the
  // program's own output. A second line afterwards would sit below that
  // output and read as more of it.
  Envelope ran;
  ran.command = "run";
  ran.status = Status::Ok;
  ran.outcome = Outcome::Ran;
  ran.output_path = "demo";
  ran.wall_ns = 30ull * 1000ull * 1000ull * 1000ull;

  CHECK(text(ran).empty());
  // The outcome still says what happened, for a machine: only the
  // human-facing line is gone.
  CHECK(render_json(ran, i18n::Language::EnUs).find("\"outcome\":\"ran\"") !=
        std::string::npos);
}

TEST_CASE("A scaffold reports its own shape, not a padded column") {
  // `Created package` is longer than the column and says something
  // different from a build, so it is not padded to match.
  Envelope envelope;
  envelope.command = "new";
  envelope.status = Status::Ok;
  envelope.outcome = Outcome::CreatedPackage;
  envelope.package_name = "demo";
  envelope.package_dir = "demo";
  envelope.wall_ns = 1000;

  const std::string line = text(envelope);
  CHECK(line == "Created package 'demo' in demo  (1 us)\n");
}

TEST_CASE("A check pluralizes its counts") {
  Envelope one;
  one.command = "check";
  one.status = Status::Ok;
  one.outcome = Outcome::Checked;
  one.file_count = 1;
  one.module_count = 1;
  one.function_count = 1;

  Envelope two;
  two.command = "check";
  two.status = Status::Ok;
  two.outcome = Outcome::Checked;
  two.file_count = 2;
  two.module_count = 3;
  two.function_count = 4;

  CHECK(text(one).starts_with("Checked   1 file, 1 module, 1 function  ("));
  CHECK(text(two).starts_with("Checked   2 files, 3 modules, 4 functions  ("));
}

TEST_CASE("A size is counted below a kibibyte and scaled above it") {
  // Whole microseconds make the expected strings readable; the scaling
  // is what is under test, not the precision.
  constexpr u64 US = 1000;
  CHECK(text(built("out/a", 0, US)).find("(1 us)") != std::string::npos);
  CHECK(text(built("out/a", 1, US)).find("(1 byte, 1 us)") !=
        std::string::npos);
  CHECK(text(built("out/a", 1023, US)).find("(1023 bytes, 1 us)") !=
        std::string::npos);
  CHECK(text(built("out/a", 1024, US)).find("(1.0 KiB, 1 us)") !=
        std::string::npos);
  constexpr u64 MIB = static_cast<u64>(1024) * 1024;
  CHECK(text(built("out/a", MIB, US)).find("(1.0 MiB, 1 us)") !=
        std::string::npos);
}

TEST_CASE("A failure with no diagnostic prints its own message") {
  Envelope envelope;
  envelope.command = "build";
  envelope.status = Status::Error;
  envelope.failure = "--emit needs a value";

  CHECK(text(envelope) == "--emit needs a value\n");
  CHECK(render_json(envelope, i18n::Language::EnUs)
            .find("\"status\":\"error\"") != std::string::npos);
  CHECK(render_json(envelope, i18n::Language::EnUs)
            .find("\"summary\":\"--emit needs a value\"") != std::string::npos);
}

TEST_CASE("A failure with a diagnostic prints no result line") {
  // The diagnostic is the report. A second line saying "Checked" beside
  // an error is a contradiction, and the absence of one is what a reader
  // greps for.
  Envelope envelope;
  envelope.command = "check";
  envelope.status = Status::Error;

  CHECK(text(envelope).empty());
  CHECK(render_json(envelope, i18n::Language::EnUs)
            .find("\"outcome\":\"failed\"") != std::string::npos);
}

TEST_CASE("Colour reaches the verb, the subject, and nothing else") {
  const std::string plain = text(built("out/demo", 2048, 2000000), false);
  const std::string colored = text(built("out/demo", 2048, 2000000), true);

  CHECK(plain.find("\x1b[") == std::string::npos);
  CHECK(colored ==
        "\x1b[1m\x1b[92mBuilt\x1b[0m     "
        "\x1b[4mout/demo\x1b[0m  "
        "\x1b[2m(2.0 KiB, 2.0 ms)\x1b[0m\n");
  // The JSON carries the sentence, not the layout: a consumer wants the
  // text, and a line of escape codes is not text.
  const std::string json =
      render_json(built("out/demo", 2048, 2000000), i18n::Language::EnUs);
  CHECK(json.find("\"summary\":\"Built     out/demo  (2.0 KiB, 2.0 ms)\"") !=
        std::string::npos);
  CHECK(json.find("\"outcome\":\"built\"") != std::string::npos);
  CHECK(json.find("\x1b[") == std::string::npos);
}

}  // namespace cli
