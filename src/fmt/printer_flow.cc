// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cst/cst.h"
#include "cst/lex.h"
#include "fmt/doc.h"
#include "fmt/printer.h"
#include "fmt/token_facts.h"
#include "lexer/token.h"

namespace alcy_fmt {

Doc Printer::build_flow(std::vector<Piece> pieces, bool allow_wrap) {
  usize first_break = pieces.size();
  for (usize i = 0; i < pieces.size(); ++i) {
    if (pieces[i].sep == Sep::Line || pieces[i].sep == Sep::SoftLine) {
      first_break = i;
      break;
    }
  }
  if (!allow_wrap || first_break == pieces.size()) {
    std::vector<Doc> out;
    out.reserve(pieces.size() * 2);
    for (Piece& piece : pieces) {
      if (piece.sep == Sep::Space || piece.sep == Sep::Line) {
        out.push_back(doc_text(" "));
      }
      out.push_back(std::move(piece.doc));
    }
    return doc_concat(std::move(out));
  }
  std::vector<Doc> head;
  head.reserve(first_break * 2);
  for (usize i = 0; i < first_break; ++i) {
    if (pieces[i].sep == Sep::Space) {
      head.push_back(doc_text(" "));
    }
    head.push_back(std::move(pieces[i].doc));
  }
  std::vector<Doc> tail;
  tail.reserve((pieces.size() - first_break) * 2);
  for (usize i = first_break; i < pieces.size(); ++i) {
    switch (pieces[i].sep) {
      case Sep::Space: tail.push_back(doc_text(" ")); break;
      case Sep::Line: tail.push_back(doc_line()); break;
      case Sep::SoftLine: tail.push_back(doc_softline()); break;
      case Sep::None: break;
    }
    tail.push_back(std::move(pieces[i].doc));
  }
  std::vector<Doc> out;
  out.reserve(2);
  out.push_back(doc_concat(std::move(head)));
  out.push_back(doc_nest(config_.indent_width, doc_concat(std::move(tail))));
  return doc_group(doc_concat(std::move(out)));
}

namespace {

// Pins a run of source bytes as docs, writing each break the source
// carried as its own hardline. The tail of a group is one of the runs
// print_flow cannot see, and a break dropped there makes the next pass
// read a different tree.
std::vector<Doc> gap_docs(std::string_view gap) {
  std::vector<Doc> out;
  usize run_start = 0;
  for (usize i = 0; i < gap.size(); ++i) {
    if (gap[i] != '\n') {
      continue;
    }
    if (i > run_start) {
      out.push_back(doc_text(gap.substr(run_start, i - run_start)));
    }
    out.push_back(doc_hardline());
    while (i + 1 < gap.size() && (gap[i + 1] == ' ' || gap[i + 1] == '\t')) {
      ++i;
    }
    run_start = i + 1;
  }
  if (run_start < gap.size()) {
    out.push_back(doc_text(gap.substr(run_start, gap.size() - run_start)));
  }
  return out;
}

}  // namespace

Doc Printer::print_flow(const std::vector<const cst::Node*>& nodes,
                        bool allow_wrap) {
  std::vector<u8> flags(nodes.size(), 0);
  token_facts::compute_generic(nodes, flags);
  for (usize i = 0; i < nodes.size(); ++i) {
    const cst::Node& node = *nodes[i];
    if (node.kind == cst::NodeKind::Token &&
        token_facts::is_prefix_operator(node.token)) {
      const cst::Node* prev = token_facts::previous_significant(nodes, i);
      if (prev == nullptr || (prev->kind == cst::NodeKind::Token &&
                              token_facts::allows_prefix_after(prev->token))) {
        flags[i] |= token_facts::NODE_UNARY;
      }
    }
  }
  const auto is_generic = [&](usize i) {
    return (flags[i] & token_facts::NODE_GENERIC) != 0;
  };
  const auto is_unary = [&](usize i) {
    return (flags[i] & token_facts::NODE_UNARY) != 0;
  };
  // Malformed source reaches the printer as Error nodes with gaps pinned
  // verbatim. A later pass reads those gaps as real breaks and can make a
  // group break where the first pass kept it flat, so a dot or an operator
  // in such a flow is not a wrapping point: the layout stays flat and a
  // second pass reads it the same way.
  bool has_error = false;
  for (const cst::Node* node : nodes) {
    if (node->kind == cst::NodeKind::Token &&
        node->token == lexer::TokenKind::Error) {
      has_error = true;
      break;
    }
  }

  std::vector<Piece> pieces;
  pieces.reserve(nodes.size() + 2);
  const cst::Node* prev = nullptr;
  // The node immediately before this one, whatever it is: the gap the
  // source put between two nodes is what an error token's neighbours
  // are pinned against.
  const cst::Node* prev_node = nullptr;
  bool prev_generic = false;
  bool prev_unary = false;
  bool pending_break = false;
  bool suppress = false;

  // Pins gap bytes so a second pass sees the same stream. A break in the
  // gap is a break in the layout too: written as bytes inside one text a
  // group would read the stop as its own line ending and lay the rest of
  // the flow out flat around it, which the next pass, reading real breaks,
  // would not. The blanks a break ends in are the one part of the gap the
  // pin does not keep: the hardline writes the indent where it lands, and
  // the bytes would be counted twice.
  const auto pin_gap_bytes = [&](std::string_view gap) {
    usize run_start = 0;
    for (usize i = 0; i < gap.size(); ++i) {
      if (gap[i] != '\n') {
        continue;
      }
      if (i > run_start) {
        pieces.push_back(
            Piece{doc_text(gap.substr(run_start, i - run_start)), Sep::None});
      }
      pieces.push_back(Piece{doc_hardline(), Sep::None});
      while (i + 1 < gap.size() && (gap[i + 1] == ' ' || gap[i + 1] == '\t')) {
        ++i;
      }
      run_start = i + 1;
    }
    if (run_start < gap.size()) {
      pieces.push_back(Piece{
          doc_text(gap.substr(run_start, gap.size() - run_start)), Sep::None});
    }
  };

  const auto is_error_token = [](const cst::Node* node) {
    return node != nullptr && node->kind == cst::NodeKind::Token &&
           node->token == lexer::TokenKind::Error;
  };
  // The whitespace the source put around an error token, as its own
  // piece. An error's bytes re-lex as whatever the bytes around them
  // make of them -- a lone quote becomes a literal when a space opens
  // room for its closing half -- so the gap is pinned verbatim: it is
  // the one thing that keeps a second pass reading the same stream.
  const auto pin_gap = [&](const cst::Node& from, const cst::Node& to) {
    usize gap_start = from.offset + from.length;
    if (from.kind == cst::NodeKind::Token &&
        from.token == lexer::TokenKind::Error) {
      // The blanks an error span ends in are gap bytes, not span
      // bytes: the span re-absorbs one of them on every pass, so they
      // all ride in the gap and the count stays put.
      gap_start = from.offset + token_facts::error_text(source_, from).size();
    }
    // A semicolon the lexer inserted stands for the break the layout
    // writes itself: the bytes past it are the next line's indent, and
    // pinning them would lay them on top of the indent the break
    // carries, growing the line by the nesting on every pass.
    if (from.kind == cst::NodeKind::Token &&
        from.token == lexer::TokenKind::Semicolon) {
      return;
    }
    if (to.offset > gap_start) {
      pin_gap_bytes(source_.substr(gap_start, to.offset - gap_start));
    }
  };

  for (usize i = 0; i < nodes.size(); ++i) {
    const cst::Node& node = *nodes[i];
    if (node.kind == cst::NodeKind::Comment) {
      Sep sep = Sep::None;
      if (!suppress && !pieces.empty()) {
        sep = Sep::Space;
      }
      if (sep == Sep::Space && is_error_token(prev_node)) {
        pin_gap(*prev_node, node);
        sep = Sep::None;
      }
      Doc text = doc_text(token_facts::node_text(source_, node));
      // A comment that runs to end of line ends the line it sits on, so
      // what follows it has to start a new one. A block comment does
      // not: code may keep reading after it on the same line.
      const bool runs_to_eol = node.comment != cst::CommentKind::Block;
      if (runs_to_eol) {
        std::vector<Doc> parts;
        parts.push_back(std::move(text));
        parts.push_back(doc_hardline());
        text = doc_concat(std::move(parts));
        suppress = true;
      } else {
        suppress = false;
      }
      pieces.push_back(Piece{std::move(text), sep});
      pending_break = false;
      prev_node = &node;
      continue;
    }
    Sep sep = Sep::None;
    const bool near_error = node.kind == cst::NodeKind::Token &&
                            node.token == lexer::TokenKind::Error;
    const bool after_comment_break = suppress;
    // A semicolon the lexer inserted at a newline stands for the break
    // that newline wrote. After a comment that ends its line the break
    // is already out -- the comment's own -- so the semicolon's byte
    // would write the line twice, and the pass over that would count a
    // blank line more than this one did. The piece is nothing instead.
    const bool semi_after_break =
        suppress && node.kind == cst::NodeKind::Token &&
        node.token == lexer::TokenKind::Semicolon && node.synthetic_semi;
    if (pieces.empty()) {
      sep = Sep::None;
    } else if (suppress) {
      sep = Sep::None;
      suppress = false;
    } else if (near_error || is_error_token(prev_node)) {
      if (prev_node != nullptr) {
        pin_gap(*prev_node, node);
      }
      sep = Sep::None;
    } else if (pending_break) {
      sep = allow_wrap && !has_error ? Sep::Line : Sep::Space;
    } else if (node.kind == cst::NodeKind::Token &&
               node.token == lexer::TokenKind::Dot && prev != nullptr &&
               prev->kind == cst::NodeKind::Token &&
               (prev->token == lexer::TokenKind::Integer ||
                prev->token == lexer::TokenKind::Float) &&
               token_facts::dot_joins_number(
                   i + 1 < nodes.size()
                       ? token_facts::node_text(source_, *nodes[i + 1])
                       : std::string_view())) {
      // A dot after a number reads as the number's decimal point when
      // the two touch and the byte past the dot continues a number: a
      // member name does not, a digit and the float suffixes do. The
      // token count of the output would differ from the tree's, so
      // the two never touch there.
      sep = Sep::Space;
    } else if (node.kind == cst::NodeKind::Token &&
               node.token == lexer::TokenKind::Dot &&
               !(prev != nullptr && prev->kind == cst::NodeKind::Token &&
                 (prev->token == lexer::TokenKind::Dot ||
                  prev->token == lexer::TokenKind::DotDot))) {
      // The chain continues on the next line at a member boundary. In
      // the middle of a run of dots it must not: a break there
      // re-pairs the dots into range tokens on the next pass, and the
      // layout moves with the pairing.
      sep = allow_wrap && !has_error ? Sep::SoftLine : Sep::None;
    } else if (token_facts::space_before(prev, prev_generic, prev_unary, node,
                                         is_generic(i))) {
      sep = Sep::Space;
    }
    // Nothing between two tokens can read back as one token, whatever
    // the spacing rules wanted: the merge is checked after them, over
    // the seam the layout actually wrote.
    if (sep == Sep::None && !after_comment_break && !near_error &&
        !is_error_token(prev_node) && prev_node != nullptr &&
        prev_node->kind == cst::NodeKind::Token &&
        node.kind == cst::NodeKind::Token &&
        token_facts::tokens_merge(token_facts::node_text(source_, *prev_node),
                                  token_facts::node_text(source_, node))) {
      sep = Sep::Space;
    }
    pieces.push_back(
        Piece{semi_after_break ? doc_empty() : node_doc(node), sep});
    // A generic bracket reads as a delimiter here, not as a comparison:
    // a break inside an argument list would space it flat.
    pending_break = node.kind == cst::NodeKind::Token && !is_unary(i) &&
                    !is_generic(i) && token_facts::is_break_after(node.token);
    prev = &node;
    prev_node = &node;
    prev_generic = is_generic(i);
    prev_unary = is_unary(i);
  }
  return build_flow(std::move(pieces), allow_wrap);
}

Doc Printer::node_doc(const cst::Node& node) {
  switch (node.kind) {
    case cst::NodeKind::Token:
      if (node.token == lexer::TokenKind::Error) {
        return doc_text(token_facts::error_text(source_, node));
      }
      return doc_text(token_facts::node_text(source_, node));
    case cst::NodeKind::Comment:
      return doc_text(token_facts::node_text(source_, node));
    case cst::NodeKind::Group: return print_group(node);
    case cst::NodeKind::Sequence: return doc_empty();
  }
  return doc_empty();
}

Doc Printer::print_group(const cst::Node& group) {
  switch (group.group_role) {
    case cst::GroupRole::Paren: return print_list(group, "(", ")");
    case cst::GroupRole::Bracket: return print_list(group, "[", "]");
    case cst::GroupRole::Brace:
      if (group.brace_role == cst::BraceRole::Literal) {
        return print_literal(group);
      }
      return print_brace_body(group);
  }
  return doc_empty();
}

Doc Printer::print_brace_body(const cst::Node& group) {
  const std::string_view close = group.has_closer ? "}" : "";
  if (group.kids.empty()) {
    std::vector<Doc> parts;
    parts.reserve(2);
    parts.push_back(doc_text("{"));
    parts.push_back(doc_text(close));
    return doc_concat(std::move(parts));
  }
  const cst::Node& inner = group.kids[0];
  const cst::BraceRole role = group.brace_role;
  const std::vector<Chunk> chunks = collect_chunks(inner, role);

  const bool member_body =
      role == cst::BraceRole::ImplBody || role == cst::BraceRole::SpecBody ||
      role == cst::BraceRole::ExternBody || role == cst::BraceRole::GroupBody;
  if (chunks.empty()) {
    // A member body still opens its lines: an impl that owns nothing
    // reads as the pair of braces the grammar gives it.
    std::vector<Doc> parts;
    parts.reserve(3);
    parts.push_back(doc_text("{"));
    if (member_body) {
      parts.push_back(doc_hardline());
    }
    parts.push_back(doc_text(close));
    return doc_concat(std::move(parts));
  }
  // An expression block holds one expression, and reads like the value
  // it wraps: on the line it started while it fits, over lines when it
  // does not. A comment that ends its line keeps it off the one-line
  // form, since the closer would land inside the comment.
  if (role == cst::BraceRole::ExprBlock && chunks.size() == 1 &&
      !chunks[0].is_comment &&
      trailing_eol_start(chunks[0]) == chunks[0].nodes.size() &&
      chunks[0].newlines_before < 2) {
    const usize end = trailing_eol_start(chunks[0]);
    std::vector<const cst::Node*> code(
        chunks[0].nodes.begin(),
        chunks[0].nodes.begin() + static_cast<isize>(end));
    std::vector<Doc> inner_parts;
    inner_parts.reserve(2);
    inner_parts.push_back(doc_line());
    inner_parts.push_back(chunk_line(
        chunks[0], chunk_terminator(chunks[0], code, role, true, false)));
    std::vector<Doc> parts;
    parts.reserve(4);
    parts.push_back(doc_text("{"));
    parts.push_back(
        doc_nest(config_.indent_width, doc_concat(std::move(inner_parts))));
    parts.push_back(doc_line());
    parts.push_back(doc_text(close));
    return doc_group(doc_concat(std::move(parts)));
  }
  Doc body = print_body_lines(chunks, role, false);
  std::vector<Doc> parts;
  parts.reserve(4);
  parts.push_back(doc_text("{"));
  std::vector<Doc> inner_parts;
  inner_parts.reserve(2);
  inner_parts.push_back(doc_hardline());
  inner_parts.push_back(std::move(body));
  parts.push_back(
      doc_nest(config_.indent_width, doc_concat(std::move(inner_parts))));
  parts.push_back(doc_hardline());
  parts.push_back(doc_text(close));
  return doc_concat(std::move(parts));
}

std::vector<Printer::Item> Printer::split_on_commas(const cst::Node& seq,
                                                    bool& had_comma,
                                                    bool& trailing) const {
  std::vector<Item> items;
  items.reserve(seq.kids.size() / 2 + 1);
  Item current;
  had_comma = false;
  trailing = false;
  for (const cst::Node& node : seq.kids) {
    if (node.kind == cst::NodeKind::Token &&
        node.token == lexer::TokenKind::Comma) {
      had_comma = true;
      items.push_back(std::move(current));
      current = Item{};
      continue;
    }
    current.nodes.push_back(&node);
  }
  items.push_back(std::move(current));
  if (had_comma && items.back().nodes.empty()) {
    trailing = true;
    items.pop_back();
  }
  return items;
}

Doc Printer::print_list(const cst::Node& group,
                        std::string_view open,
                        std::string_view close) {
  if (group.kids.empty()) {
    std::vector<Doc> parts;
    parts.reserve(2);
    parts.push_back(doc_text(open));
    parts.push_back(doc_text(close));
    return doc_concat(std::move(parts));
  }
  const cst::Node& inner = group.kids[0];
  bool had_comma = false;
  bool trailing = false;
  std::vector<Item> items = split_on_commas(inner, had_comma, trailing);
  if (!group.has_closer) {
    std::vector<const cst::Node*> nodes;
    nodes.reserve(inner.kids.size());
    for (const cst::Node& node : inner.kids) {
      nodes.push_back(&node);
    }
    std::vector<Doc> parts;
    parts.reserve(2);
    parts.push_back(doc_text(open));
    parts.push_back(print_flow(nodes, true));
    if (!inner.kids.empty()) {
      const cst::Node& last = inner.kids.back();
      const usize from = last.offset + last.length;
      const usize to = group.offset + group.length;
      if (to > from) {
        std::vector<Doc> tail = gap_docs(source_.substr(from, to - from));
        parts.insert(parts.end(), std::make_move_iterator(tail.begin()),
                     std::make_move_iterator(tail.end()));
      }
    }
    return doc_concat(std::move(parts));
  }
  if (items.empty() || (items.size() == 1 && items[0].nodes.empty())) {
    std::vector<Doc> parts;
    parts.reserve(3);
    parts.push_back(doc_text(open));
    if (had_comma) {
      parts.push_back(doc_text(","));
    }
    parts.push_back(doc_text(close));
    return doc_concat(std::move(parts));
  }

  std::vector<Doc> item_docs;
  item_docs.reserve(items.size());
  for (Item& item : items) {
    item_docs.push_back(print_flow(item.nodes, true));
  }
  std::vector<Doc> body;
  body.reserve(item_docs.size() * 2 + 1);
  for (usize k = 0; k < item_docs.size(); ++k) {
    if (k > 0) {
      body.push_back(doc_text(","));
      body.push_back(doc_line());
    }
    body.push_back(std::move(item_docs[k]));
  }
  // A comma the source did not write lands inside a delimiter it
  // never closed and grows by one per pass, so an unclosed list
  // invents none; the comma the source did write is its own byte and
  // prints whatever the group's shape.
  if (trailing || (group.has_closer && items.size() >= 2)) {
    body.push_back(
        doc_if_break(doc_text(","), trailing ? doc_text(",") : doc_empty()));
  }
  std::vector<Doc> inner_parts;
  inner_parts.reserve(2);
  inner_parts.push_back(doc_softline());
  inner_parts.push_back(doc_concat(std::move(body)));
  // The bytes between the last item and a closer are a real part of
  // the source. A break there has to be written as a break: dropping
  // it lets the next pass read a shorter, differently split tree and
  // move the layout. Where there is no gap, the softline still lets
  // a long list wrap before its closer.
  std::vector<Doc> before_close;
  if (!inner.kids.empty()) {
    const cst::Node& last = inner.kids.back();
    const usize from = last.offset + last.length;
    const usize to = group.offset + group.length - (group.has_closer ? 1 : 0);
    if (to > from) {
      const std::string_view gap = source_.substr(from, to - from);
      // A plain blank is layout the formatter owns: it normalizes the
      // space before a closer away. Only a break the source wrote has
      // to survive, or the next pass reads a different tree.
      if (gap.find('\n') != std::string_view::npos) {
        before_close = gap_docs(gap);
      }
    }
  }
  if (before_close.empty()) {
    before_close.push_back(doc_softline());
  }
  std::vector<Doc> out;
  out.reserve(4);
  out.push_back(doc_text(open));
  out.push_back(
      doc_nest(config_.indent_width, doc_concat(std::move(inner_parts))));
  out.insert(out.end(), std::make_move_iterator(before_close.begin()),
             std::make_move_iterator(before_close.end()));
  out.push_back(doc_text(close));
  return doc_group(doc_concat(std::move(out)));
}

Doc Printer::print_literal(const cst::Node& group) {
  if (group.kids.empty()) {
    return doc_text("{}");
  }
  const cst::Node& inner = group.kids[0];
  bool had_comma = false;
  bool trailing = false;
  std::vector<Item> items = split_on_commas(inner, had_comma, trailing);
  const std::string_view close = group.has_closer ? "}" : "";
  if (items.empty() || (items.size() == 1 && items[0].nodes.empty())) {
    // A closer the source never wrote is not invented here either:
    // the next pass would read it as the end of a group this one left
    // open.
    std::vector<Doc> parts;
    parts.reserve(3);
    parts.push_back(doc_text("{"));
    if (had_comma) {
      parts.push_back(doc_text(","));
    }
    parts.push_back(doc_text(close));
    return doc_concat(std::move(parts));
  }
  if (!had_comma) {
    std::vector<Doc> inner_parts;
    inner_parts.reserve(2);
    inner_parts.push_back(doc_line());
    inner_parts.push_back(print_flow(items[0].nodes, true));
    std::vector<Doc> out;
    out.reserve(4);
    out.push_back(doc_text("{"));
    out.push_back(
        doc_nest(config_.indent_width, doc_concat(std::move(inner_parts))));
    out.push_back(doc_line());
    out.push_back(doc_text(close));
    return doc_group(doc_concat(std::move(out)));
  }

  std::vector<Doc> item_docs;
  item_docs.reserve(items.size());
  for (Item& item : items) {
    item_docs.push_back(print_flow(item.nodes, true));
  }
  std::vector<Doc> body;
  body.reserve(item_docs.size() * 2 + 1);
  for (usize k = 0; k < item_docs.size(); ++k) {
    if (k > 0) {
      body.push_back(doc_text(","));
      body.push_back(doc_line());
    }
    body.push_back(std::move(item_docs[k]));
  }
  // An unclosed literal grows an invented comma by one per pass, the
  // same way an unclosed list does; the comma the source wrote is its
  // own byte and stays.
  if (trailing || (group.has_closer && items.size() >= 2)) {
    body.push_back(
        doc_if_break(doc_text(","), trailing ? doc_text(",") : doc_empty()));
  }
  std::vector<Doc> inner_parts;
  inner_parts.reserve(2);
  inner_parts.push_back(doc_line());
  inner_parts.push_back(doc_concat(std::move(body)));
  std::vector<Doc> out;
  out.push_back(doc_text("{"));
  out.push_back(
      doc_nest(config_.indent_width, doc_concat(std::move(inner_parts))));
  out.push_back(doc_line());
  out.push_back(doc_text(close));
  return doc_group(doc_concat(std::move(out)));
}

}  // namespace alcy_fmt
