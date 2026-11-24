# `alcy test`

Build a package as it stands, with its tests compiled as items in
the tree, and run the tests in it. `alcy test` is what round-trips
compiler changes against the package: it exercises the package's own
`#[test] fn`s with the same code the program itself uses.

## Usage

```console
$ alcy test [--release | --profile <NAME>] [PACKAGE-DIR]
           [--filter TEXT] [--list] [--no-run]
           [--execution multi-process|single-thread]
           [--linker DRIVER] [--link-args ARG]... [--no-cache]
           [--freestanding]
           [--on-panic abort|backtrace] [--optimize on|off]
           [--overflow-checks on|off]
```

`PACKAGE-DIR` defaults to the current directory. A positional ending
in `.al` is rejected. A suite applies to every listed member: tests run
member by member, their counts are added into one summary, and each test
label starts with the member's declared path (`core/cmp::less_is_lt`).

## How tests enter the program

The source marks a function as a test with `#[test]` and gives the whole
set an existence condition with `#[when(testing)]` in any module of the
package:

```text
#[when(testing)] {
  #[test] fn adds() {
    assert_eq(util::twice(2), 4)
  }

  fn assert_eq(left: i64, right: i64) {
    if left != right {
      panic("not equal")
    }
  }
}
```

`#[test]` says what the function is; `#[when(testing)]` says when it
exists. Other commands leave the group out entirely, so the tests never
enter a build or a check. The gate is general: a `#[when(EXPR)] { ... }`
group can hold any items and can use any fact of the build descriptor.

- Only the package's own `#[test] fn`s are picked up. A dependency's
  are not, and never are.
- The test functions and the helpers beside them are ordinary functions
  of the module that wrote them. Nothing is imported for you.
- `alcy test` links those functions into one program
  instead of the package's own `main`, then runs that program.
- A library member without its own `main` is fine; the generated program
  is the entry, not your package.

Tests are created and run in the order they appear across all modules,
deterministically; the run never consults the clock or the file system
except through the package it operates on.

## Selection

| Flag | Meaning |
| --- | --- |
| `--list` | Print the tests the build would run, one full name per line, and run none |
| `--filter TEXT` | Keep only tests whose reported name contains this text. A superstring keeps every match; e.g. `vector` keeps `my_vector` too |
| `--no-run` | Build the test program and stop before running it |
| `--execution multi-process` (default) | One process per test: a test that aborts kills itself and the rest report |
| `--execution single-thread` | Every test in one process; a failing test ends the run and the report says the run stopped early |

Names are reported as `<module>::<test>`; a module-qualified filter
spelling like `util::adds` matches, as does plain `adds`.

## Observed output

```console
$ alcy test
Tested    2 tests  (720 ms, 384.8 MiB peak, cache miss)
```

```console
$ alcy test --list
main::adds
main::doubles
Listed    2 tests  (37.1 ms, 99.7 MiB peak)
```

```console
$ alcy test --json
{"version":1,"command":"test","status":"ok","outcome":"tested","cache":"hit","summary":"Tested    2 tests  (589 ms, 357.5 MiB peak, cache hit)","output_path":null,"subject":"2 tests","stats":{"files":0,"modules":0,"functions":0,"findings":0,"fixes":0,"tests":2,"test_failures":0,"test_ended_early":false,"output_bytes":0,"wall_ns":588942561,"peak_memory_bytes":374874112},"diagnostics":[]}
```

`--json` keeps one document on standard output and captures a test's
own output in the same document (`stats.output_bytes`); the test's
output never writes through. In multi-process mode a crashing test is
reported by signal, and one stopping the process in single-thread mode
sets `test_ended_early: true`.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | every selected test passed |
| 1 | a test failed, or the command line was rejected |
| 4 | the package was not a program `alcy test` can accept (a target that cannot start a test process) |

A test is a process the run starts and watches. The wasm backend cannot
start one, and neither can a freestanding target on a platform whose
entry the runner cannot execute; the runner refuses either with
`Cannot run tests for target` and exit code 4 before building. A
freestanding target on Linux can start one: its entry reads `argc` and
`argv` off the initial stack. `alcy test` always selects the kernel-only
link for Linux ELF targets, so that path keeps its coverage even though a
hosted build links the system C runtime by default.

## See also

- [`alcy build`](build.md) - build the program without its tests
- [`alcy check`](check.md) - analyze the package without emitting
- `alcy test --help` - the synopsis for this build
