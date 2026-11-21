// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

std::vector<Printer::Chunk> Printer::collect_chunks(const cst::Node& seq,
                                                    cst::BraceRole role) const {
  const bool split_comma = role == cst::BraceRole::StructBody ||
                           role == cst::BraceRole::EnumBody ||
                           role == cst::BraceRole::MatchBody;
  std::vector<Chunk> chunks;
  chunks.reserve(seq.kids.size() / 2 + 1);
  Chunk current;
  // The previous lexeme ended a chunk: a comment on the same line as
  // the separator rides the line that separator finished rather than
  // starting one of its own, or the layout would move it on the pass
  // over this layout's own output.
  bool after_separator = false;
  // The separator just consumed was a semicolon, so a semicolon spelled
  // right after it terminates the statement that separator ended.
  bool after_semicolon = false;
  const auto flush = [&]() {
    if (!current.nodes.empty()) {
      chunks.push_back(std::move(current));
      current = Chunk{};
    }
  };
  usize separator_end = 0;
  for (const cst::Node& node : seq.kids) {
    if (node.kind == cst::NodeKind::Token &&
        node.token == lexer::TokenKind::Semicolon) {
      // A semicolon with no chunk in front of it ends nothing: the
      // grammar skips a stray one, and its flag must not leak into the
      // chunk that follows. The one exception is the pair the source
      // spells on the line after a break the lexer already ended the
      // statement at: both terminate the same statement, and the chunk
      // has to learn that the source wrote one, because the layout keeps
      // a spelled semicolon wherever the line break it would leave
      // reads differently. The chunk it lands on holds code: a comment
      // chunk terminates nothing, so the flag stays off one.
      if (!current.nodes.empty()) {
        current.explicit_semi = !node.synthetic_semi;
      } else if (!node.synthetic_semi && after_semicolon && !chunks.empty() &&
                 !chunks.back().is_comment) {
        chunks.back().explicit_semi = true;
      }
      flush();
      after_separator = true;
      after_semicolon = true;
      separator_end = node.offset + node.length;
      continue;
    }
    if (split_comma && node.kind == cst::NodeKind::Token &&
        node.token == lexer::TokenKind::Comma) {
      flush();
      after_separator = true;
      after_semicolon = false;
      separator_end = node.offset + node.length;
      continue;
    }
    if (node.kind == cst::NodeKind::Comment) {
      const bool same_line = node.newlines_before == 0 &&
                             (!current.nodes.empty() || after_separator);
      if (same_line) {
        if (current.nodes.empty() && !chunks.empty()) {
          chunks.back().nodes.push_back(&node);
          chunks.back().has_riding_gap = true;
          chunks.back().riding_gap_start = separator_end;
        } else {
          current.nodes.push_back(&node);
        }
        if (node.comment == cst::CommentKind::Line) {
          flush();
        }
        after_separator = false;
      } else {
        flush();
        Chunk comment;
        comment.nodes.push_back(&node);
        comment.is_comment = true;
        comment.newlines_before = node.newlines_before;
        chunks.push_back(std::move(comment));
      }
      after_semicolon = false;
      continue;
    }
    if (current.nodes.empty()) {
      current.newlines_before = node.newlines_before;
    }
    current.nodes.push_back(&node);
    after_separator = false;
    after_semicolon = false;
  }
  flush();
  return chunks;
}

// Whether a chunk is an `#[attribute(...)]` on its own line, the piece a
// module-level item's attribute occupies. The item reads glued to it, so
// the blank line between chunks is suppressed there.
bool Printer::chunk_is_attribute(const Chunk& chunk) {
  for (const cst::Node* node : chunk.nodes) {
    if (node->kind != cst::NodeKind::Token) {
      return false;
    }
    return node->token == lexer::TokenKind::Hash;
  }
  return false;
}

