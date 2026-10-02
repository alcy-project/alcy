; Code navigation for alcy: what is defined here and what is referred to.

; Definitions ---------------------------------------------------------------

(function_item
  name: (identifier) @name) @definition.function

(intrinsic_fn_item
  name: (identifier) @name) @definition.function

(spec_method
  name: (identifier) @name) @definition.method

(struct_item
  name: (identifier) @name) @definition.class

(enum_item
  name: (identifier) @name) @definition.class

(enum_variant
  name: (identifier) @name) @definition.constant

(spec_item
  name: (identifier) @name) @definition.interface

(type_parameters
  (identifier) @name) @definition.type.parameter

(field_declaration
  name: (identifier) @name) @definition.field

(static_item
  name: (identifier) @name) @definition.constant

(const_item
  name: (identifier) @name) @definition.constant

(parameter
  pattern: (identifier_pattern
    name: (identifier) @name)) @definition.parameter

(parameter
  pattern: (mut_identifier_pattern
    name: (identifier) @name)) @definition.parameter

(declaration_statement
  pattern: (identifier_pattern
    name: (identifier) @name)) @definition.variable

(declaration_statement
  pattern: (mut_identifier_pattern
    name: (identifier) @name)) @definition.variable

; References -----------------------------------------------------------------

; A path that is the whole callee of a call names the function being called.
(call_expression
  function: (path_expression
    path: (path
      (identifier) @name))) @reference.call

(method_call_expression
  name: (identifier) @name) @reference.call

; A qualified path names the item it ends in.
(path_expression
  path: (path
    (identifier) @name .)) @reference.call

(path_expression
  path: (path
    . (identifier) @name)) @reference.call

; A field or a variant reached through a path.
(struct_expression
  path: (path
    (identifier) @name)) @reference.class

(tuple_variant_pattern
  path: (path
    (identifier) @name)) @reference.class

(struct_pattern
  path: (path
    (identifier) @name)) @reference.class

(unit_variant_pattern
  path: (path
    (identifier) @name)) @reference.class

; An import names what it brings in.
(use_item
  path: (path
    (identifier) @name)) @reference.module

(use_item
  alias: (identifier) @name) @definition.module
