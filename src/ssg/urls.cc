// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ssg/urls.h"

#include <string>
#include <string_view>

#include "ssg/catalog.h"
#include "ssg/markdown.h"

namespace ssg {

std::string canonical_url(std::string_view route) {
  constexpr std::string_view suffix = "index.html";
  std::string trimmed = std::string(route);
  if (trimmed.size() >= suffix.size() &&
      trimmed.compare(trimmed.size() - suffix.size(), suffix.size(), suffix) ==
          0) {
    trimmed.erase(trimmed.size() - suffix.size());
  }
  return std::string(SITE_URL) + trimmed;
}

std::string page_links(const Catalog& catalog,
                       std::string_view language,
                       std::string_view tree_path) {
  std::string out;
  out += "<link rel=\"canonical\" href=\"" +
         canonical_url(prefix_of(catalog, language) + std::string(tree_path)) +
         "\" />";
  for (const std::string& other : catalog.languages) {
    out += "\n    <link rel=\"alternate\" hreflang=\"" + other + "\" href=\"" +
           canonical_url(prefix_of(catalog, other) + std::string(tree_path)) +
           "\" />";
  }
  return out;
}

std::string social_meta(std::string_view title,
                        std::string_view description,
                        std::string_view route) {
  const std::string title_text = escape_html(title);
  const std::string description_text = escape_html(description);
  const std::string url = canonical_url(route);
  std::string out;
  out += "<meta property=\"og:type\" content=\"website\" />";
  out += "\n    <meta property=\"og:title\" content=\"" + title_text + "\" />";
  out += "\n    <meta property=\"og:description\" content=\"" +
         description_text + "\" />";
  out += "\n    <meta property=\"og:url\" content=\"" + url + "\" />";
  out += "\n    <meta property=\"og:image\" content=\"" +
         std::string(OG_IMAGE) + "\" />";
  out += "\n    <meta name=\"twitter:card\" content=\"summary_large_image\" />";
  return out;
}

std::string speculation_rules() {
  return std::string("<script type=\"speculationrules\">") +
         "{\"prerender\":[{\"source\":\"document\",\"where\":{\"href_matches\":"
         "\"/alcy/guide/**\"},\"eagerness\":\"moderate\"},"
         "{\"source\":\"document\",\"where\":{\"href_matches\":\"/alcy/"
         "reference/**\"},\"eagerness\":\"moderate\"}],"
         "\"prefetch\":[{\"source\":\"document\",\"where\":{\"href_matches\":"
         "\"/alcy/*\"},\"eagerness\":\"moderate\"}]}"
         "</script>";
}

std::string catalog_map(std::string_view assets, std::string_view language) {
  std::string out = "<script type=\"importmap\">{\"imports\":{\"alcy:i18n\":\"";
  out += assets;
  out += "shared/i18n.";
  out += language;
  out += ".js\"}}</script>";
  return out;
}

}  // namespace ssg
