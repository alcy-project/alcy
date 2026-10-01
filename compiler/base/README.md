# base

Recursion budgets (`nesting.h`).

`MAX_NESTING` bounds how deeply a pass may recurse over one input, and
`NestingGuard`/`NestingScope` spend it. Every tree walk shares the one
budget, because it is a property of the language rather than of any pass.

## Input requirements

- A caller checks `exhausted()` *before* `enter()`. Spending the budget
  is a property of the input, not an internal failure, so this reports
  rather than trips a check, and each consumer words its own diagnostic
  and picks its own code.
- Depth is per guard. Two guards over one walk count independently, so a
  pass that walks two trees in one frame keeps each tree's own budget.
- No allocation: the depth is a counter beside the frame.

This module used to hold the process-wide logger. It was removed in
`8b44cae`, when every command began reporting through one result
envelope, and this file outlived it.