// Whether a run of nodes is one the lexer would end a statement after, so
// the newline that replaces a spelled-out semicolon reads back as one.
bool Printer::ends_like_statement(const std::vector<const cst::Node*>& nodes) {
  for (usize i = nodes.size(); i > 0; --i) {
    const cst::Node& node = *nodes[i - 1];
    if (node.kind == cst::NodeKind::Comment) {
      continue;
    }
    if (node.kind == cst::NodeKind::Group) {
      return node.has_closer;
    }
    switch (node.token) {
      case lexer::TokenKind::Ident:
      case lexer::TokenKind::Underscore:
      case lexer::TokenKind::Integer:
      case lexer::TokenKind::Float:
      case lexer::TokenKind::String:
      case lexer::TokenKind::Char:
      case lexer::TokenKind::True:
      case lexer::TokenKind::False:
      case lexer::TokenKind::Break:
      case lexer::TokenKind::Continue:
      case lexer::TokenKind::Ret:
      case lexer::TokenKind::Question:
      case lexer::TokenKind::DotDot:
      case lexer::TokenKind::DotDotEq:
      case lexer::TokenKind::DotDotLess:
      case lexer::TokenKind::Self:
      case lexer::TokenKind::SelfType:
      case lexer::TokenKind::I8:
      case lexer::TokenKind::I16:
      case lexer::TokenKind::I32:
      case lexer::TokenKind::I64:
      case lexer::TokenKind::I128:
      case lexer::TokenKind::Isize:
      case lexer::TokenKind::U8:
      case lexer::TokenKind::U16:
      case lexer::TokenKind::U32:
      case lexer::TokenKind::U64:
      case lexer::TokenKind::U128:
      case lexer::TokenKind::Usize:
      case lexer::TokenKind::F32:
      case lexer::TokenKind::F64:
      case lexer::TokenKind::Bool:
      case lexer::TokenKind::Str: return true;
      default: return false;
    }
  }
  return false;
}

bool Printer::chunk_is_use(const Chunk& chunk) {
  for (usize i = 0; i < chunk.nodes.size(); ++i) {
    const cst::Node& node = *chunk.nodes[i];
    if (node.kind != cst::NodeKind::Token) {
      continue;
    }
    if (node.token == lexer::TokenKind::Use) {
      return true;
    }
    if (node.token == lexer::TokenKind::Pub) {
      for (usize j = i + 1; j < chunk.nodes.size(); ++j) {
        if (chunk.nodes[j]->kind == cst::NodeKind::Token &&
            chunk.nodes[j]->token == lexer::TokenKind::Use) {
          return true;
        }
      }
      return false;
    }
    return false;
  }
  return false;
}

// Where the chunk's run of trailing comments that end a line starts.
// A block comment does not end a line, so it stays part of the code.
usize Printer::trailing_eol_start(const Chunk& chunk) {
  usize end = chunk.nodes.size();
  while (end > 0) {
    const cst::Node& node = *chunk.nodes[end - 1];
    if (node.kind != cst::NodeKind::Comment ||
        node.comment == cst::CommentKind::Block) {
      break;
    }
    --end;
  }
  return end;
}

// Whether the code ends in a statement block rather than an
// expression: a match arm whose body is a block takes no separator
// after it, while one whose body is a match or a struct literal is an
// expression arm and keeps its comma.
bool Printer::ends_in_block(const std::vector<const cst::Node*>& code) {
  for (usize i = code.size(); i > 0; --i) {
    const cst::Node* node = code[i - 1];
    if (node->kind == cst::NodeKind::Comment) {
      continue;
    }
    if (node->kind != cst::NodeKind::Group ||
        node->group_role != cst::GroupRole::Brace) {
      return false;
    }
    return node->brace_role == cst::BraceRole::Block ||
           node->brace_role == cst::BraceRole::ExprBlock ||
           node->brace_role == cst::BraceRole::FnBody;
  }
  return false;
}

// A spelled-out semicolon survives wherever the newline that replaces
// it would not read the same. A spec or extern member is terminated by
// one outright. The last chunk of a body keeps its statement status:
// without the semicolon the run becomes the block's trailing
// expression instead of a statement whose value nobody takes. A run the
// lexer does not end a statement after (an intrinsic signature that
// closes in a greater-than) keeps it too. Everywhere else the line
// break reads the same to the parser.
// Whether the chunk is an item the grammar terminates with a
// semicolon: a use, or an intrinsic signature. The newline a formatter
// writes reads the same to the compiler, but the grammar spells these
// terminators, so the layout does too.
bool Printer::chunk_is_semi_terminated_item(const Chunk& chunk) {
  for (const cst::Node* node : chunk.nodes) {
    if (node->kind == cst::NodeKind::Token &&
        (node->token == lexer::TokenKind::Use ||
         node->token == lexer::TokenKind::Intrinsic)) {
      return true;
    }
  }
  return false;
}

