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

A message is a sentence, so it starts with a capital, and the wording
below is in the same voice: these are descriptions of what a code
means, close enough to the message to be recognisable in the output and
deliberately not a second copy of it.

## pkg (1000–1999)

Manifests (`compiler/pkg/manifest.cc`, 1000–1099):

- `1000` Syntax error: the manifest does not parse.
- `1001` Semantic error: parsed but invalid (see `ManifestError`).

Modules (`compiler/pkg/modules.cc`, 1100–1199):

- `1100` Semantic error: a module entry selects nothing or conflicts.
- `1101` Unselected file: a source file belongs to no module.

Resolution (`compiler/pkg/resolve.cc`, 1200–1299):

- `1200` I/O error: the package root or manifest cannot be read.
- `1201` Cycle error: package dependencies form a cycle.

Toolchain (`compiler/pkg/toolchain.cc`, 1300–1399):

- `1300` Syntax error: the toolchain file does not parse. The file is
  read without a source location, so the message names the file rather
  than pointing into it.
- `1301` Semantic error: parsed but not a usable toolchain. Covers a
  `linker` that is not a string and a `link-args` that is not a list of
  strings.

## lexer (2000–2099)

`compiler/lexer/lexer.cc`:

- `2000` Invalid character.
- `2001` Unterminated string.
- `2002` Unterminated character literal.
- `2003` Invalid number.
- `2004` Unterminated block comment.
- `2005` Invalid escape.

## parser (3000–3999)

Grammar (`compiler/parser/parser.h`, 3000–3099):

- `3000` Unexpected token.
- `3001` Reserved word used as an identifier.
- `3002` Internal error: the token stream failed structural
  verification (see `lexer::verify_token_stream`).
- `3003` Internal error: the parsed arena failed structural
  verification (see `ast::verify_file`).
- `3004` Nesting deeper than the language's budget.
- `3005` A range end that is not spelled `..<` or `..=`.

Desugaring (`compiler/parser/desugar.cc`, 3100–3199):

- `3100` Or-pattern alternatives bind different name sets.
- `3101` A name is already bound in the innermost scope.

## analyzer (4000–4999)

Module resolution (`compiler/analyzer/resolve.cc`, `compiler/analyzer/resolve.h`,
4000–4009):

- `4000` Duplicate module.
- `4001` Unresolved import.
- `4002` Ambiguous import.
- `4003` Unreachable file (warning).
- `4004` Invalid path: an unknown file id reached resolution.
- `4005` Internal error: the module tree failed structural
  verification (see `analyzer::verify_module_tree`).

Type checking (`compiler/analyzer/checker.h`, 4010–4019):

- `4010` Recursive type.
- `4011` Unknown type.
- `4012` Duplicate definition.
- `4013` Reserved name.
- `4014` Arity mismatch.
- `4015` Generic argument error.
- `4016` Unsupported type.
- `4017` Internal error: checked types failed storage verification.

Expression checking (`compiler/analyzer/checker.h`, 4020–4039):

- `4020` Type mismatch.
- `4021` Unknown value.
- `4022` Arity error.
- `4023` Invalid operation.
- `4024` Non-exhaustive match.
- `4025` Refutable `let`.
- `4026` Must-use violation.
- `4027` Bad `?` operator use.
- `4028` Bad `return`.
- `4029` Bad assignment.
- `4030` Break outside a loop.
- `4031` Reserved: defined but no check reports it yet. An unsupported
  primitive currently uses `4016`, which names the type rather than the
  expression.
- `4032` Not compile-time known.
- `4033` Invalid compile-time value.
- `4034` Unknown intrinsic.

Destructors (`compiler/analyzer/checker.h`, 4040–4049):

- `4040` Bad `drop` signature.
- `4041` Drop on a copyable type.

Recursion budget (`compiler/analyzer/checker.h`, 4050–4059):

- `4050` Nesting deeper than the language's budget. One walk per
  `base::NestingGuard`, so the code names the pass rather than the tree.

## lowering (5000–5099)

`compiler/lowering/lowerer.h`:

- `5000` Unsupported construct.
- `5001` Internal error: lowered IR failed verification.
- `5002` Unreachable code reached lowering.
- `5003` Destructor glue could not be placed.
- `5004` Discarded destructor.
- `5005` Nesting deeper than the language's budget.

## borrow (6000–6099)

`compiler/borrow/borrow.cc`:

- `6000` Use after move.
- `6001` Borrow conflict.
- `6002` Reference escape.
- `6003` Assignment to a borrowed place.

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

- `8000` No manifest found.
- `8001` I/O error reading, writing, or staging files.
- `8002` Not implemented: the request names an unwired path.
- `8003` No targets selected.
- `8004` Link error from the system linker.
