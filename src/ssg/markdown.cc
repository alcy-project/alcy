// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/markdown.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssg {
namespace {

std::string_view trim(std::string_view text) {
  usize start = 0;
  usize end = text.size();
  while (start < end && (text[start] == ' ' || text[start] == '\t')) {
    start++;
  }
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
    end--;
  }
  return text.substr(start, end - start);
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() &&
         text.substr(0, prefix.size()) == prefix;
}

// ATX heading: one to six `#`, at least one space, the text is the rest.
bool match_heading(std::string_view line, i32* level, std::string* text) {
  usize count = 0;
  while (count < line.size() && line[count] == '#') {
    count++;
  }
  if (count == 0 || count > 6 || count >= line.size()) {
    return false;
  }
  if (line[count] != ' ' && line[count] != '\t') {
    return false;
  }
  *level = static_cast<i32>(count);
  *text = std::string(trim(line.substr(count + 1)));
  return true;
}

bool match_unordered(std::string_view line, std::string* item) {
  if (line.size() < 2) {
    return false;
  }
  if ((line[0] == '-' || line[0] == '*') && line[1] == ' ') {
    *item = std::string(trim(line.substr(2)));
    return true;
  }
  return false;
}

bool match_ordered(std::string_view line, std::string* item) {
  usize count = 0;
  while (count < line.size() && line[count] >= '0' && line[count] <= '9') {
    count++;
  }
  if (count == 0 || count + 2 > line.size()) {
    return false;
  }
  if (line[count] != '.' || line[count + 1] != ' ') {
    return false;
  }
  *item = std::string(trim(line.substr(count + 2)));
  return true;
}

bool match_quote(std::string_view line, std::string* text) {
  if (line.empty() || line.front() != '>') {
    return false;
  }
  std::string_view rest = line.substr(1);
  if (!rest.empty() && rest.front() == ' ') {
    rest = rest.substr(1);
  }
  *text = std::string(trim(rest));
  return true;
}

bool match_fence(std::string_view line, std::string* info) {
  if (!starts_with(line, "```")) {
    return false;
  }
  *info = std::string(trim(line.substr(3)));
  return true;
}

bool is_table_line(std::string_view line) {
  const std::string_view view = trim(line);
  return !view.empty() && view.front() == '|';
}

bool is_block_start(std::string_view line) {
  if (trim(line).empty()) {
    return true;
  }
  std::string unused;
  i32 level = 0;
  return match_fence(line, &unused) || match_heading(line, &level, &unused) ||
         match_unordered(line, &unused) || match_ordered(line, &unused) ||
         match_quote(line, &unused) || is_table_line(line);
}

// The separator row: per column, dashes with optional leading and
// trailing colons. At least one dash per cell.
bool match_separator(std::string_view line, std::vector<CellAlign>* aligns) {
  const std::vector<std::string> cells = split_table_cells(line);
  if (cells.empty()) {
    return false;
  }
  std::vector<CellAlign> result;
  result.reserve(cells.size());
  for (const std::string& cell : cells) {
    const std::string_view view = cell;
    if (view.empty()) {
      return false;
    }
    const bool left = view.front() == ':';
    const bool right = view.back() == ':';
    const usize dash_begin = left ? 1 : 0;
    const usize dash_end = view.size() - (right ? 1 : 0);
    if (dash_end <= dash_begin) {
      return false;
    }
    for (usize i = dash_begin; i < dash_end; i++) {
      if (view[i] != '-') {
        return false;
      }
    }
    using enum CellAlign;
    result.push_back(left && right ? Center
                     : right       ? Right
                     : left        ? Left
                                   : None);
  }
  *aligns = std::move(result);
  return true;
}

// Splits one table line: the leading and trailing pipes are
// delimiters, and a pipe inside a code span or after a backslash is
// cell text. The pipe escape itself is left for the inline renderer,
// which already turns `\|` into `|`.
std::vector<std::string> split_table_cells_impl(std::string_view line) {
  std::string_view view = trim(line);
  usize begin = 0;
  usize end = view.size();
  if (!view.empty() && view.front() == '|') {
    begin = 1;
  }
  if (end > begin && view.back() == '|') {
    end -= 1;
  }
  std::vector<std::string> cells;
  std::string cell;
  bool in_code = false;
  bool escaped = false;
  for (usize index = begin; index < end; index++) {
    const char c = view[index];
    if (escaped) {
      cell += c;
      escaped = false;
      continue;
    }
    if (c == '\\') {
      cell += c;
      escaped = true;
      continue;
    }
    if (c == '`') {
      in_code = !in_code;
      cell += c;
      continue;
    }
    if (c == '|' && !in_code) {
      cells.emplace_back(trim(cell));
      cell.clear();
      continue;
    }
    cell += c;
  }
  cells.emplace_back(trim(cell));
  return cells;
}

}  // namespace

