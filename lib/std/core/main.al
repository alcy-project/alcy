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
