# `alcy lint`

Static analyzer for alcy source. `lint` reads a package the same way
the compiler does - parse, resolve, type-check - and adds opinion on
top: style choices the language has no rule for, design conventions,
maintainability reads. The compiler decides whether code is correct;
`lint` decides whether code is good enough to keep without comment.

## Usage

```console
$ alcy lint [--file FILE] [--fix] [--list-rules] [--deny-warnings]
           [--allow RULE]... [--warn RULE]... [--deny RULE]...
           [--no-borrow-checks] [PACKAGE-DIR]
```

`PACKAGE-DIR` defaults to the current directory. `.al`-suffixed positions
are rejected - use `--file` for a single source.

## Configuration

`.alcy/lint.toml` beside the goal package's manifest holds the settings.
An absent file means every rule at its default; a present one configures
the goal package alone (the staged standard library and path dependencies
are skipped).

```toml
# Run lowering and the borrow check after the rules (default true).
borrow-checks = true

# Report a warning as an error, the CI check (default false).
deny-warnings = false

# The level a rule not named below uses. One of allow, warn, deny.
level = "warn"

[rules]
# A string is the level; a table adds that rule's options.
unused-variable = { level = "deny", ignore = ["i", "j"] }
missing-docs = "allow"
too-many-arguments = { level = "warn", max-args = 5 }
```

An unknown rule name or level is an error. Unknown keys are ignored,
like the other `.alcy/` files. `ignore` lists names a rule must not
report; the binding rules and `unused-import` read it. A `deny`
finding makes `lint` exit non-zero; `warn` does not change the exit
code.

## Command-line overrides

`--allow`, `--warn`, and `--deny` each take a rule id or `all`, and
are repeatable. They edit the resolved table after the file is read,
so a flag wins over lint.toml. When two flags name the same rule, the
loudest level wins. `--deny-warnings` promotes every finding to an
error (a silenced rule stays silenced). `--no-borrow-checks` skips the
lowering and borrow tail after analysis, the fast edit loop; `alcy
check` is the one that runs them all.

`--list-rules` prints the registered rules, their default levels, and
what each reports, and stops without reading a package or running any
file. With `--json`, the same table is one JSON document with a
`rules` array.

## Suppressing a finding

Any rule can be silenced at the line that would report it:

```text
fn example() -> i32 {
  unused := 1 // alcy-lint-ignore
  pinned := 2 // alcy-lint-ignore: unused-variable -- held for the debugger
  ret 0
}
```

`alcy-lint-ignore` on the line silences it whatever the rule's level,
or a list of `rule-id`s narrows it. `alcy-lint-ignore-next-line: RULE`
applies from the next line. The directive is read from the source
bytes; suppressing a finding offers no repair.

## Repairs

`alcy lint --fix` applies the repairs rules are sure of, re-lints,
and reports what remains. A repair that overlaps one already applied
is left; the run stops after four passes. A denied finding a repair
removed does not fail the run. A file that cannot be written is
reported and stops the loop; the final pass is on the rewritten
bytes.

## Observed output

```console
$ alcy lint
Linted    1 file, 1 finding  (3.29 ms, 71.1 MiB peak)
```

The finding itself is on standard error:

```text
warning[EL010]: unused variable: unused
 --> main.al:2:3
  |
2 |   unused := 42
  |   ^^^^^^ if this is intentional, prefix the name with an underscore
```

Findings under `--json` are entries in the envelope's `diagnostics`
array. Each carries `check_name` with the rule's id.

```console
$ alcy lint --json
{"version":1,"command":"lint","status":"ok","outcome":"linted","cache":"off","summary":"Linted    1 file, 1 finding  (3.75 ms, 72.8 MiB peak)","output_path":null,"stats":{"files":1,"modules":1,"functions":0,"findings":1,"fixes":0,"tests":0,"test_failures":0,"test_ended_early":false,"output_bytes":0,"wall_ns":3745283,"peak_memory_bytes":76365824},"diagnostics":[{"severity":"warning","code":{"stage":"lint","local_id":10},"check_name":"unused-variable","message":"unused variable: unused","span":{"file":"main.al","offset":21,"length":6},"labels":[{"span":{"file":"main.al","offset":21,"length":6},"message":"if this is intentional, prefix the name with an underscore"}]}]}
```

Exit code is `0` for warnings, `9` for a denied finding, lint read
failure, or a `--deny-warnings` promotion.

## Rules

| Id | Default | Reports |
| --- | --- | --- |
| `unused-variable` | warn | a binding no expression reads |
| `unused-mut` | warn | a `mut` binding that is never written |
| `shadow` | allow | a binding that shadows an enclosing one |
| `unreachable-code` | allow | a statement after control leaves the block |
| `unreachable-pattern` | warn | a match arm an earlier arm already covers |
| `while-true` | warn | `while true`, which `loop` states better |
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
| `unused-variant` | allow | an enum variant nothing constructs |
| `missing-docs` | allow | a `pub` item without a doc comment |
| `too-many-arguments` | warn | a function above `max-args` parameters |
| `unused-import` | warn | an import no path in the module names |

See [the Linter README](../../src/linter/README.md) for the in-depth
explanations of what each rule reads; `.alcy/lint.toml` entries are
the same ids shown by `--list-rules`.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | no finding, or warnings only |
| 9 | a denied finding fired, or the input could not be read |

## See also

- [`alcy fmt`](fmt.md) - layout owned by the compiler
- `alcy lint --help` - the synopsis for this build