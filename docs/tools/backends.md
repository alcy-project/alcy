# Backends and output formats

The compiler carries several backends. Which one writes a build is a
property of the target and the profile, not of the source file, and the
answer changes what an artifact looks like. This page collects the rules
and the differences a script or a test has to account for.

## Choosing a backend

| Target and profile | Backend |
| --- | --- |
| Native x86_64 Linux, unoptimized (the default debug build) | `direct-x86_64` |
| Native riscv64 Linux, unoptimized (the default debug build) | `direct-riscv64` |
| Any other native target, and optimized builds | `llvm` |
| A wasm target | `direct-wasm` |

`--backend` overrides this table; the build fails with a diagnostic if
the named backend is not part of this compiler or cannot write the
requested artifact. `--emit=ir` and `--emit=ir-bc` need no backend at all
and ignore `--backend`.

## What each backend writes

| Output kind | `llvm` | `direct-x86_64` | `direct-riscv64` | `direct-wasm` |
| --- | --- | --- | --- | --- |
| `executable` | object, then the system linker | object, then the system linker | object, then the system linker (freestanding) | final WASI module |
| `object` | relocatable object | relocatable ELF64 object | relocatable ELF64 object | - |
| `asm` | textual assembly | textual assembly, AT&T or Intel | textual RISC-V assembly | rejected, see below |
| `llvm-ir`, `llvm-bc` | textual IR / bitcode | - | - | - |
| `ir`, `ir-bc` | alcy IR, written by the pipeline | alcy IR, written by the pipeline | alcy IR, written by the pipeline | alcy IR, written by the pipeline |

`--emit=asm` writes one assembly source per target, with a `.s`
suffix. It replaces an unnamed output's suffix like the other modes; it
stops before linking. `-j` does not split an assembly output, the same
as `--emit=llvm-ir`.

The direct x86_64 backend writes AT&T syntax by default; `--asm-syntax=intel`
selects Intel syntax. RISC-V has one assembly spelling, and LLVM prints its
own assembly; both ignore the flag.

## Consequences of the choice

- **Debug and release can produce different text.** A native unoptimized
  build selects `direct-x86_64` or `direct-riscv64`, while an optimized build
  selects LLVM.
  These write different assembly and different object bytes for the
  same source, just as different optimization levels do. Pin `--backend`
  when the text or bytes have to stay comparable across builds; a test
  that compares generated assembly must name its backend. The riscv64
  printer emits fixed-width instructions with `.option norvc`, so its
  `.s` assembles to the same instructions as its object.
- **Assembly is equivalent, not identical.** The direct x86_64 backend's `.s`
  assembles to the same program as its object, but not to the same bytes.
  LLVM's own printer includes directives such as `.file` and
  `.prefalign`; those need LLVM's integrated assembler, which `clang -c`
  provides. The direct backend's output targets GNU `as`.
- **The riscv64 backend links freestanding executables through the system
  linker.** Its program cases are linked without a C runtime and run under
  qemu-user with `--mode=qemu` in the case harness.
- **wasm has no assembly form.** `--emit=asm` with a wasm target is
  refused with a diagnostic. Use `--emit=executable` for a WASI module.

The implementation of each backend is described in
[the architecture document](../../src/docs/architecture.md).
