# pipeline

Compilation pipeline: the linear stage flow that wires every module
together (see `ARCHITECTURE.md`).

Stages run frontend (discovery, std/runtime staging) -> analyzer
(resolve, check) -> lower -> borrow -> codegen_llvm (emit, link) ->
run. Each stage takes validated artifacts from the previous one and
returns `base::Result<T, diag::Reported>`, reporting through the
shared `DiagBag` in `PipelineContext`.

## Entry points

- `build_single_file` / `build_package` -> object or executable.
- `check_single_file` / `check_package_tree` -> `CheckResult` counts.
- `run_single_file` / `run_package` -> `RunOutcome{exit_code}`.
- `std_prelude` -> `base::Result<std::span<...>, diag::Reported>`;
  `stage_runtime` -> `base::Result<void, diag::Reported>`.

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
