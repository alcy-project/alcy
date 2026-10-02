# pipeline

Compilation pipeline: the linear stage flow that wires every module
together (see `compiler/docs/architecture.md`).

Stages run discovery and std staging -> parse (lex, parse, desugar) ->
analyzer (resolve, check) -> lowering -> borrow -> codegen_llvm (emit,
link) -> run. Each stage takes validated artifacts from the previous one
and returns `base::Result<T, diag::Reported>`, reporting through the
shared `DiagBag` in `PipelineContext`. A stage never calls the one after
it, so `analyzer` reads parsed items rather than source bytes and the
parse stage owns the threads and the diagnostic merge order.

## Entry points

- `build_single_file` / `build_package` -> object or executable.
- `check_single_file` / `check_package` -> `CheckOutcome` counts.
- `run_package` -> `RunOutcome{exit_code}`.
- `parse_files` -> the items of every file, once, which every target of a
  package then resolves against; `resolve_inputs` is the one-shot form.
- `std_prelude` -> `std::span<const analyzer::ModuleInput>`;
  `link_executable` -> `base::Result<void, diag::Reported>`.

## Input requirements

- Every entry validates its inputs at the boundary: manifests via
  `pkg::verify_manifest`, the module tree and arena at
  `check_package`, IR once at `StorageBuilder::build`. Stages never
  re-verify what the producing boundary proved.
- Emission creates missing output directories and checks every
  write: an unwritable object path is `PIPELINE_IO_ERROR`, never a
  silent success.
- Exit codes are owned by the cli (`BuildFailed = 3`,
  `CheckFailed = 4`, `RunFailed = 6`); the pipeline only reports
  pass/fail through the bag.
