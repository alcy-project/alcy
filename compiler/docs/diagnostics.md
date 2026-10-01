# Diagnostic codes

Numeric codes identify compiler diagnostics (`error[E<code>]`,
`warning[W<code>]`). This document is the single registry: every code
is assigned here, and definitions in code must match it. Values grow
along the compile pipeline so that early-stage failures sort before
late-stage ones:

| Range     | Stage       | Owner                         |
| --------- | ----------- | ----------------------------- |
| 0–999     | tests       | Synthetic codes for unit tests; never emitted by the compiler itself |
| 1000–1999 | pkg         | Manifests, modules, resolution |
| 2000–2999 | lexer       | `compiler/lexer/lexer.cc`          |
| 3000–3999 | parser      | `compiler/parser/parser.h`, `compiler/parser/desugar.cc` |
| 4000–4999 | analyzer    | `compiler/analyzer/resolve.*`, `compiler/analyzer/checker.h` |
| 5000–5999 | lowering    | `compiler/lowering/lowerer.h`   |
| 6000–6999 | borrow      | `compiler/borrow/borrow.cc`        |
| 7000–7999 | ir          | `compiler/ir/verifier.h` (7100+, one per `VerificationErrorKind`) |
| 8000–8999 | pipeline    | `compiler/pipeline/pipeline_context.h` |
| 9000+     | future      | Unassigned                    |

Each stage owns a stride of 1000 with sub-ranges of 100 per area, so
new checks fit without renumbering. Adding a code means claiming the
next free value in the owning area here first, then defining it.

A code is not a message. It identifies a check for whatever reads the
output, and most checks say more than one thing: `2000` is an invalid
character and an empty character literal, `4024` covers a missing
wildcard arm, an uncovered variant, and a missing variant by name. The
text of each of those is a separate entry in `compiler/i18n/messages.def`,
keyed by an `i18n::Key` that names what is wrong rather than where it
happened, because a translation is keyed by the message and half the
codes here have no single wording to key it with. A code stays at the
call site, where the check that found it lives.

## pkg (1000–1999)

Manifests (`compiler/pkg/manifest.cc`, 1000–1099):

- `1000` syntax error: the manifest does not parse.
- `1001` semantic error: parsed but invalid (see `ManifestError`).

Modules (`compiler/pkg/modules.cc`, 1100–1199):

- `1100` semantic error: a module entry selects nothing or conflicts.
- `1101` unselected file: a source file belongs to no module.

Resolution (`compiler/pkg/resolve.cc`, 1200–1299):

- `1200` I/O error: the package root or manifest cannot be read.
- `1201` cycle error: package dependencies form a cycle.

## lexer (2000–2099)

`compiler/lexer/lexer.cc`:

- `2000` invalid character.
- `2001` unterminated string.
- `2002` unterminated character literal.
- `2003` invalid number.
- `2004` unterminated block comment.
- `2005` invalid escape.

## parser (3000–3999)

Grammar (`compiler/parser/parser.h`, 3000–3099):

- `3000` unexpected token.
- `3001` reserved word used as an identifier.
- `3002` internal error: the token stream failed structural
  verification (see `lexer::verify_token_stream`).
- `3003` internal error: the parsed arena failed structural
  verification (see `ast::verify_file`).

Desugar (`compiler/parser/desugar.cc`, 3100–3199):

- `3100` `or`-pattern alternatives bind different name sets.
- `3101` a name is already bound in the innermost scope.

## analyzer (4000–4999)

Module resolution (`compiler/analyzer/resolve.cc`, `compiler/analyzer/resolve.h`,
4000–4009):

- `4000` duplicate module.
- `4001` unresolved import.
- `4002` ambiguous import.
- `4003` unreachable file (warning).
- `4004` invalid path: an unknown file id reached resolution.
- `4005` internal error: the module tree failed structural
  verification (see `analyzer::verify_module_tree`).

Type checking (`compiler/analyzer/checker.h`, 4010–4019):

- `4010` recursive type.
- `4011` unknown type.
- `4012` duplicate definition.
- `4013` reserved name.
- `4014` arity mismatch.
- `4015` generic argument error.
- `4016` unsupported type.
- `4017` internal error: checked types failed storage verification.

Expression checking (`compiler/analyzer/checker.h`, 4020–4039):

- `4020` type mismatch.
- `4021` unknown value.
- `4022` arity error.
- `4023` invalid operation.
- `4024` non-exhaustive match.
- `4025` refutable `let`.
- `4026` must-use violation.
- `4027` bad `?` operator use.
- `4028` bad `return`.
- `4029` bad assignment.
- `4030` `break` outside a loop.
- `4031` unsupported expression.
- `4032` not compile-time known.
- `4033` invalid compile-time value.
- `4034` unknown intrinsic.

Destructors (`compiler/analyzer/checker.h`, 4040–4049):

- `4040` bad `drop` signature.
- `4041` `drop` on a copyable type.

## lowering (5000–5099)

`compiler/lowering/lowerer.h`:

- `5000` unsupported construct.
- `5001` internal error: lowered IR failed verification.
- `5002` unreachable code reached lowering.
- `5003` destructor glue could not be placed.
- `5004` discarded destructor.

## borrow (6000–6099)

`compiler/borrow/borrow.cc`:

- `6000` use after move.
- `6001` borrow conflict.
- `6002` reference escape.
- `6003` assignment to a borrowed place.

## ir (7000–7999)

`compiler/ir/verifier.h` emits one code per `VerificationErrorKind`, numbered in
declaration order from `7100`: the message is the kind name, and the
diagnostic carries no span. Adding a kind takes the next ordinal; the
range up to 8000 is reserved, so no renumbering is needed. These are
internal-invariant failures, so a user normally sees the phase that
caught the bad IR (for example `5001` or `4017`) rather than the code
here.

## pipeline (8000–8999)

`compiler/pipeline/pipeline_context.h`:

- `8000` no manifest found.
- `8001` I/O error reading, writing, or staging files.
- `8002` not implemented: the request names an unwired path.
- `8003` no targets selected.
- `8004` link error from the system linker.
