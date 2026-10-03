# ADR-0044: Function types and closures

- Subject: the language
- Status: Accepted
- Date: 2026-10-03

## Context

Closures do not exist; `||` is logical-or only
(`docs/spec/grammar.md`). Functions are items: `fn` declares
parameters with types and an optional return type, generics bind
from argument types with a turbofish fallback, and calls resolve
through paths only — `check_call` rejects a non-`Path` callee
with `AnalyzerCallNonPath`, and lowering resolves every call
through `call_targets` to a statically known function. The
iteration story needs callbacks that see local context
(`filter` with a threshold, `sort_by` with a comparator), which
bare function items cannot express.

Two questions shaped the design: what a closure looks like, and
what it captures. Rust spells `|params| body` with implicit
captures inferred from use, moving toward finer inference over
time; C++ spells `[captures](params)` with explicit modes.
The language's explicitness favors declared context, but the
spelling must not be C++ by default: it should read as alcy.

## Decision

**A closure is an anonymous function: `(params) -> body`.**

```ebnf
closure_expr  = [ captures ] , "(" , [ closure_param ,
                  { "," , closure_param } , [ "," ] ] , ")" ,
                  "->" , expr ;
closure_param = [ "mut" ] , ( IDENT | "_" ) , [ ":" , type ] ;
captures      = "[" , [ IDENT , { "," , IDENT } , [ "," ] ] , "]" ;
```

`(a: i32, b: i32) -> { a + b }` and `(a) -> a + b` are both
closures; types and braces are optional, there is no `ret`, and
the value is the body's value exactly as a block's is. A bare
parameter list means no captures; `[]` says the same thing
explicitly and both spellings are accepted. `mut` and `_`
mirror declaration patterns; destructuring waits. A type on `_`
is accepted and ignored. Duplicate params follow `fn` items.

Disambiguation needs no new machinery. The parser scans tokens
without consuming, in the style of `scan_lead`: a `Colon` at
bracket depth zero inside the parens, or an `Arrow` right after
the matching `)`, means a closure. Both signals are syntax errors
today — no valid program changes meaning, and no lexer change is
needed (`Arrow` already exists). `)` and `->` share a line under
the existing newline rule, which already constrains `fn` return
types the same way; the body may start on the next line. A `{`
body reuses `block`, while `Path {` after `->` is a struct
literal under the existing rule. The body is one `expr`, so
statements and assignment need braces; meeting `=` there is a
guidance diagnostic, not a silent truncation. No new keywords,
and `||` stays logical-or.

**Captures are explicit.**

`[x, y]` names locals: `let` bindings, enclosing parameters, and
`self`. Module-scope names need no listing — they need no
environment. Modes are inferred from use (read borrows, write
borrows mutably, move moves); the list carries no mode syntax.
A name used but unlisted is an error naming the missing capture;
a name listed but unused is an error, since even an unread
borrow constrains checking. Each nesting level lists from its
immediate outer scope. A listed capture shadowed by a parameter
is unused, and reported as such.

Every outer name in a body then traces to the list line, and
desugar-time lambda lifting becomes mechanical: the list is the
environment, tuples are structural so no nominal type is needed,
and shadowing is the only order constraint. The alternatives
stay out: implicit capture (Rust's ergonomics against this
language's reader-clarity, recorded as a deliberate split),
`|x|` and `||` (a third meaning for `|`, and pipes cannot name
captures anyway), suffix clauses (late discovery plus a new
keyword), use-site sigils (viral, a new aesthetic), `use` for
locals (a second namespace on one keyword, and bare bodies could
never capture), and no captures at all (too weak for the
iteration story).

**`ret` returns from the closure.** A closure is the innermost
function, as in Rust; the spec's "enclosing function" covers it.
This is also what keeps lifting sound — a non-local return
across a lifted boundary has no meaning to preserve.

**Function types live in type position: `(A, B) -> R`.**

Structural: two closures over one signature share the type, and
captures are erased. Nesting is right-associative. This is how a
callback parameter is named (`fn each(f: (T) -> (), ...)`). The
parser hooks into the `LParen` case of `parse_type`: unit is no
parameters, one type is one, a tuple is many, and `&`, arrays,
and turbofish compose by existing recursion.

**Inference fills the blanks.** Unannotated parameters infer
from use through unification; failure asks for an annotation,
the way generic types do. The return type comes from the body.
Closures are monomorphic: no generic closures in MVP.

**Calls go through values.** `check_call` accepts a non-`Path`
callee of function type and unifies parameters and return;
lowering gains an indirect-call path beside `call_targets`;
borrow treats an indirect call with a conservative summary.

**MVP represents every function value as code plus environment.**
A uniform fat pointer, null environment for plain functions, the
environment boxed at creation. No anonymous types enter the
type system. Borrowed captures compose by region intersection
like `&`-field structs and copy like views, whose copies keep
the loan. Monomorphized and inline environments are follow-ups.

## Consequences

What this buys, in slices: S1 parses closures and function types
(AST, grammar files, parser tests). S2 runs non-capturing
closures end to end: function types resolve to a signature in
the type table, closure literals check and lower to synthetic
functions, named functions coerce to values, and calls through
values lower to indirect calls with a result whose loans track
its arguments. S3 adds captures: the environment value, capture
checking, mode inference, and loans through captured state.

What it costs is one allocation per closure creation, indirect
call overhead everywhere a value is called, and an explicit
list on every closure that sees outer scope. Deferred on
purpose: generic closures (and function types as generic
arguments, which cannot mangle apart yet), destructuring
parameters, `comp` closures, `Fn`-style specs, monomorphized
representations, explicit capture modes, and recursion, which
needs a self-name the syntax does not give.

## Staged landing

**Landed:** S1 and S2. Closures parse with optional capture
lists; function types resolve to a structural signature; a
non-capturing closure checks against an annotated or expected
signature, lowers to a synthetic function whose environment
slot is null, and runs through `alcy run`. A named function
coerces to the same value shape through a wrapper that drops
the environment. Calls through values lower indirectly, and
borrow treats their results conservatively: a result that can
carry loans carries its arguments' and callee's loans, so a
returned reference keeps what it borrows from live.

**Follow-up:** S3, captures. Until then a non-empty capture
list and a bare use of an outer local both report
not-implemented, and a function type bound as a generic
argument is rejected because its symbol cannot be mangled
apart yet.
