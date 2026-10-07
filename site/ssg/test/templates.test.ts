// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { equal, throws } from "node:assert/strict";
import { test } from "node:test";

import { stampTemplate } from "../src/templates.js";

test("catalog strings are stamped in the page's language", () => {
  equal(stampTemplate("<h1>{{t:site.guide}}</h1>", "ja", {}), "<h1>ガイド</h1>");
  equal(stampTemplate("<h1>{{t:site.guide}}</h1>", "en", {}), "<h1>Guide</h1>");
});

test("computed values are inserted", () => {
  equal(
    stampTemplate('<a href="{{home}}guide/">{{lang}}</a>', "ja", {
      home: "./",
      lang: "ja",
    }),
    '<a href="./guide/">ja</a>',
  );
});

test("an unknown message key fails the build", () => {
  throws(() => stampTemplate("{{t:no.such.key}}", "en", {}));
});

test("an unknown placeholder fails the build", () => {
  throws(() => stampTemplate("{{nope}}", "en", {}));
});
