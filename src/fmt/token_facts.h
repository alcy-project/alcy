// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cst/cst.h"
#include "fpag/base/numeric.h"
#include "lexer/token.h"

namespace alcy_fmt::token_facts {

// The per-node facts a flow records while it spaces its pieces: whether a
// bracket reads as a generic delimiter, and whether an operator reads as a
// prefix one. One byte holds both because a node asks for them together.
inline constexpr u8 NODE_GENERIC = 1;
inline constexpr u8 NODE_UNARY = 2;

std::string_view node_text(std::string_view source, const cst::Node& node);
std::string_view error_text(std::string_view source, const cst::Node& node);
bool error_absorbs(std::string_view text);
bool tokens_merge(std::string_view prev, std::string_view next);
bool dot_joins_number(std::string_view after);
bool is_path_like(lexer::TokenKind kind);
bool is_prefix_operator(lexer::TokenKind kind);
bool allows_prefix_after(lexer::TokenKind kind);
bool is_break_after(lexer::TokenKind kind);

bool space_before(const cst::Node* prev,
                  bool prev_generic,
                  bool prev_unary,
                  const cst::Node& cur,
                  bool cur_generic);
void compute_generic(const std::vector<const cst::Node*>& nodes,
                     std::vector<u8>& flags);
bool looks_like_generic(const std::vector<const cst::Node*>& nodes, usize i);
const cst::Node* previous_significant(
    const std::vector<const cst::Node*>& nodes,
    usize i);
const cst::Node* next_significant(const std::vector<const cst::Node*>& nodes,
                                  usize i);

}  // namespace alcy_fmt::token_facts

