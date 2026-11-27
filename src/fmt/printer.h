// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cst/cst.h"
#include "fmt/doc.h"
#include "fmt/formatter.h"
#include "fpag/base/numeric.h"

namespace alcy_fmt {

// Turns a CST into a layout document. The printer owns no state between
// runs: every decision comes from the tree and the injected config. The
// implementation is split by concern across printer.cc, printer_flow.cc
// and printer_chunks.cc.
class Printer {
 public:
  Printer(std::string_view source, const FormatConfig& config);

  Doc run(const cst::Node& root);

 private:
  enum class Sep : u8 { None, Space, Line, SoftLine };

  struct Piece {
    Doc doc;
    Sep sep = Sep::None;
  };

  struct Item {
    std::vector<const cst::Node*> nodes;
  };

  struct Chunk {
    std::vector<const cst::Node*> nodes;
    bool is_comment = false;
    usize newlines_before = 0;
    // The chunk ended at a semicolon spelled in the source rather than
    // at the newline the lexer turned into one.
    bool explicit_semi = false;
    // Where the whitespace before a comment that rode the line a
    // separator finished starts: after the separator itself, whose
    // bytes the terminator already wrote.
    bool has_riding_gap = false;
    usize riding_gap_start = 0;
  };

  std::vector<Chunk> collect_chunks(const cst::Node& seq,
                                    cst::BraceRole role) const;
  Doc chunk_line(const Chunk& chunk, Doc terminator);
  static usize trailing_eol_start(const Chunk& chunk);
  static bool ends_in_block(const std::vector<const cst::Node*>& code);
  static bool keeps_semicolon(const Chunk& chunk,
                              const std::vector<const cst::Node*>& code,
                              cst::BraceRole role,
                              bool last_chunk,
                              bool next_suppresses);
  static bool chunk_is_semi_terminated_item(const Chunk& chunk);
  static bool chunk_starts_with_suppressor(const Chunk& chunk);
  Doc print_body_lines(const std::vector<Chunk>& chunks,
                       cst::BraceRole role,
                       bool top_level);
  Doc chunk_terminator(const Chunk& chunk,
                       const std::vector<const cst::Node*>& code,
                       cst::BraceRole role,
                       bool last_chunk,
                       bool next_suppresses);
  static bool ends_like_statement(const std::vector<const cst::Node*>& nodes);
  Doc print_line(const std::vector<const cst::Node*>& nodes);
  Doc print_flow(const std::vector<const cst::Node*>& nodes, bool allow_wrap);
  Doc build_flow(std::vector<Piece> pieces, bool allow_wrap);
  Doc node_doc(const cst::Node& node);
  Doc print_group(const cst::Node& group);
  Doc print_brace_body(const cst::Node& group);
  Doc print_list(const cst::Node& group,
                 std::string_view open,
                 std::string_view close);
  Doc print_literal(const cst::Node& group);
  std::vector<Item> split_on_commas(const cst::Node& seq,
                                    bool& had_comma,
                                    bool& trailing) const;
  static bool chunk_is_use(const Chunk& chunk);
  static bool chunk_is_const(const Chunk& chunk);
  static bool chunk_is_attribute(const Chunk& chunk);

  std::string_view source_;
  FormatConfig config_;
};

}  // namespace alcy_fmt
