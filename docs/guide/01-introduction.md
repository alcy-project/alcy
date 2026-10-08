# Introduction

alcy is an experimental, statically-typed programming language. The
project is pre-MVP: the language, its standard library, and most of the
compiler pipeline are still being designed, so nothing in this guide
promises stability. What it describes is what runs today.

The compiler is written in C++20 and has two back ends: LLVM for native
targets, and a direct WebAssembly back end that needs no LLVM and is what
the [playground](/playground/) on this site runs. That makes the
playground the shortest path to seeing the language work: it compiles and
executes programs in the browser, and every `alcy` example in this guide
was compiled with the same module when the site was built.

Two ideas show up everywhere in the language's design: abstractions are
meant to compile away, and the programmer is meant to be able to see what
the compiler decided. [The principles](../principles.md) spell out where
the language is going, and [the specification](../spec/overview.md) is
its normative description.

- [Getting started](02-getting-started.md) builds the compiler and runs a
  first program.
- [A tour of the language](03-language-tour.md) walks through what the
  compiler accepts today.
