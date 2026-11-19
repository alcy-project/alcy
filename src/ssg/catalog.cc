// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/catalog.h"

#include <fstream>
#include <initializer_list>
#include <ios>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ssg {
namespace {

// A minimal reader for the catalog's fixed layout: string keys, string
// or array-of-strings values, two levels of objects. The file is
// authored in-tree; anything more than this shape failing is a build
// error, not a best-effort parse.
struct Json {
  const char* cursor;
  const char* begin;

  void skip_ws() {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' ||
           *cursor == '\r') {
      cursor++;
    }
  }

  std::string error_at(std::string_view message) const {
    return std::string(message) + " at byte " +
           std::to_string(static_cast<usize>(cursor - begin));
  }

  base::Result<std::string, std::string> parse_string() {
    skip_ws();
    if (*cursor != '"') {
      return base::make_err(error_at("expected a string"));
    }
    cursor++;
    std::string out;
    while (*cursor != '\0' && *cursor != '"') {
      const char c = *cursor;
      if (c == '\\') {
        cursor++;
        switch (*cursor) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            // The site's catalog keys select ASCII; a non-BMP escape
            // arrives in the authored file as literal UTF-8 bytes.
            u32 code = 0;
            for (i32 i = 0; i < 4; i++) {
              const char hex = cursor[i + 1];
              code <<= 4;
              if (hex >= '0' && hex <= '9') {
                code |= static_cast<u32>(hex - '0');
              } else if (hex >= 'a' && hex <= 'f') {
                code |= static_cast<u32>(hex - 'a' + 10);
              } else if (hex >= 'A' && hex <= 'F') {
                code |= static_cast<u32>(hex - 'A' + 10);
              } else {
                return base::make_err(error_at("bad \\u escape"));
              }
            }
            cursor += 4;
            // A surrogate half names no character; UTF-8 encodes one
            // code point per escape and the catalog writes the rest
            // literally.
            if (code >= 0xD800 && code <= 0xDFFF) {
              return base::make_err(
                  error_at("a surrogate escape does not name a character"));
            }
            if (code < 0x80) {
              out += static_cast<char>(code);
            } else if (code < 0x800) {
              out += static_cast<char>(0xC0 | (code >> 6));
              out += static_cast<char>(0x80 | (code & 0x3F));
            } else {
              out += static_cast<char>(0xE0 | (code >> 12));
              out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
              out += static_cast<char>(0x80 | (code & 0x3F));
            }
            break;
          }
          default: return base::make_err(error_at("bad escape in a string"));
        }
        cursor++;
        continue;
      }
      out += c;
      cursor++;
    }
    if (*cursor != '"') {
      return base::make_err(error_at("unterminated string"));
    }
    cursor++;
    return base::make_ok(std::move(out));
  }

  base::Result<std::vector<std::string>, std::string> parse_string_array() {
    skip_ws();
    if (*cursor != '[') {
      return base::make_err(error_at("expected an array"));
    }
    cursor++;
    std::vector<std::string> items;
    while (true) {
      skip_ws();
      if (*cursor == ']') {
        cursor++;
        return base::make_ok(std::move(items));
      }
      base::Result<std::string, std::string> item = parse_string();
      if (item.is_err()) {
        return base::make_err(std::move(item).unwrap_err());
      }
      items.push_back(std::move(item).unwrap());
      skip_ws();
      if (*cursor == ',') {
        cursor++;
        continue;
      }
      if (*cursor == ']') {
        cursor++;
        return base::make_ok(std::move(items));
      }
      return base::make_err(error_at("expected , or ] in an array"));
    }
  }

  // { "<key>": "<value>", ... } with string values only.
  base::Result<std::unordered_map<std::string, std::string>, std::string>
  parse_string_map() {
    skip_ws();
    if (*cursor != '{') {
      return base::make_err(error_at("expected an object"));
    }
    cursor++;
    std::unordered_map<std::string, std::string> table;
    while (true) {
      skip_ws();
      if (*cursor == '}') {
        cursor++;
        return base::make_ok(std::move(table));
      }
      base::Result<std::string, std::string> key = parse_string();
      if (key.is_err()) {
        return base::make_err(std::move(key).unwrap_err());
      }
      skip_ws();
      if (*cursor != ':') {
        return base::make_err(error_at("expected : after a key"));
      }
      cursor++;
      base::Result<std::string, std::string> value = parse_string();
      if (value.is_err()) {
        return base::make_err(std::move(value).unwrap_err());
      }
      table.insert({std::move(key).unwrap(), std::move(value).unwrap()});
      skip_ws();
      if (*cursor == ',') {
        cursor++;
        continue;
      }
      if (*cursor == '}') {
        cursor++;
        return base::make_ok(std::move(table));
      }
      return base::make_err(error_at("expected , or } in an object"));
    }
  }
};

std::string render_params(
    std::string_view template_text,
    std::span<const std::pair<std::string_view, std::string_view>> params) {
  std::string out;
  out.reserve(template_text.size() + params.size() * 8);
  usize index = 0;
  while (index < template_text.size()) {
    const char c = template_text[index];
    if (c == '{') {
      const usize close = template_text.find('}', index + 1);
      if (close != std::string_view::npos) {
        const std::string_view name =
            template_text.substr(index + 1, close - index - 1);
        bool replaced = false;
        for (const auto& [key, value] : params) {
          if (key == name) {
            out += value;
            replaced = true;
            break;
          }
        }
        if (replaced) {
          index = close + 1;
          continue;
        }
        out += template_text.substr(index, close + 1 - index);
        index = close + 1;
        continue;
      }
    }
    out += c;
    index++;
  }
  return out;
}

}  // namespace

