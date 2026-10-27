// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "fpag/base/numeric.h"
#include "lsp/document.h"
#include "lsp/transport.h"
#include "text/json.h"

namespace lsp {

// The LSP message loop: reads frames, routes each JSON-RPC request or
// notification, and writes the responses and diagnostics. One instance
// serves one client over one transport.
class Server {
 public:
  explicit Server(Transport& transport) : transport_(transport) {}

  // Runs until the client sends `exit` or closes the input, and returns
  // the process exit code the protocol asks for.
  i32 run();

 private:
  void handle(std::string_view payload);
  void handle_request(const text::JsonValue& message);
  void handle_notification(const text::JsonValue& message);
  void initialize(const text::JsonValue& id);
  void shutdown(const text::JsonValue& id);
  void did_open(const text::JsonValue& params);
  void did_change(const text::JsonValue& params);
  void did_close(const text::JsonValue& params);
  void formatting(const text::JsonValue& id, const text::JsonValue& params);
  void hover(const text::JsonValue& id, const text::JsonValue& params);
  void struct_layout(const text::JsonValue& id, const text::JsonValue& params);
  // The open document a request names, with its position read out.
  const Document* open_document_at(const text::JsonValue& params,
                                   Position* position) const;
  void publish_diagnostics(const Document& document);
  void publish_empty_diagnostics(std::string_view uri);
  void send_result(const text::JsonValue& id, text::JsonValue result);
  void send_error(const text::JsonValue& id,
                  i32 code,
                  std::string_view message);
  void send_notification(std::string_view method, text::JsonValue params);

  Transport& transport_;
  DocumentStore documents_;
  bool initialized_ = false;
  bool shutdown_ = false;
  bool exit_ = false;
  i32 exit_code_ = 0;
};

}  // namespace lsp
