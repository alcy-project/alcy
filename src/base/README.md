# base

Logging (`logger.h`).

Process-wide diagnostic sink used by the cli and pipeline stages
for prefix-free output (`wo_prefix`) and debug tracing (`DLOG`).
Color/style resolution happens once in the cli; modules just log.

## Input requirements

- `init_logger` runs before any logging call; logging before init
  is a caller bug.
- User-facing diagnostics go through `diag::DiagBag` rendering,
  not the logger directly: the logger carries prose and debug
  traces, never structured errors.
