# Getting started

## Try it in the playground

The [playground](/playground/) runs the compiler as a WebAssembly
module inside the page. Type or pick a program, press **Check** to have
it analyzed, and **Run** to compile and execute it. No server is
involved: the same compiler binary that CI builds for the site does the
work in a worker in your browser.

## Build the compiler

Building alcy natively needs GN, Ninja, Clang, LLD, and libc++. On Linux
and macOS, `nix develop` provides all of them; `compiler/docs/build.md`
covers the other setups.

```sh
nix develop
uv run ./tools/build.py
```

The compiler lands in `out/build/alcy`. Compile and run a single file
with it:

```sh
out/build/alcy compile hello.al -o hello
./hello
```

Run the test suite with:

```sh
uv run ./tools/run.py --target=tests
```

## Hello, world

Put this in `hello.al`:

```alcy
fn main() {
  println("hello, alcy")
}
```

`main` is the entry point, and a program is one or more `fn` items. This
`main` takes no parameters and returns nothing, so the process exits
with status 0 after the line is printed.

The same program can be pasted into the playground and run there
unchanged. The guide's examples are written to work that way: each
`alcy` block is a complete program, compiled when this site is built.
