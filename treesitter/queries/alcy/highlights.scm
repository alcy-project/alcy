; Highlighting for alcy.
;
; The captures name the kind of thing rather than its spelling, so a theme
; decides what a keyword and a builtin look like. A node with a field says
; what it is (`name`, `field`, `value`); an anonymous token names itself.

; A declaration's name is the thing the declaration introduces. An
; identifier that matches nothing above is a plain variable, which is the
; last capture here so that it loses to the ones that mean more.
(function_item name: (identifier) @function)
(intrinsic_fn_item name: (identifier) @function)
(spec_method name: (identifier) @function.method)
(method_call_expression name: (identifier) @function.method)

(struct_item name: (identifier) @type)
(enum_item name: (identifier) @type)
(spec_item name: (identifier) @type)
(path_type (path (identifier) @type))
(type_parameters (identifier) @type.parameter)

(enum_variant name: (identifier) @constructor)
(field_declaration name: (identifier) @property)
(field_pattern name: (identifier) @property)
(field_initializer name: (identifier) @property)

(static_item name: (identifier) @constant)
(const_item name: (identifier) @constant)
(use_item alias: (identifier) @namespace)

(type_parameters (identifier) @type.parameter)

; Keywords -------------------------------------------------------------------

[
  "fn"
  "intrinsic"
  "struct"
  "enum"
  "impl"
  "spec"
  "static"
  "const"
  "use"
] @keyword

; `pub` is a named node of its own, so it is captured by name rather than
; as the anonymous token under it.
(visibility) @keyword

[
  "if"
  "else"
  "loop"
  "while"
  "for"
  "in"
  "ret"
  "match"
  "comp"
] @keyword.control

; `break` and `continue` are whole nodes rather than anonymous tokens,
; because each names an expression.
(break_expression) @keyword.control
(continue_expression) @keyword.control

; `self`, `Self`, `super`, and `package` are named nodes, since each is a
; path segment rather than an anonymous token.
"as" @keyword.import

(self) @variable.builtin
(self_type) @type.builtin
(super) @keyword.import
(package) @keyword.import

[
  "mut"
  "const"
] @keyword.modifier

; Types ----------------------------------------------------------------------

(primitive_type) @type.builtin
(str_type) @type.builtin
(never_type) @type.builtin
(unit_type) @type.builtin
(self) @variable.builtin
(self_type) @type.builtin

; Literals ------------------------------------------------------------------

(string) @string
(char) @character
(integer) @number
(float) @number
(boolean) @constant.builtin

[
  (line_comment)
  (block_comment)
] @comment
(doc_comment) @comment.documentation

; A `_` in a pattern is a discard rather than a name.
(wildcard_pattern) @variable.builtin

; Operators and punctuation --------------------------------------------------

[
  "+"
  "-"
  "*"
  "/"
  "%"
  "**"
  "&"
  "|"
  "^"
  "~"
  "<<"
  ">>"
  "&&"
  "||"
  "!"
  "=="
  "!="
  ">"
  "<"
  ">="
  "<="
  "="
  "+="
  "-="
  "*="
  "/="
  "%="
  "**="
  "&="
  "|="
  "^="
  "<<="
  ">>="
  ":="
  "->"
  "=>"
  ".."
  "..="
  "..<"
  "?"
] @operator

(binary_expression operator: _ @operator)
(comparison_expression operator: _ @operator)
(unary_expression operator: _ @operator)
(range_expression operator: _ @operator)
(assignment_statement operator: _ @operator)

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

[
  ","
  "."
  ":"
  "::"
  ";"
] @punctuation.delimiter

; The fallback, last on purpose: an earlier capture for the same node wins,
; and this one says the least.
(identifier) @variable
