# cli

Command-line interface: argument parsing, validation, dispatch, and
diagnostic presentation.

Parsing (`parse_args`) owns argv syntax only; semantic validation
(`validate`) owns field combinations; commands own execution. Output
rendering (`parse_output`, `diagnostic_output`) is separate from
both, so parsing stays pure and testable.

## Entry points

- `parse_args(argc, argv)` -> `ParseOutcome` (`CliConfig`, help,
  version, or grammar errors).
- `validate_cli_config(config)` ->
  `base::Result<void, ConfigError>` (pure): a subcommand is present
  and trailing program arguments only accompany `run`. Grammar-level
  flag errors never reach here.
- `cli_main(argc, argv)` -> exit code. Grammar failures exit
  `ArgParseError`; semantic failures print `error: <detail>` and
  exit the same way; dispatch only ever sees a validated config.
- `run_build` / `run_check` / `run_run` / `run_new` / `run_init`.

## Input requirements

- `CliConfig` views borrow argv storage: a config must not outlive
  its argument vector.
- `program_args` outside `run` is rejected (`UnexpectedProgramArgs`)
  instead of silently ignored; an empty `target_dir` selects `"."`
  at the command layer.