base::Result<Catalog, std::string> load_catalog(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return base::make_err("cannot open the catalog: " + path);
  }
  std::ostringstream bytes;
  bytes << input.rdbuf();
  const std::string source = bytes.str();
  Json json{source.c_str(), source.c_str()};

  json.skip_ws();
  if (*json.cursor != '{') {
    return base::make_err(
        json.error_at("the catalog must open with an object"));
  }
  json.cursor++;
  Catalog catalog;
  while (true) {
    json.skip_ws();
    if (*json.cursor == '}') {
      json.cursor++;
      break;
    }
    base::Result<std::string, std::string> key = json.parse_string();
    if (key.is_err()) {
      return base::make_err(std::move(key).unwrap_err());
    }
    const std::string name = std::move(key).unwrap();
    json.skip_ws();
    if (*json.cursor != ':') {
      return base::make_err(json.error_at("expected : after a top-level key"));
    }
    json.cursor++;
    if (name == "languages") {
      auto parsed = json.parse_string_array();
      if (parsed.is_err()) {
        return base::make_err(std::move(parsed).unwrap_err());
      }
      catalog.languages = std::move(parsed).unwrap();
    } else if (name == "default") {
      auto parsed = json.parse_string();
      if (parsed.is_err()) {
        return base::make_err(std::move(parsed).unwrap_err());
      }
      catalog.default_language = std::move(parsed).unwrap();
    } else if (name == "names") {
      auto parsed = json.parse_string_map();
      if (parsed.is_err()) {
        return base::make_err(std::move(parsed).unwrap_err());
      }
      catalog.language_names = std::move(parsed).unwrap();
    } else if (name == "messages") {
      json.skip_ws();
      if (*json.cursor != '{') {
        return base::make_err(
            json.error_at("messages must be an object of objects"));
      }
      json.cursor++;
      while (true) {
        json.skip_ws();
        if (*json.cursor == '}') {
          json.cursor++;
          break;
        }
        auto language_key = json.parse_string();
        if (language_key.is_err()) {
          return base::make_err(std::move(language_key).unwrap_err());
        }
        const std::string language = std::move(language_key).unwrap();
        json.skip_ws();
        if (*json.cursor != ':') {
          return base::make_err(
              json.error_at("expected : after a language key"));
        }
        json.cursor++;
        auto table = json.parse_string_map();
        if (table.is_err()) {
          return base::make_err(std::move(table).unwrap_err());
        }
        catalog.per_language.emplace(language, std::move(table).unwrap());
        json.skip_ws();
        if (*json.cursor == ',') {
          json.cursor++;
          continue;
        }
        if (*json.cursor == '}') {
          json.cursor++;
          break;
        }
        return base::make_err(json.error_at("expected , or } in messages"));
      }
    } else {
      return base::make_err("unknown top-level key: " + name);
    }
    json.skip_ws();
    if (*json.cursor == ',') {
      json.cursor++;
      continue;
    }
    if (*json.cursor == '}') {
      json.cursor++;
      break;
    }
    return base::make_err(json.error_at("expected , or } in the catalog"));
  }
  json.skip_ws();
  if (*json.cursor != '\0') {
    return base::make_err(
        json.error_at("junk after the catalog's closing brace"));
  }

  if (catalog.languages.empty()) {
    return base::make_err(json.error_at("the catalog lists no languages"));
  }
  if (catalog.default_language.empty()) {
    catalog.default_language = catalog.languages[0];
  }
  const auto defaults = catalog.per_language.find(catalog.default_language);
  if (defaults == catalog.per_language.end()) {
    return base::make_err(
        "the catalog has no messages for the default language: " +
        catalog.default_language);
  }
  catalog.messages = defaults->second;
  for (const auto& [language, table] : catalog.per_language) {
    if (language == catalog.default_language) {
      continue;
    }
    for (const auto& [key, value] : table) {
      (void)value;
      if (catalog.messages.find(key) == catalog.messages.end()) {
        std::string error = "catalog.json: the " + language + " message `";
        error += key;
        error += "` has no ";
        error += catalog.default_language;
        error += " counterpart";
        return base::make_err(std::move(error));
      }
    }
  }
  return base::make_ok(std::move(catalog));
}

std::string translate(const Catalog& catalog,
                      std::string_view language,
                      std::string_view key) {
  const auto table = catalog.per_language.find(std::string(language));
  if (table != catalog.per_language.end()) {
    const auto found = table->second.find(std::string(key));
    if (found != table->second.end()) {
      return found->second;
    }
  }
  const auto fallback = catalog.messages.find(std::string(key));
  return fallback != catalog.messages.end() ? fallback->second
                                            : std::string(key);
}

std::string translate(
    const Catalog& catalog,
    std::string_view language,
    std::string_view key,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        params) {
  const std::string rendered = translate(catalog, language, key);
  std::vector<std::pair<std::string_view, std::string_view>> table;
  table.reserve(params.size());
  for (const auto& [name, value] : params) {
    table.emplace_back(name, value);
  }
  return render_params(rendered, table);
}

std::string prefix_of(const Catalog& catalog, std::string_view language) {
  if (language == catalog.default_language) {
    return std::string();
  }
  return std::string(language) + "/";
}

std::vector<std::string> other_languages(const Catalog& catalog,
                                         std::string_view language) {
  std::vector<std::string> others;
  for (const std::string& name : catalog.languages) {
    if (name != language) {
      others.push_back(name);
    }
  }
  return others;
}

LanguageInfo language_info_of(const Catalog& catalog,
                              std::string_view language) {
  const auto name = catalog.language_names.find(std::string(language));
  return {name != catalog.language_names.end() ? name->second
                                               : std::string(language),
          prefix_of(catalog, language)};
}
}  // namespace ssg
