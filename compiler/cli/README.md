# cli

Command-line interface: argument parsing, validation, dispatch, and
diagnostic presentation.

Parsing (`parse_args`) owns argv syntax only; semantic validation
(`validate`) owns field combinations; commands own execution. Output
rendering (`parse_output`, `output`) is separate from
both, so parsing stays pure and testable.

Every text a user reads comes from the `i18n` catalog: the option
descriptions and the parser's own labels in `usage`, the result line in
`output`, and the reason in `validate`. `build_parser(language)` takes
the language because the help is composed before the arguments are
parsed, so `cli_main` reads it off the raw arguments with
`scan_language`, the way it reads `--color`.

## Output

Every human-readable error is a `diag::Diagnostic` with no span and no
code, rendered by `diag::render`, so the marker, the severity word, and
the colour are the renderer's and the sentence is the catalogue's. A
rejected flag combination, a parse error, and an unknown verb all take
that path; nothing in this module formats its own `error: `.

`logger` is the one exit. A `Logger` is a destination and one method, and
a block is non-empty text ending in exactly one newline, which the
producer owns and `is_block` states. The destination is a function
pointer and a context rather than a stream, so a test collects blocks
without a descriptor.

Standard error carries the diagnostics and standard output carries the
answer: the result line, the time-trace summary, `--help`, `--version`,
and the whole `--json` document. `docs/adr/0032-errors-on-stderr-results-on-stdout.md`
has the reasoning; `Interruption` is the split for an invocation that
stopped before a command ran.

## Entry points

- `parse_args(argc, argv)` -> `ParseOutcome` (`CliConfig`, help,
  version, or grammar errors).
- `build_parser(language)` -> `arg::Parser` whose help text is written
  in that language, valid for the life of the process.
- `scan_language(argc, argv)` -> the `--lang` value, or the default
  for a value that names no catalog. The parser reports such a value
  as an invalid choice; nothing falls back.
- `validate_cli_config(config)` ->
  `base::Result<void, ConfigError>` (pure): a subcommand is present
  and trailing program arguments only accompany `run`. Grammar-level
  flag errors never reach here.
- `cli_main(argc, argv)` -> exit code. Grammar failures exit
  `ArgParseError`; semantic failures report a diagnostic and exit the
  same way; dispatch only ever sees a validated config.
- `run_build` / `run_compile` / `run_check` / `run_run` / `run_new` /
  `run_init`.
- `trace` owns `--time-trace` sessions: `TraceSession` points the
  context at the global profiler while a command runs and writes the
  Chromium trace JSON on `finish()`.

## Input requirements

- `CliConfig` views borrow argv storage: a config must not outlive
  its argument vector.
- `program_args` outside `run` is rejected (`UnexpectedProgramArgs`)
  instead of silently ignored; an empty `target_dir` selects `"."`
  at the command layer.
- `LANG`, `LC_ALL`, and every other ambient input are ignored. The
  language is `--lang`, defaulted to `en-us`, and a tag that names no
  catalog is an error rather than a fallback.
