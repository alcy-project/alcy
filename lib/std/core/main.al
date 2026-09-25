// Toolchain standard library, core member.
//
// This file ships inside the compiler binary and is injected as a
// prelude module into every compilation, so its public items need no
// imports. It declares compiler-provided intrinsics only; everything
// else in core arrives as ordinary library code later.

pub intrinsic fn memcopy(dst: &mut u8, src: &u8, n: usize);

pub intrinsic fn print(msg: str);

pub intrinsic fn println(msg: str);

pub intrinsic fn panic(msg: str) -> !;

pub intrinsic fn str_len(s: str) -> usize;

pub intrinsic fn str_byte(s: str, i: usize) -> u8;

pub intrinsic fn str_slice(s: str, start: usize, end: usize) -> str;

pub struct WriteOutcome { written: usize, total: usize }

// Formats `args` into `buf` by compile-time expansion; see
// docs/spec/fmt.md. Calls check and expand through compiler support
// (like the print intrinsics); the body never executes, and reaching
// it aborts. The `[u8; 0]` and `()` parameter types are placeholders:
// calls accept any byte-array size and any tuple arity through custom
// checking, since the language cannot name them yet.
pub fn write(comp fmt: str, buf: &mut [u8; 0], args: ()) -> WriteOutcome {
  panic("fmt::write must expand")
}
