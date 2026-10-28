// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/server.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "lsp/analysis.h"
#include "lsp/analysis_cache.h"
#include "lsp/code_actions.h"
#include "lsp/completion.h"
#include "lsp/diagnostics.h"
#include "lsp/document.h"
#include "lsp/folding.h"
#include "lsp/formatting.h"
#include "lsp/highlights.h"
#include "lsp/hover.h"
#include "lsp/inlay_hints.h"
#include "lsp/layout.h"
#include "lsp/navigation.h"
#include "lsp/protocol.h"
#include "lsp/semantic_tokens.h"
#include "lsp/signature.h"
#include "lsp/symbols.h"
#include "lsp/transport.h"
#include "text/json.h"

namespace lsp {

namespace {

// JSON-RPC and LSP error codes that cross the wire.
constexpr i32 PARSE_ERROR = -32700;
constexpr i32 INVALID_REQUEST = -32600;
constexpr i32 METHOD_NOT_FOUND = -32601;
constexpr i32 SERVER_NOT_INITIALIZED = -32002;

const text::JsonValue* field(const text::JsonValue& object,
                             std::string_view key) {
  return object.find(key);
}

std::string_view string_field(const text::JsonValue& object,
                              std::string_view key) {
  const text::JsonValue* value = field(object, key);
  return value == nullptr ? std::string_view{} : value->string_or("");
}

i64 int_field(const text::JsonValue& object,
              std::string_view key,
              i64 fallback) {
  const text::JsonValue* value = field(object, key);
  if (value == nullptr || !value->is_number()) {
    return fallback;
  }
  return static_cast<i64>(value->as_number());
}

Position read_position(const text::JsonValue& value) {
  return Position{static_cast<u32>(int_field(value, "line", 0)),
                  static_cast<u32>(int_field(value, "character", 0))};
}

ContentChange read_change(const text::JsonValue& value) {
  ContentChange change;
  change.text = std::string(string_field(value, "text"));
  const text::JsonValue* range = field(value, "range");
  if (range == nullptr || !range->is_object()) {
    return change;
  }
  const text::JsonValue* start = field(*range, "start");
  const text::JsonValue* end = field(*range, "end");
  if (start == nullptr || end == nullptr) {
    return change;
  }
  change.has_range = true;
  change.range = Range{read_position(*start), read_position(*end)};
  return change;
}

text::JsonValue make_string(std::string_view value) {
  return text::JsonValue::make_string(std::string(value));
}

}  // namespace

i32 Server::run() {
  std::string payload;
  while (!exit_ && transport_.read_message(&payload)) {
    handle(payload);
  }
  if (transport_.failed()) {
    return 1;
  }
  return exit_code_;
}

void Server::handle(std::string_view payload) {
  base::Result<text::JsonValue, text::JsonError> parsed =
      text::parse_json(payload);
  if (parsed.is_err()) {
    send_error(text::JsonValue{}, PARSE_ERROR, "parse error");
    return;
  }
  const text::JsonValue& message = parsed.value();
  if (!message.is_object()) {
    send_error(text::JsonValue{}, INVALID_REQUEST, "invalid request");
    return;
  }
  if (field(message, "id") != nullptr) {
    handle_request(message);
  } else {
    handle_notification(message);
  }
}

void Server::handle_request(const text::JsonValue& message) {
  const text::JsonValue* id = field(message, "id");
  const std::string_view method = string_field(message, "method");
  if (method.empty()) {
    send_error(*id, INVALID_REQUEST, "request has no method");
    return;
  }
  if (method == "initialize") {
    initialize(*id);
    return;
  }
  if (!initialized_) {
    send_error(*id, SERVER_NOT_INITIALIZED, "server not initialized");
    return;
  }
  if (shutdown_) {
    send_error(*id, INVALID_REQUEST, "server is shutting down");
    return;
  }
  if (method == "shutdown") {
    shutdown(*id);
    return;
  }
  if (method == "textDocument/formatting") {
    const text::JsonValue* params = field(message, "params");
    if (params == nullptr || !params->is_object()) {
      send_error(*id, INVALID_REQUEST, "formatting needs params");
      return;
    }
    formatting(*id, *params);
    return;
  }
  if (method == "textDocument/foldingRange") {
    const text::JsonValue* params = field(message, "params");
    if (params == nullptr || !params->is_object()) {
      send_error(*id, INVALID_REQUEST, "foldingRange needs params");
      return;
    }
    folding_range(*id, *params);
    return;
  }
  if (method == "textDocument/documentSymbol") {
    const text::JsonValue* params = field(message, "params");
    if (params == nullptr || !params->is_object()) {
      send_error(*id, INVALID_REQUEST, "documentSymbol needs params");
      return;
    }
    document_symbol(*id, *params);
    return;
  }
  if (method == "textDocument/definition" ||
      method == "textDocument/references") {
    const text::JsonValue* params = field(message, "params");
    if (params == nullptr || !params->is_object()) {
      send_error(*id, INVALID_REQUEST, "navigation needs params");
      return;
    }
    if (method == "textDocument/definition") {
      definition(*id, *params);
    } else {
      references(*id, *params);
    }
    return;
  }
  if (method == "textDocument/documentHighlight") {
    const text::JsonValue* params = field(message, "params");
    if (params == nullptr || !params->is_object()) {
      send_error(*id, INVALID_REQUEST, "documentHighlight needs params");
      return;
    }
    document_highlight(*id, *params);
    return;
  }
  if (method == "textDocument/diagnostic") {
    const text::JsonValue* params = field(message, "params");
    if (params == nullptr || !params->is_object()) {
      send_error(*id, INVALID_REQUEST, "diagnostic needs params");
      return;
    }
    diagnostic(*id, *params);
    return;
  }
  if (method == "textDocument/semanticTokens/full") {
    const text::JsonValue* params = field(message, "params");
    if (params == nullptr || !params->is_object()) {
      send_error(*id, INVALID_REQUEST, "semanticTokens needs params");
      return;
    }
    semantic_tokens_full(*id, *params);
    return;
  }
  if (method == "textDocument/codeAction") {
    const text::JsonValue* params = field(message, "params");
    if (params == nullptr || !params->is_object()) {
      send_error(*id, INVALID_REQUEST, "codeAction needs params");
      return;
    }
    code_action(*id, *params);
    return;
  }
  if (method == "textDocument/inlayHint") {
    const text::JsonValue* params = field(message, "params");
    if (params == nullptr || !params->is_object()) {
      send_error(*id, INVALID_REQUEST, "inlayHint needs params");
      return;
    }
    inlay_hint(*id, *params);
    return;
  }
  if (method == "textDocument/completion" ||
      method == "textDocument/signatureHelp") {
    const text::JsonValue* params = field(message, "params");
    if (params == nullptr || !params->is_object()) {
      send_error(*id, INVALID_REQUEST, "a completion query needs params");
      return;
    }
    if (method == "textDocument/completion") {
      completion(*id, *params);
    } else {
      signature(*id, *params);
    }
    return;
  }
  if (method == "textDocument/hover" || method == "alcy/structLayout") {
    const text::JsonValue* params = field(message, "params");
    if (params == nullptr || !params->is_object()) {
      send_error(*id, INVALID_REQUEST, "a layout query needs params");
      return;
    }
    if (method == "textDocument/hover") {
      hover(*id, *params);
    } else {
      struct_layout(*id, *params);
    }
    return;
  }
  send_error(*id, METHOD_NOT_FOUND, "method not found");
}

void Server::handle_notification(const text::JsonValue& message) {
  const std::string_view method = string_field(message, "method");
  if (method == "exit") {
    exit_ = true;
    exit_code_ = shutdown_ ? 0 : 1;
    return;
  }
  if (method == "initialized") {
    initialized_ = true;
    return;
  }
  if (!initialized_ || shutdown_) {
    return;
  }
  const text::JsonValue* params = field(message, "params");
  if (params == nullptr || !params->is_object()) {
    return;
  }
  if (method == "textDocument/didOpen") {
    did_open(*params);
  } else if (method == "textDocument/didChange") {
    did_change(*params);
  } else if (method == "textDocument/didClose") {
    did_close(*params);
  }
}

void Server::initialize(const text::JsonValue& id) {
  if (initialized_) {
    send_error(id, INVALID_REQUEST, "server is already initialized");
    return;
  }
  text::JsonValue sync = text::JsonValue::make_object();
  sync.set("openClose", text::JsonValue::make_bool(true));
  // Full saves are not requested; the buffer the client holds is the
  // source of truth. Incremental changes keep a large file inexpensive.
  sync.set("change", text::JsonValue::make_number(2));
  sync.set("save", text::JsonValue::make_bool(false));

  text::JsonValue capabilities = text::JsonValue::make_object();
  capabilities.set("textDocumentSync", std::move(sync));
  // Position encoding defaults to UTF-16; advertising it keeps the
  // choice explicit for clients that read it.
  capabilities.set("positionEncoding", make_string("utf-16"));
  capabilities.set("documentFormattingProvider",
                   text::JsonValue::make_bool(true));
  capabilities.set("documentSymbolProvider", text::JsonValue::make_bool(true));
  capabilities.set("foldingRangeProvider", text::JsonValue::make_bool(true));
  capabilities.set("hoverProvider", text::JsonValue::make_bool(true));
  capabilities.set("definitionProvider", text::JsonValue::make_bool(true));
  capabilities.set("referencesProvider", text::JsonValue::make_bool(true));
  text::JsonValue completion_options = text::JsonValue::make_object();
  text::JsonValue completion_triggers = text::JsonValue::make_array();
  completion_triggers.push(make_string("."));
  completion_triggers.push(make_string(":"));
  completion_options.set("triggerCharacters", std::move(completion_triggers));
  capabilities.set("completionProvider", std::move(completion_options));
  text::JsonValue signature_options = text::JsonValue::make_object();
  text::JsonValue signature_triggers = text::JsonValue::make_array();
  signature_triggers.push(make_string("("));
  signature_triggers.push(make_string(","));
  signature_options.set("triggerCharacters", std::move(signature_triggers));
  capabilities.set("signatureHelpProvider", std::move(signature_options));
  capabilities.set("inlayHintProvider", text::JsonValue::make_bool(true));
  capabilities.set("codeActionProvider", text::JsonValue::make_bool(true));
  text::JsonValue semantic = text::JsonValue::make_object();
  text::JsonValue legend = text::JsonValue::make_object();
  legend.set("tokenTypes", semantic_token_types());
  legend.set("tokenModifiers", text::JsonValue::make_array());
  semantic.set("legend", std::move(legend));
  semantic.set("full", text::JsonValue::make_bool(true));
  capabilities.set("semanticTokensProvider", std::move(semantic));
  capabilities.set("documentHighlightProvider",
                   text::JsonValue::make_bool(true));
  text::JsonValue diagnostic_provider = text::JsonValue::make_object();
  diagnostic_provider.set("interFileDependencies",
                          text::JsonValue::make_bool(false));
  diagnostic_provider.set("workspaceDiagnostics",
                          text::JsonValue::make_bool(false));
  capabilities.set("diagnosticProvider", std::move(diagnostic_provider));
  text::JsonValue experimental = text::JsonValue::make_object();
  experimental.set("alcy/structLayout", text::JsonValue::make_bool(true));
  capabilities.set("experimental", std::move(experimental));

  text::JsonValue server_info = text::JsonValue::make_object();
  server_info.set("name", make_string("alcy-lsp"));
  server_info.set("version", make_string("0.1.0"));

  text::JsonValue result = text::JsonValue::make_object();
  result.set("capabilities", std::move(capabilities));
  result.set("serverInfo", std::move(server_info));

  initialized_ = true;
  send_result(id, std::move(result));
}

void Server::shutdown(const text::JsonValue& id) {
  shutdown_ = true;
  send_result(id, text::JsonValue{});
}

const Document* Server::open_document_at(const text::JsonValue& params,
                                         Position* position) const {
  const text::JsonValue* text_document = field(params, "textDocument");
  const text::JsonValue* raw_position = field(params, "position");
  if (text_document == nullptr || !text_document->is_object() ||
      raw_position == nullptr || !raw_position->is_object()) {
    return nullptr;
  }
  *position = read_position(*raw_position);
  return documents_.find(string_field(*text_document, "uri"));
}

void Server::folding_range(const text::JsonValue& id,
                           const text::JsonValue& params) {
  const text::JsonValue* text_document = field(params, "textDocument");
  if (text_document == nullptr || !text_document->is_object()) {
    send_error(id, INVALID_REQUEST, "foldingRange needs a text document");
    return;
  }
  const Document* document =
      documents_.find(string_field(*text_document, "uri"));
  if (document == nullptr) {
    send_result(id, text::JsonValue::make_array());
    return;
  }
  send_result(id, document_folding_ranges(*document));
}

void Server::document_symbol(const text::JsonValue& id,
                             const text::JsonValue& params) {
  const text::JsonValue* text_document = field(params, "textDocument");
  if (text_document == nullptr || !text_document->is_object()) {
    send_error(id, INVALID_REQUEST, "documentSymbol needs a text document");
    return;
  }
  const Document* document =
      documents_.find(string_field(*text_document, "uri"));
  if (document == nullptr) {
    send_result(id, text::JsonValue::make_array());
    return;
  }
  send_result(id, document_symbols(*document));
}

void Server::hover(const text::JsonValue& id, const text::JsonValue& params) {
  Position position;
  const Document* document = open_document_at(params, &position);
  if (document == nullptr) {
    send_result(id, text::JsonValue{});
    return;
  }
  const DocumentAnalysis& analysis = cache_.get(*document);
  const std::optional<std::string> markdown =
      hover_markdown(*document, analysis, position);
  if (!markdown.has_value()) {
    send_result(id, text::JsonValue{});
    return;
  }
  text::JsonValue contents = text::JsonValue::make_object();
  contents.set("kind", make_string("markdown"));
  contents.set("value", make_string(*markdown));
  text::JsonValue result = text::JsonValue::make_object();
  result.set("contents", std::move(contents));
  send_result(id, std::move(result));
}

void Server::definition(const text::JsonValue& id,
                        const text::JsonValue& params) {
  Position position;
  const Document* document = open_document_at(params, &position);
  if (document == nullptr) {
    send_result(id, text::JsonValue{});
    return;
  }
  const DocumentAnalysis& analysis = cache_.get(*document);
  const std::optional<diag::Span> location =
      definition_at(*document, analysis, position);
  if (!location.has_value()) {
    send_result(id, text::JsonValue{});
    return;
  }
  send_result(id, location_json(*document, *location));
}

void Server::references(const text::JsonValue& id,
                        const text::JsonValue& params) {
  Position position;
  const Document* document = open_document_at(params, &position);
  if (document == nullptr) {
    send_result(id, text::JsonValue::make_array());
    return;
  }
  bool include_declaration = true;
  if (const text::JsonValue* context = field(params, "context");
      context != nullptr && context->is_object()) {
    if (const text::JsonValue* include = context->find("includeDeclaration");
        include != nullptr && include->is_bool()) {
      include_declaration = include->as_bool();
    }
  }
  const DocumentAnalysis& analysis = cache_.get(*document);
  text::JsonValue result = text::JsonValue::make_array();
  if (include_declaration) {
    if (const std::optional<diag::Span> location =
            definition_at(*document, analysis, position)) {
      result.push(location_json(*document, *location));
    }
  }
  for (const diag::Span site : references_at(*document, analysis, position)) {
    result.push(location_json(*document, site));
  }
  send_result(id, std::move(result));
}

void Server::completion(const text::JsonValue& id,
                        const text::JsonValue& params) {
  Position position;
  const Document* document = open_document_at(params, &position);
  if (document == nullptr) {
    send_result(id, text::JsonValue::make_array());
    return;
  }
  send_result(id, completion_items(*document, cache_.get(*document), position));
}

void Server::signature(const text::JsonValue& id,
                       const text::JsonValue& params) {
  Position position;
  const Document* document = open_document_at(params, &position);
  if (document == nullptr) {
    send_result(id, text::JsonValue{});
    return;
  }
  send_result(id, signature_help(*document, cache_.get(*document), position));
}

void Server::inlay_hint(const text::JsonValue& id,
                        const text::JsonValue& params) {
  const text::JsonValue* text_document = field(params, "textDocument");
  if (text_document == nullptr || !text_document->is_object()) {
    send_result(id, text::JsonValue::make_array());
    return;
  }
  const Document* document =
      documents_.find(string_field(*text_document, "uri"));
  if (document == nullptr) {
    send_result(id, text::JsonValue::make_array());
    return;
  }
  send_result(id, inlay_hints(*document, cache_.get(*document)));
}

void Server::code_action(const text::JsonValue& id,
                         const text::JsonValue& params) {
  const text::JsonValue* text_document = field(params, "textDocument");
  if (text_document == nullptr || !text_document->is_object()) {
    send_result(id, text::JsonValue::make_array());
    return;
  }
  const Document* document =
      documents_.find(string_field(*text_document, "uri"));
  const text::JsonValue* range = field(params, "range");
  if (document == nullptr || range == nullptr || !range->is_object()) {
    send_result(id, text::JsonValue::make_array());
    return;
  }
  const text::JsonValue* start = range->find("start");
  const text::JsonValue* end = range->find("end");
  if (start == nullptr || end == nullptr) {
    send_result(id, text::JsonValue::make_array());
    return;
  }
  send_result(
      id, code_actions(*document, read_position(*start), read_position(*end)));
}

void Server::document_highlight(const text::JsonValue& id,
                                const text::JsonValue& params) {
  Position position;
  const Document* document = open_document_at(params, &position);
  if (document == nullptr) {
    send_result(id, text::JsonValue::make_array());
    return;
  }
  send_result(id,
              document_highlights(*document, cache_.get(*document), position));
}

void Server::diagnostic(const text::JsonValue& id,
                        const text::JsonValue& params) {
  const text::JsonValue* text_document = field(params, "textDocument");
  if (text_document == nullptr || !text_document->is_object()) {
    send_result(id, text::JsonValue{});
    return;
  }
  const Document* document =
      documents_.find(string_field(*text_document, "uri"));
  if (document == nullptr) {
    send_result(id, text::JsonValue{});
    return;
  }
  send_result(id, document_diagnostic_report(*document));
}

void Server::semantic_tokens_full(const text::JsonValue& id,
                                  const text::JsonValue& params) {
  const text::JsonValue* text_document = field(params, "textDocument");
  if (text_document == nullptr || !text_document->is_object()) {
    send_result(id, semantic_tokens_empty());
    return;
  }
  const Document* document =
      documents_.find(string_field(*text_document, "uri"));
  if (document == nullptr) {
    send_result(id, semantic_tokens_empty());
    return;
  }
  send_result(id, semantic_tokens(*document, cache_.get(*document)));
}

void Server::struct_layout(const text::JsonValue& id,
                           const text::JsonValue& params) {
  Position position;
  const Document* document = open_document_at(params, &position);
  if (document == nullptr) {
    send_result(id, text::JsonValue{});
    return;
  }
  const std::optional<StructLayout> layout =
      struct_layout_at(*document, cache_.get(*document), position);
  if (!layout.has_value()) {
    send_result(id, text::JsonValue{});
    return;
  }
  send_result(id, struct_layout_json(*layout));
}

void Server::did_open(const text::JsonValue& params) {
  const text::JsonValue* text_document = field(params, "textDocument");
  if (text_document == nullptr || !text_document->is_object()) {
    return;
  }
  Document& document =
      documents_.open(std::string(string_field(*text_document, "uri")),
                      std::string(string_field(*text_document, "languageId")),
                      int_field(*text_document, "version", 0),
                      std::string(string_field(*text_document, "text")));
  publish_diagnostics(document);
}

void Server::did_change(const text::JsonValue& params) {
  const text::JsonValue* text_document = field(params, "textDocument");
  if (text_document == nullptr || !text_document->is_object()) {
    return;
  }
  const std::string_view uri = string_field(*text_document, "uri");
  Document* document = documents_.find(uri);
  if (document == nullptr) {
    return;
  }
  std::vector<ContentChange> changes;
  const text::JsonValue* list = field(params, "contentChanges");
  if (list != nullptr && list->is_array()) {
    for (const text::JsonValue& item : list->as_array()) {
      if (item.is_object()) {
        changes.push_back(read_change(item));
      }
    }
  }
  document->apply(changes,
                  int_field(*text_document, "version", document->version()));
  publish_diagnostics(*document);
}

void Server::did_close(const text::JsonValue& params) {
  const text::JsonValue* text_document = field(params, "textDocument");
  if (text_document == nullptr || !text_document->is_object()) {
    return;
  }
  const std::string_view uri = string_field(*text_document, "uri");
  documents_.close(uri);
  cache_.erase(uri);
  publish_empty_diagnostics(uri);
}

void Server::formatting(const text::JsonValue& id,
                        const text::JsonValue& params) {
  const text::JsonValue* text_document = field(params, "textDocument");
  if (text_document == nullptr || !text_document->is_object()) {
    send_error(id, INVALID_REQUEST, "formatting needs a text document");
    return;
  }
  const Document* document =
      documents_.find(string_field(*text_document, "uri"));
  if (document == nullptr) {
    send_error(id, INVALID_REQUEST, "document is not open");
    return;
  }
  send_result(id, document_formatting(*document));
}

void Server::publish_diagnostics(const Document& document) {
  text::JsonValue params = text::JsonValue::make_object();
  params.set("uri", make_string(document.uri()));
  params.set("diagnostics", document_diagnostics(document));
  send_notification("textDocument/publishDiagnostics", std::move(params));
}

void Server::publish_empty_diagnostics(std::string_view uri) {
  text::JsonValue params = text::JsonValue::make_object();
  params.set("uri", make_string(uri));
  params.set("diagnostics", text::JsonValue::make_array());
  send_notification("textDocument/publishDiagnostics", std::move(params));
}

void Server::send_result(const text::JsonValue& id, text::JsonValue result) {
  text::JsonValue message = text::JsonValue::make_object();
  message.set("jsonrpc", make_string("2.0"));
  message.set("id", id);
  message.set("result", std::move(result));
  transport_.write_message(text::to_json(message));
}

void Server::send_error(const text::JsonValue& id,
                        i32 code,
                        std::string_view message_text) {
  text::JsonValue error = text::JsonValue::make_object();
  error.set("code", text::JsonValue::make_number(static_cast<f64>(code)));
  error.set("message", make_string(message_text));

  text::JsonValue message = text::JsonValue::make_object();
  message.set("jsonrpc", make_string("2.0"));
  message.set("id", id);
  message.set("error", std::move(error));
  transport_.write_message(text::to_json(message));
}

void Server::send_notification(std::string_view method,
                               text::JsonValue params) {
  text::JsonValue message = text::JsonValue::make_object();
  message.set("jsonrpc", make_string("2.0"));
  message.set("method", make_string(method));
  message.set("params", std::move(params));
  transport_.write_message(text::to_json(message));
}

}  // namespace lsp
