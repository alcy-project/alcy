// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ssg/catalog.h"

namespace ssg {

// The URL side of the site: where pages live, what they link to, and
// what a social client reads. Every language tree is one page set,
// and the generator stamps every page once.

constexpr std::string_view SITE_URL = "https://alcy-project.github.io/alcy/";
constexpr std::string_view SITE_PATH = "/alcy/";
constexpr std::string_view OG_IMAGE =
    "https://alcy-project.github.io/alcy/og.png";
constexpr std::string_view REPO_URL = "https://github.com/alcy-project/alcy";
constexpr std::string_view REPO_BLOB_URL =
    "https://github.com/alcy-project/alcy/blob/main";

// A directory index is served at its directory: `guide/index.html` is
// reached as `guide/`.
std::string canonical_url(std::string_view route);

// The canonical link and one alternate per language.
std::string page_links(const Catalog& catalog,
                       std::string_view language,
                       std::string_view tree_path);

// This page's address in another language tree, relative to the page.
std::string language_href(const Catalog& catalog,
                          std::string_view route,
                          std::string_view target,
                          std::string_view tree_path);

// The social card a crawler reads.
std::string social_meta(std::string_view title,
                        std::string_view description,
                        std::string_view route);

// The speculation rules one script for the whole site.
std::string speculation_rules();

// The name the shared modules import their catalog by.
constexpr std::string_view CATALOG_SPECIFIER = "alcy:i18n";

// The import map naming this tree's catalog module.
std::string catalog_map(std::string_view assets, std::string_view language);

}  // namespace ssg
