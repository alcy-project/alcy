// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/server.h"

#include <string>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "lsp/diagnostics.h"
#include "lsp/document.h"
#include "lsp/formatting.h"
#include "lsp/frame.h"
#include "lsp/transport.h"
#include "text/json.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace lsp {

#if !defined(_WIN32)

namespace {

std::string read_all(i32 fd) {
  std::string out;
  char buffer[4096];
  for (;;) {
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count <= 0) {
      break;
    }
    out.append(buffer, static_cast<usize>(count));
  }
  return out;
}

// Feeds \`requests\` to a server and returns everything it wrote. The
// input is closed first, so the server sees end of input after the last
// message.
std::string serve(const std::string& requests, i32* exit_code) {
  i32 input[2];
  i32 output[2];
  CHECK(::pipe(input) == 0);
  CHECK(::pipe(output) == 0);
  CHECK(::write(input[1], requests.data(), requests.size()) ==
        static_cast<ssize_t>(requests.size()));
  ::close(input[1]);

  Transport transport(input[0], output[1]);
  Server server(transport);
  *exit_code = server.run();

  ::close(output[1]);
  const std::string response = read_all(output[0]);
  ::close(output[0]);
  ::close(input[0]);
  return response;
}

}  // namespace

TEST_CASE("The server answers initialize and exits cleanly on shutdown") {
  std::string requests;
  append_frame(requests,
               R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
  append_frame(requests,
               R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
  append_frame(requests,
               R"({"jsonrpc":"2.0","id":2,"method":"shutdown","params":null})");
  append_frame(requests, R"({"jsonrpc":"2.0","method":"exit"})");

  i32 exit_code = -1;
  const std::string response = serve(requests, &exit_code);

  CHECK(exit_code == 0);
  CHECK(response.find(R"("id":1)") != std::string::npos);
  CHECK(response.find(R"("capabilities")") != std::string::npos);
  CHECK(response.find(R"("id":2)") != std::string::npos);
}

TEST_CASE("The server refuses a request before initialize") {
  std::string requests;
  append_frame(requests,
               R"({"jsonrpc":"2.0","id":7,"method":"shutdown","params":null})");
  append_frame(requests, R"({"jsonrpc":"2.0","method":"exit"})");

  i32 exit_code = -1;
  const std::string response = serve(requests, &exit_code);

  // No shutdown was accepted, so exit is an abnormal one.
  CHECK(exit_code == 1);
  CHECK(response.find("-32002") != std::string::npos);
}

TEST_CASE("The server publishes diagnostics when a document opens") {
  std::string requests;
  append_frame(requests,
               R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
  append_frame(
      requests,
      R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///broken.al","languageId":"alcy","version":1,"text":"fn main( -> i32 {\n}\n"}}})");
  append_frame(requests,
               R"({"jsonrpc":"2.0","id":2,"method":"shutdown","params":null})");
  append_frame(requests, R"({"jsonrpc":"2.0","method":"exit"})");

  i32 exit_code = -1;
  const std::string response = serve(requests, &exit_code);

  CHECK(exit_code == 0);
  CHECK(response.find("textDocument/publishDiagnostics") != std::string::npos);
  CHECK(response.find(R"("severity")") != std::string::npos);
}

#endif

TEST_CASE("A clean buffer reports nothing and a broken one reports") {
  const Document clean("file:///clean.al", "alcy", 1,
                       "struct Point { x: i32 }");
  const text::JsonValue empty = document_diagnostics(clean);
  CHECK(empty.is_array());
  CHECK(empty.as_array().empty());

  const Document broken("file:///broken.al", "alcy", 1,
                        "fn main( -> i32 {\n}\n");
  const text::JsonValue reported = document_diagnostics(broken);
  CHECK(reported.is_array());
  CHECK(!reported.as_array().empty());
}

TEST_CASE("A lint finding is published as a diagnostic") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  ret 0\n}\n");
  const text::JsonValue diagnostics = document_diagnostics(document);
  CHECK(diagnostics.is_array());
  CHECK(!diagnostics.as_array().empty());
}

TEST_CASE("Formatting replaces a buffer that is not canonical") {
  const Document document("file:///point.al", "alcy", 1,
                          "struct  Point{x:i32}");
  const text::JsonValue edits = document_formatting(document);
  CHECK(edits.is_array());
  CHECK(!edits.as_array().empty());
}

}  // namespace lsp