std::vector<std::string> split_table_cells(std::string_view line) {
  return split_table_cells_impl(line);
}

std::string escape_html(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 16);
  for (const char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string escape_attribute(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 16);
  for (const char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

namespace {

std::vector<std::string_view> split_lines(std::string_view text) {
  // Views into the source: the parse only materializes the strings a
  // block keeps, so a file is never copied line by line.
  std::vector<std::string_view> lines;
  usize begin = 0;
  while (begin < text.size()) {
    usize end = begin;
    while (end < text.size() && text[end] != '\n') {
      end++;
    }
    std::string_view line = text.substr(begin, end - begin);
    // Keep the \r: a CRLF file is refused at the write step instead.
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    lines.push_back(line);
    begin = end < text.size() ? end + 1 : end;
  }
  return lines;
}

usize line_number(usize index) {
  return index + 1;
}

Block make_heading(usize line, i32 level, std::string text) {
  Block block;
  block.type = BlockType::Heading;
  block.line = line;
  block.level = level;
  block.text = std::move(text);
  return block;
}

Block make_paragraph(usize line, std::string text) {
  Block block;
  block.type = BlockType::Paragraph;
  block.line = line;
  block.text = std::move(text);
  return block;
}

Block make_code(usize line, std::string language, std::string text) {
  Block block;
  block.type = BlockType::Code;
  block.line = line;
  block.language = std::move(language);
  block.text = std::move(text);
  return block;
}

Block make_blockquote(usize line, std::string text) {
  Block block;
  block.type = BlockType::Blockquote;
  block.line = line;
  block.text = std::move(text);
  return block;
}

Block make_list(usize line, bool ordered, std::vector<std::string> items) {
  Block block;
  block.type = BlockType::List;
  block.line = line;
  block.ordered = ordered;
  block.items = std::move(items);
  return block;
}

Block make_table(usize line,
                 std::vector<std::vector<std::string>> rows,
                 std::vector<CellAlign> aligns) {
  Block block;
  block.type = BlockType::Table;
  block.line = line;
  block.rows = std::move(rows);
  block.aligns = std::move(aligns);
  return block;
}

}  // namespace

base::Result<Blocks, std::string> parse_markdown(std::string_view source) {
  const std::vector<std::string_view> lines = split_lines(source);
  Blocks blocks;
  usize index = 0;
  while (index < lines.size()) {
    const std::string_view line = lines[index];
    const std::string_view view = trim(line);
    if (view.empty()) {
      index++;
      continue;
    }

    std::string info;
    if (match_fence(line, &info)) {
      const usize start = line_number(index);
      index++;
      std::string body;
      bool closed = false;
      while (index < lines.size()) {
        if (starts_with(trim(lines[index]), "```")) {
          closed = true;
          index++;
          break;
        }
        if (!body.empty()) {
          body += '\n';
        }
        body += lines[index];
        index++;
      }
      if (!closed) {
        return base::make_err(
            std::string("") /* replaced by caller with path */ +
            std::to_string(start) + ": the code fence is never closed");
      }
      blocks.push_back(make_code(start, info, std::move(body)));
      continue;
    }

    i32 level = 0;
    std::string heading;
    if (match_heading(line, &level, &heading)) {
      blocks.push_back(
          make_heading(line_number(index), level, std::move(heading)));
      index++;
      continue;
    }

    std::string quote;
    if (match_quote(line, &quote)) {
      const usize start = line_number(index);
      std::string joined;
      while (index < lines.size()) {
        std::string part;
        if (!match_quote(lines[index], &part)) {
          break;
        }
        if (!joined.empty()) {
          joined += ' ';
        }
        joined += part;
        index++;
      }
      blocks.push_back(make_blockquote(start, std::move(joined)));
      continue;
    }

    std::string item;
    bool unordered = false;
    bool ordered = false;
    if (match_unordered(line, &item)) {
      unordered = true;
    } else if (match_ordered(line, &item)) {
      ordered = true;
    }
    if (unordered || ordered) {
      const usize start = line_number(index);
      std::vector<std::string> items;
      while (index < lines.size()) {
        const std::string_view current = lines[index];
        std::string current_item;
        if (unordered ? match_unordered(current, &current_item)
                      : match_ordered(current, &current_item)) {
          items.push_back(std::move(current_item));
          index++;
          continue;
        }
        if (starts_with(lines[index], "  ") && !items.empty()) {
          items.back() += ' ';
          items.back() += trim(lines[index]);
          index++;
          continue;
        }
        break;
      }
      blocks.push_back(make_list(start, ordered, std::move(items)));
      continue;
    }

    if (is_table_line(line)) {
      const usize start = line_number(index);
      std::vector<std::string_view> rows;
      usize end = index;
      while (end < lines.size() && is_table_line(lines[end])) {
        rows.push_back(lines[end]);
        end++;
      }
      std::vector<CellAlign> aligns;
      if (rows.size() >= 2 && match_separator(trim(rows[1]), &aligns)) {
        std::vector<std::vector<std::string>> parsed_rows;
        parsed_rows.push_back(split_table_cells(rows[0]));
        const usize width = parsed_rows[0].size();
        for (usize row = 2; row < rows.size(); row++) {
          std::vector<std::string> cells = split_table_cells(rows[row]);
          if (cells.size() != width) {
            return base::make_err(
                std::to_string(start + row) + ": table row has " +
                std::to_string(cells.size()) + " columns, expected " +
                std::to_string(width));
          }
          parsed_rows.push_back(std::move(cells));
        }
        blocks.push_back(
            make_table(start, std::move(parsed_rows), std::move(aligns)));
        index = end;
        continue;
      }
      // Not a table: the lines read as one paragraph, the same as any
      // block the subset does not know how to delimit.
      std::string joined;
      for (const std::string_view row : rows) {
        if (!joined.empty()) {
          joined += ' ';
        }
        joined += trim(row);
      }
      blocks.push_back(make_paragraph(start, std::move(joined)));
      index = end;
      continue;
    }

    const usize start = line_number(index);
    std::string joined;
    while (index < lines.size() && !is_block_start(lines[index])) {
      if (!joined.empty()) {
        joined += ' ';
      }
      joined += trim(lines[index]);
      index++;
    }
    blocks.push_back(make_paragraph(start, std::move(joined)));
  }
  return base::make_ok(std::move(blocks));
}

namespace {

// The `]` matching the `[` at `start`, skipping nested brackets.
usize label_end(std::string_view text, usize start) {
  usize depth = 0;
  for (usize index = start; index < text.size(); index++) {
    const char c = text[index];
    if (c == '\\') {
      index++;
    } else if (c == '[') {
      depth++;
    } else if (c == ']') {
      depth--;
      if (depth == 0) {
        return index;
      }
    }
  }
  return std::string_view::npos;
}

std::string render_inline_with(std::string_view text,
                               ResolveLink resolve,
                               void* resolve_context,
                               bool allow_links);

std::string render_inline_with(std::string_view text,
                               ResolveLink resolve,
                               void* resolve_context,
                               bool allow_links) {
  const auto resolve_or_keep = [resolve,
                                resolve_context](std::string_view url) {
    if (resolve == nullptr) {
      return std::string(url);
    }
    return resolve(url, resolve_context);
  };
  std::string html;
  usize index = 0;
  while (index < text.size()) {
    const char c = text[index];
    if (c == '\\' && index + 1 < text.size()) {
      html += escape_html(text.substr(index + 1, 1));
      index += 2;
      continue;
    }
    if (c == '`') {
      const usize end = text.find('`', index + 1);
      if (end != std::string_view::npos) {
        html += "<code>";
        html += escape_html(text.substr(index + 1, end - index - 1));
        html += "</code>";
        index = end + 1;
        continue;
      }
    }
    if (allow_links && c == '[') {
      const usize close = label_end(text, index);
      if (close != std::string_view::npos && close + 1 < text.size() &&
          text[close + 1] == '(') {
        const usize url_end = text.find(')', close + 2);
        if (url_end != std::string_view::npos) {
          const std::string_view label =
              text.substr(index + 1, close - index - 1);
          const std::string_view url =
              text.substr(close + 2, url_end - close - 2);
          html += "<a href=\"";
          html += escape_attribute(resolve_or_keep(url));
          html += "\">";
          html += render_inline_with(label, resolve, resolve_context, false);
          html += "</a>";
          index = url_end + 1;
          continue;
        }
      }
    }
    if (starts_with(text.substr(index), "**")) {
      const usize end = text.find("**", index + 2);
      if (end != std::string_view::npos) {
        html += "<strong>";
        html += render_inline_with(text.substr(index + 2, end - index - 2),
                                   resolve, resolve_context, false);
        html += "</strong>";
        index = end + 2;
        continue;
      }
    }
    if (c == '*') {
      const usize end = text.find('*', index + 1);
      if (end != std::string_view::npos) {
        html += "<em>";
        html += render_inline_with(text.substr(index + 1, end - index - 1),
                                   resolve, resolve_context, false);
        html += "</em>";
        index = end + 1;
        continue;
      }
    }
    // A run of characters with no markup: escaped in one append per
    // run rather than one substring allocation per character.
    usize run = index + 1;
    while (run < text.size()) {
      const char next = text[run];
      if (next == '\\' || next == '`' || next == '*' ||
          (allow_links && next == '[')) {
        break;
      }
      run++;
    }
    html += escape_html(text.substr(index, run - index));
    index = run;
  }
  return html;
}

}  // namespace

std::string render_inline(std::string_view text,
                          ResolveLink resolve,
                          void* resolve_context) {
  return render_inline_with(text, resolve, resolve_context, true);
}
}  // namespace ssg
