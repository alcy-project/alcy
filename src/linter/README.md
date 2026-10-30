# linter

A static analyzer for alcy, invoked as `alcy lint`. It reads a package
through the compiler frontend (parse, resolve, type-check) and reports
findings that do not stop the compiler: an unused binding, a loop over
`true`, a comparison of a value with itself. The compiler it runs beside
is the authority; the linter adds opinion, never semantics.

The module is a library, not a compiler stage. It depends on the
compiler; the compiler never depends on it. The cli calls
`linter::lint_package` / `linter::lint_single_file`, and the tests call
`linter::lint_source` to lint text without a file.

## Configuration

`.alcy/lint.toml` beside the goal package manifest holds the settings;
an absent file means every rule at its default. Only the goal package
is configured, and only its own modules are linted: staged standard
library sources and path dependencies are skipped.

```toml
# Run lowering and the borrow check after the rules (default true).
# borrow-checks = true

# Report a warning as an error, the CI gate (default false).
# deny-warnings = false

# The level a rule not named below uses. One of allow, warn, deny.
level = "warn"

[rules]
# A string is the level; a table adds that rule's options.
unused-variable = { level = "deny", ignore = ["i", "j"] }
missing-docs = "allow"
too-many-arguments = { level = "warn", max-args = 5 }
```

An unknown rule name or level is an error: both are typos. Unknown keys
are ignored, like the other `.alcy/` files (ADR-0019). `ignore` lists
names the rule must not report; the binding rules and `unused-import`
read it. A `deny` finding is an error, so `alcy lint` exits non-zero;
a `warn` finding does not change the exit code.

The command line can override a level too, the counterpart to clippy's
`-A` / `-W` / `-D`: `--allow`, `--warn`, and `--deny` each take a rule
id or `all` and may be repeated. A flag edits the resolved table after
the file is read, so it wins over lint.toml, and when one rule is named
by more than one flag the loudest level wins. An unknown id is the same
error lint.toml reports, so a run that misspells a rule still fails.

`alcy lint --list-rules` prints the registered rules, their default
levels, and what each reports, the counterpart to clang-tidy's
`--list-checks`; it reads no package and stops before any file is
loaded. With `--json` the same table is one JSON document with a
`rules` array, so a tool reads ids and defaults rather than the
aligned text.

`--deny-warnings` reports every finding as an error, the gate a CI run
wants: it promotes what fires and does not turn on a rule the config
silenced, so `allow` still means allow. `deny-warnings = true` in the
file is the same thing, and the two add. `--no-borrow-checks` is the
command-line spelling of `borrow-checks = false`, the fast edit loop;
measured on a 61-module package it takes the run from about 29 ms to
about 12 ms, most of it the lowering and borrow tail the rules never
read.

## Suppressing a finding

A rule the package config enables can be silenced at the line that
would report it. A `// alcy-lint-ignore` comment on that line makes the
finding there go away, whatever the rule's level; a list of ids after a
colon narrows it to those rules, and `all` in the list means every
rule. The directive is read at the start of the comment, so a note that
merely mentions it does nothing.

```alcy
fn example() -> i32 {
  unused := 1 // alcy-lint-ignore
  pinned := 2 // alcy-lint-ignore: unused-variable -- held for the debugger
  ret 0
}
```

The line above can silence the next one instead, the spelling for an
item whose finding points at a declaration rather than at an expression:

```alcy
// alcy-lint-ignore-next-line: missing-docs
pub fn api() -> i32 {
  ret 1
}
```

The directive is read from the source bytes beside the finding, not from
a parsed attribute, because the language has no attribute syntax yet.
Every rule reports through the context, so a rule needs no suppression
code of its own and a suppressed finding offers no repair.

## Fixing

