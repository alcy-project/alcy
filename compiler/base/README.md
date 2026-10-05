# base

Cross-cutting helpers a pass may use and no pass owns: recursion budgets
(`nesting.h`) and a loop over independent units of work (`for_each.h`).

## Recursion budgets

`MAX_NESTING` bounds how deeply a pass may recurse over one input, and
`NestingGuard`/`NestingScope` spend it. Every tree walk shares the one
budget, because it is a property of the language rather than of any pass.

### Input requirements

- A caller checks `exhausted()` *before* `enter()`. Spending the budget
  is a property of the input, not an internal failure, so this reports
  rather than trips a check, and each consumer words its own diagnostic
  and picks its own code.
- Depth is per guard. Two guards over one walk count independently, so a
  pass that walks two trees in one frame keeps each tree's own budget.
- No allocation: the depth is a counter beside the frame.

## Work spread over threads

`for_each(begin, end, jobs, body)` runs `body(i)` for every index in a
range, on `jobs` threads, handing indices out in slices through one
atomic counter. Fewer than two jobs runs the range on the calling thread.

There is no task pool here, and the unit of work is what decides that. A
task pool earns its cost when the units are small enough that queueing
them costs more than running them, or when they nest; the units here are
whole files, so the counter is enough to keep the threads evenly loaded.

### Input requirements

- The units are independent. Two units that share anything are a race,
  and the counter is the only synchronisation.
- A unit does not stop the run. A caller that wants part of a range done
  raises a flag of its own and the remaining units read it.
- The units are large relative to a slice. The slice count is derived
  from the range and the job count; a unit smaller than a slice is handed
  out whole, which is still correct and only costs balance.

## Removed

This module used to hold the process-wide logger. It was removed in
`8b44cae`, when every command began reporting through one result
envelope.