// Whether a chunk begins with a token the lexer's semicolon-insertion
// rule suppresses: a line break in front of one of these does not
// stand in for a semicolon, so the spelled one ahead of it cannot be
// dropped without merging the two chunks into one.
bool Printer::chunk_starts_with_suppressor(const Chunk& chunk) {
  for (const cst::Node* node : chunk.nodes) {
    if (node->kind != cst::NodeKind::Token) {
      return false;
    }
    switch (node->token) {
      case lexer::TokenKind::Comma:
      case lexer::TokenKind::RParen:
      case lexer::TokenKind::RBracket:
      case lexer::TokenKind::RBrace:
      case lexer::TokenKind::Dot:
      // The whole dot family: the lexer's probe reads the next
      // character, and every token that starts with a dot reads as a
      // chain continuation to it, so a line break before any of them
      // stands without a semicolon.
      case lexer::TokenKind::DotDot:
      case lexer::TokenKind::DotDotEq:
      case lexer::TokenKind::DotDotLess:
      case lexer::TokenKind::Else: return true;
      default: return false;
    }
  }
  return false;
}

bool Printer::keeps_semicolon(const Chunk& chunk,
                              const std::vector<const cst::Node*>& code,
                              cst::BraceRole role,
                              bool last_chunk,
                              bool next_suppresses) {
  // The items the grammar terminates with a semicolon carry it whether
  // the source spelled it or left it to the newline: a reader without
  // the insertion rule, the tree-sitter grammar among them, has to see
  // the terminator the specification writes.
  if (chunk_is_semi_terminated_item(chunk)) {
    return true;
  }
  if (!chunk.explicit_semi) {
    return false;
  }
  if (role == cst::BraceRole::SpecBody || role == cst::BraceRole::ExternBody) {
    return true;
  }
  if (next_suppresses) {
    return true;
  }
  if (last_chunk) {
    return true;
  }
  return !ends_like_statement(code);
}

// The line one chunk contributes: its code, the separator that ends it,
// and the comments that ride after it. A comment chunk is its own text.
Doc Printer::chunk_line(const Chunk& chunk, Doc terminator) {
  if (chunk.is_comment) {
    return doc_text(token_facts::node_text(source_, *chunk.nodes.front()));
  }
  const usize end = trailing_eol_start(chunk);
  std::vector<const cst::Node*> code(
      chunk.nodes.begin(), chunk.nodes.begin() + static_cast<isize>(end));
  std::vector<Doc> parts;
  parts.reserve(chunk.nodes.size() - end + 4);
  parts.push_back(print_line(code));
  if (terminator.kind != DocKind::Empty) {
    parts.push_back(std::move(terminator));
  }
  // The space rides only after code: a chunk that is nothing but its
  // comments starts the line with them, and a pass over a layout that
  // began each such line with a space would read the space back and
  // move the comment again. After an error the source's own gap is
  // pinned instead of the canonical space -- those bytes decide what
  // the error's bytes read back as, and a comment that moved would
  // feed them to the span.
  for (usize i = end; i < chunk.nodes.size(); ++i) {
    if (end > 0) {
      const cst::Node& last_code = *chunk.nodes[end - 1];
      const cst::Node& comment = *chunk.nodes[i];
      const bool rides_separator =
          i == end && chunk.has_riding_gap && chunk.riding_gap_start > 0;
      if (last_code.kind == cst::NodeKind::Token &&
          last_code.token == lexer::TokenKind::Error) {
        const usize gap_start =
            rides_separator
                ? chunk.riding_gap_start
                : last_code.offset +
                      token_facts::error_text(source_, last_code).size();
        if (comment.offset > gap_start) {
          parts.push_back(
              doc_text(source_.substr(gap_start, comment.offset - gap_start)));
        }
      } else if (rides_separator && comment.offset > chunk.riding_gap_start) {
        parts.push_back(doc_text(source_.substr(
            chunk.riding_gap_start, comment.offset - chunk.riding_gap_start)));
      } else {
        parts.push_back(doc_text(" "));
      }
    }
    parts.push_back(doc_text(token_facts::node_text(source_, *chunk.nodes[i])));
  }
  return doc_concat(std::move(parts));
}

