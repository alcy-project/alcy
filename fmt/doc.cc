// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fmt/doc.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alcy_fmt {

namespace {

// One pending rendering step: a document together with the indentation
// in force and whether it is being measured flat.
struct Cmd {
  const Doc* doc = nullptr;
  usize indent = 0;
  bool flat = false;
};

void push_children(std::vector<Cmd>& stack,
                   const Doc& doc,
                   usize indent,
                   bool flat) {
  for (usize i = doc.kids.size(); i > 0; --i) {
    stack.push_back(Cmd{.doc = &doc.kids[i - 1], .indent = indent, .flat = flat});
  }
}

// Whether the document still fits in remaining columns before the next
// hard newline. A hardline never fits a flat rendering, which is what
// makes a group that holds one break.
bool fits(const Doc& doc, usize remaining) {
  std::vector<Cmd> stack;
  stack.push_back(Cmd{.doc = &doc, .indent = 0, .flat = true});
  while (!stack.empty()) {
    const Cmd cmd = stack.back();
    stack.pop_back();
    const Doc& node = *cmd.doc;
    switch (node.kind) {
      case DocKind::Empty: break;
      case DocKind::Text: {
        const usize newline = node.text.rfind('\n');
        if (newline == std::string::npos) {
          if (node.text.size() > remaining) {
            return false;
          }
          remaining -= node.text.size();
        } else {
          remaining = node.text.size() - newline - 1;
        }
        break;
      }
      case DocKind::Concat:
        push_children(stack, node, cmd.indent, cmd.flat);
        break;
      case DocKind::Nest:
        if (!node.kids.empty()) {
          stack.push_back(Cmd{.doc = &node.kids[0],
                              .indent = cmd.indent + node.indent,
                              .flat = cmd.flat});
        }
        break;
      case DocKind::Group:
        if (!node.kids.empty()) {
          stack.push_back(
              Cmd{.doc = &node.kids[0], .indent = cmd.indent, .flat = true});
        }
        break;
      case DocKind::IfBreak:
        if (!node.kids.empty()) {
          const usize which = cmd.flat ? 1 : 0;
          if (which < node.kids.size()) {
            stack.push_back(Cmd{.doc = &node.kids[which],
                                .indent = cmd.indent,
                                .flat = cmd.flat});
          }
        }
        break;
      case DocKind::Line:
        if (cmd.flat) {
          if (remaining == 0) {
            return false;
          }
          --remaining;
        } else {
          return true;
        }
        break;
      case DocKind::SoftLine:
        if (!cmd.flat) {
          return true;
        }
        break;
      case DocKind::HardLine: return false;
    }
  }
  return true;
}

void append_text(std::string& out, usize& column, std::string_view text) {
  out.append(text);
  const usize newline = text.rfind('\n');
  if (newline == std::string_view::npos) {
    column += text.size();
  } else {
    column = text.size() - newline - 1;
  }
}

void append_newline(std::string& out, usize& column, usize indent) {
  // Trailing blanks never survive a line break.
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) {
    out.pop_back();
  }
  out.push_back('\n');
  out.append(indent, ' ');
  column = indent;
}

}  // namespace

Doc doc_empty() { return Doc{}; }

Doc doc_text(std::string_view text) {
  Doc doc;
  if (!text.empty()) {
    doc.kind = DocKind::Text;
    doc.text = std::string(text);
  }
  return doc;
}

Doc doc_concat(std::vector<Doc> parts) {
  Doc doc;
  doc.kind = DocKind::Concat;
  doc.kids = std::move(parts);
  return doc;
}

Doc doc_line() {
  Doc doc;
  doc.kind = DocKind::Line;
  return doc;
}

Doc doc_softline() {
  Doc doc;
  doc.kind = DocKind::SoftLine;
  return doc;
}

Doc doc_hardline() {
  Doc doc;
  doc.kind = DocKind::HardLine;
  return doc;
}

Doc doc_nest(usize indent, Doc child) {
  Doc doc;
  doc.kind = DocKind::Nest;
  doc.indent = indent;
  doc.kids.push_back(std::move(child));
  return doc;
}

Doc doc_group(Doc child) {
  Doc doc;
  doc.kind = DocKind::Group;
  doc.kids.push_back(std::move(child));
  return doc;
}

Doc doc_if_break(Doc broken, Doc flat) {
  Doc doc;
  doc.kind = DocKind::IfBreak;
  doc.kids.push_back(std::move(broken));
  doc.kids.push_back(std::move(flat));
  return doc;
}

std::string render(const Doc& doc, usize width) {
  std::string out;
  usize column = 0;
  std::vector<Cmd> stack;
  stack.push_back(Cmd{.doc = &doc, .indent = 0, .flat = false});
  while (!stack.empty()) {
    const Cmd cmd = stack.back();
    stack.pop_back();
    const Doc& node = *cmd.doc;
    switch (node.kind) {
      case DocKind::Empty: break;
      case DocKind::Text: append_text(out, column, node.text); break;
      case DocKind::Concat:
        push_children(stack, node, cmd.indent, cmd.flat);
        break;
      case DocKind::Nest:
        if (!node.kids.empty()) {
          stack.push_back(Cmd{.doc = &node.kids[0],
                              .indent = cmd.indent + node.indent,
                              .flat = cmd.flat});
        }
        break;
      case DocKind::Group: {
        if (node.kids.empty()) {
          break;
        }
        bool flat = cmd.flat;
        if (!flat) {
          const usize remaining = width > column ? width - column : 0;
          flat = fits(node.kids[0], remaining);
        }
        stack.push_back(
            Cmd{.doc = &node.kids[0], .indent = cmd.indent, .flat = flat});
        break;
      }
      case DocKind::IfBreak:
        if (!node.kids.empty()) {
          const usize which = cmd.flat ? 1 : 0;
          if (which < node.kids.size()) {
            stack.push_back(Cmd{.doc = &node.kids[which],
                                .indent = cmd.indent,
                                .flat = cmd.flat});
          }
        }
        break;
      case DocKind::Line:
        if (cmd.flat) {
          ++column;
          out.push_back(' ');
        } else {
          append_newline(out, column, cmd.indent);
        }
        break;
      case DocKind::SoftLine:
        if (!cmd.flat) {
          append_newline(out, column, cmd.indent);
        }
        break;
      case DocKind::HardLine: append_newline(out, column, cmd.indent); break;
    }
  }
  return out;
}

}  // namespace alcy_fmt
