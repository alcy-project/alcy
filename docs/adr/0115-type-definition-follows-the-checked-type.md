# ADR-0115: Type definition follows the checked type

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

An editor asks where a value's type is declared, and the answer should
be the type the compiler gave the value, not the type the source
happened to spell.

## Decision

- The type comes from the checker: checked_expr_type on the innermost
  expression, or the innermost written type when the position sits in an
  annotation.
- A reference is looked through to its pointee.
- A struct or enum type is mapped to its declaration through the
  checker's own StructInfo or EnumInfo, and the declaration is kept only
  when it is in the root file, as every other span is.
- A primitive, tuple, or function type has no declaration, so the
  request answers null.

## Consequences

A variable, a field, or a call result jumps to the declaration of its
checked type, including through a reference and through a generic
instantiation, whose instance names the same nominal. A written type
jumps to its declaration by the path's last segment, which is the name
the checker registered.