// One chunk per line, with the blank lines the source had between them.
Doc Printer::print_body_lines(const std::vector<Chunk>& chunks,
                              cst::BraceRole role,
                              bool top_level) {
  const bool member = top_level || role == cst::BraceRole::ImplBody ||
                      role == cst::BraceRole::SpecBody ||
                      role == cst::BraceRole::ExternBody ||
                      role == cst::BraceRole::GroupBody;
  std::vector<Doc> parts;
  parts.reserve(chunks.size() * 2);
  for (usize k = 0; k < chunks.size(); ++k) {
    if (k > 0) {
      const bool prev_comment = chunks[k - 1].is_comment;
      const bool cur_comment = chunks[k].is_comment;
      bool blank = false;
      if (member) {
        if (prev_comment && cur_comment) {
          blank = chunks[k].newlines_before >= 2;
        } else if (prev_comment) {
          blank = chunks[k].newlines_before >= 2;
        } else if (chunk_is_attribute(chunks[k - 1])) {
          blank = false;
        } else if (cur_comment) {
          blank = true;
        } else {
          blank = true;
        }
        if (top_level && chunk_is_use(chunks[k - 1]) &&
            chunk_is_use(chunks[k])) {
          blank = false;
        }
      } else {
        blank = chunks[k].newlines_before >= 2;
      }
      parts.push_back(doc_hardline());
      if (blank) {
        parts.push_back(doc_hardline());
      }
    }
    const usize end = trailing_eol_start(chunks[k]);
    std::vector<const cst::Node*> code(
        chunks[k].nodes.begin(),
        chunks[k].nodes.begin() + static_cast<isize>(end));
    bool next_suppresses = false;
    for (usize j = k + 1; j < chunks.size(); ++j) {
      if (chunks[j].is_comment) {
        continue;
      }
      next_suppresses = chunk_starts_with_suppressor(chunks[j]);
      break;
    }
    parts.push_back(chunk_line(
        chunks[k], chunk_terminator(chunks[k], code, role,
                                    k + 1 == chunks.size(), next_suppresses)));
  }
  return doc_concat(std::move(parts));
}

// The separator that ends one chunk of a body. A field and a variant
// are comma-terminated; a match arm takes a comma unless its body is a
// block, which the line break already separates, and one the source
// spelled with a semicolon keeps it; everywhere else only a
// load-bearing semicolon appears.
Doc Printer::chunk_terminator(const Chunk& chunk,
                              const std::vector<const cst::Node*>& code,
                              cst::BraceRole role,
                              bool last_chunk,
                              bool next_suppresses) {
  for (usize i = code.size(); i > 0; --i) {
    const cst::Node* node = code[i - 1];
    if (node->kind == cst::NodeKind::Comment) {
      continue;
    }
    // A chunk that ends in a delimiter the source never closed eats
    // whatever follows it in the output, so the next pass would read a
    // terminator as part of the group's contents. No terminator is
    // written after one.
    if (node->kind == cst::NodeKind::Group && !node->has_closer) {
      return doc_empty();
    }
    // A hungry error eats a separator the same way; a complete one
    // ended where it ended, and the separator after it reads back as a
    // separator.
    if (node->kind == cst::NodeKind::Token &&
        node->token == lexer::TokenKind::Error &&
        token_facts::error_absorbs(token_facts::error_text(source_, *node))) {
      return doc_empty();
    }
    break;
  }
  if (role == cst::BraceRole::StructBody || role == cst::BraceRole::EnumBody) {
    return doc_text(",");
  }
  if (role == cst::BraceRole::MatchBody) {
    if (chunk.explicit_semi) {
      return doc_text(";");
    }
    if (ends_in_block(code)) {
      return doc_empty();
    }
    return doc_text(",");
  }
  return keeps_semicolon(chunk, code, role, last_chunk, next_suppresses)
             ? doc_text(";")
             : doc_empty();
}

Doc Printer::print_line(const std::vector<const cst::Node*>& nodes) {
  bool has_block = false;
  for (const cst::Node* node : nodes) {
    if (node->kind == cst::NodeKind::Group &&
        node->group_role == cst::GroupRole::Brace &&
        node->brace_role != cst::BraceRole::Literal) {
      has_block = true;
      break;
    }
  }
  return print_flow(nodes, !has_block);
}

}  // namespace alcy_fmt