`alcy lint --fix` applies the repairs the rules are sure of and then
reports what remains. A pass lints, writes each file's non-overlapping
repairs, and lints again; a repair that overlaps one already applied is
left for a later pass, and the run stops after four - a repair a fifth
pass would have offered, or one the last pass skipped as overlapping,
is not applied and its finding stays in the report. The final pass runs
on the rewritten files, so the findings it prints describe the bytes on
disk, and a denied finding a repair removed does not fail the run. A
file that cannot be written is reported, ends the pass loop, and keeps
its original bytes, so the final pass reports against those.

## Performance

A run is the frontend, the rules, and, unless borrow-checks is false,
the lowering and borrow tail. The rules on a module share one walk, one
binding table, and the name-usage table the walk records; the binding
table is built only when an enabled rule reads it, and so is the usage
table. The tail is most of the cost and the rules do not need it, so
borrow-checks = false stops after analysis for a faster edit loop. The
run then does not report a borrow error, and alcy check remains the
complete gate.

## Rules

| Id | Default | Reports |
| --- | --- | --- |
| `unused-variable` | warn | a binding no expression reads |
| `unused-mut` | warn | a `mut` binding that is never written |
| `shadow` | allow | a binding that shadows an enclosing one |
| `unreachable-code` | allow | a statement after control leaves the block |
| `while-true` | warn | `while true`, which a `loop` states better |
| `empty-loop` | warn | a `loop` with an empty body |
| `double-negation` | warn | an operator applied to itself |
| `self-comparison` | warn | a comparison whose operands are one expression |
| `nested-if` | warn | an `if` whose only statement is another `if` |
| `needless-bool` | warn | an `if` whose branches return true and false |
| `useless-cast` | warn | a cast to the type the value already has |
| `float-equality` | allow | an equality test on floating-point values |
| `needless-borrow` | warn | a borrow of a dereference that is the reference |
| `lossy-cast` | allow | a cast that narrows the value's scalar type |
| `bool-comparison` | warn | a bool compared with a true or false literal |
| `dead-code` | allow | a function no call in the package reaches |
| `unused-field` | allow | a struct field no expression reads |
| `missing-docs` | allow | a `pub` item without a doc comment |
| `too-many-arguments` | warn | a function above `max-args` parameters |
| `unused-import` | warn | an import no path in the module names |

Two rules are off by default, for different reasons.
`unreachable-code` duplicates the lowering pass, which already warns
`WD003` for the same statement; a run that lints without lowering opts
in, and a run that lowers sees the finding twice if it does. `dead-code`
has no compiler counterpart, and a package in progress always has
functions waiting for their first caller, so it waits to be asked.
`float-equality` is opt-in because comparing floats exactly is
sometimes the point; the rule reports the comparison that is usually a
mistake, not every one. A rule that suppresses a
false positive is preferred to one that guesses: `unused-mut` counts a
method receiver as a write, because the syntax does not say whether the
receiver is taken newly exclusively.

`unused-field` reads the field names the walk records. A field is read
when a projection or a destructuring pattern names it; the
constructor's initializer is a write and does not count, which is the
question the rule answers. A `pub` field is the package's surface and
is never reported; the struct's own `pub` does not make every field
one.

`dead-code` reads the checker's call graph, not the source: a finding
is a private function whose (module, signature) pair no recorded call
target names. The entry point, `pub` declarations, and spec
implementations are the surface the rule keeps out of the question; the
goal package alone is read, so the toolchain's own functions are never
findings. Liveness is per signature, not per path: the checker records
a call target without the function it sits in, so a helper reached only
from an uncalled function stays live. The rule under-reports rather than
claim a chain it cannot prove. It is opt-in because a package in
progress always has functions waiting for their first caller.

`unused-import` reads the path heads the walk recorded. An import is
used when the module names its local name as the head of a value path, a
type path, a pattern path, or a struct literal; a `pub use` is a
re-export and an underscore alias imports for resolution's sake, so
neither is reported. The rule removes the import and its line with a
repair.

The type-based rules read the analyzer's recorded types, not the
source spelling. `types.h` answers what type an expression was checked
at and whether that type is a float or a shared reference; a cast whose
two types are the same, an equality over a float, `&*r` over a shared
reference, a cast from a wider integer or float, and a bool compared
with a true or false literal are the findings. `double-negation`,
`useless-cast`, `needless-borrow`, and `bool-comparison` also offer a
machine-applicable repair (`ctx.fix`) when the expression they would
put in place binds tightly enough that the regrouping cannot change
the meaning.

`needless-bool` reads the shape ADR-0059 gives an `if`: every branch
must end in `ret`, so `if c { ret true } else { ret false }` is the
condition itself and the swapped pair is its negation. Both branches
returning one literal is a constant and an `if PAT := EXPR` tests a
pattern rather than a value, so neither is reported. The repair writes
`ret c` or `ret !(c)`, parenthesizing the condition because it is any
expression.

`nested-if` collapses the pair it reports: `if a { if b { body } }` is
`if a && b { body }`. Only `||` binds looser than the join, so a
condition that is one is parenthesized and every other reads bare; a
pattern test cannot be joined, so the finding stays without a repair.

## Adding a rule

1. Define the rule as a type in `rules/rules.h` - the id lint.toml
   addresses, the default level, the code, and the summary - and one
   check shape: a whole-module `run`, or a `check_expr` / `check_item`
   / `check_block` hook the engine's shared walk dispatches to.
   Implement it in the `rules/` file that already covers its subject -
   the binding table, checked types, control flow, expressions, items,
   imports, fields, or the call graph - adding a file only when the
   subject is new; the split is by what a rule reads, as the Layout
   section lists. A module rule that needs the binding, name-usage, or
   field-usage table declares `READS_BINDINGS`, `READS_USAGE`, or
   `READS_FIELD_USAGE`; the engine builds each once per module, and
   only when an enabled rule asked. A rule reads
   `LintContext` - the checked package, the AST, the sources, and the
   resolved setting - and reports through
   `ctx.report<Key>(span, args...)`, which composes the catalog message
   and applies the configured level. `ctx.fix(finding, span, bytes)`
   attaches the repair a client may apply without asking, and
   `ctx.label` attaches a note. The `RuleType` concept holds the type
   to the shape before anything compiles.
2. Add the message to `src/i18n/messages.def` under `/* linter */`.
3. Add a `LintCode` in `diag_code.h` and the type to the list in
   `rule.cc`. The list's order is the configuration table order, so
   append rather than insert; the registry folds the list into the
   table the runtime reads.
4. Add a case to `src/tests/linter/linter_test.cc` that reaches
   the finding and asserts the name and the span.

## Layout

- `lint.h` / `lint.cc`: the entry points and the file/config loading.
- `engine.h` / `engine.cc`: builds the active rule lists, the
  per-module binding table, and the usage table; dispatches the one
  walk; and counts what the rules emitted.
- `rule.h` / `rule.cc`: the rule table.
- `config.h` / `config.cc`: `.alcy/lint.toml`.
- `context.h`: what one rule run reads and how it reports.
- `walk.h` / `walk.cc`: one traversal with callbacks, for rules that
  only inspect nodes.
- `bindings.h` / `bindings.cc`: the scope-resolving pass that builds
  one binding table per module; `unused-variable`, `unused-mut`, and
  `shadow` read it.
- `types.h` / `types.cc`: the checked-type query the type-based rules
  read, including the conservative answer for a generic body.
- `usage.h` / `usage.cc`: the path heads the walk records for
  `unused-import`.
- `fix.h` / `fix.cc`: collecting a pass's repairs and applying one
  file's edits, which `--fix` drives.
- `rules/`: the rule implementations and their shared declarations.
  `rules.h` holds every rule type; the registry folds them into one
  table. The implementation files group rules by what they read rather
  than one file per rule: `bindings_rules.cc` (the binding table),
  `control_flow_rules.cc` (blocks and loops), `expression_rules.cc`
  (one expression at a time), `type_rules.cc` (checked types),
  `item_rules.cc` (declarations), `import_rules.cc`, `field_rules.cc`,
  and `dead_code_rules.cc` (the call graph).
