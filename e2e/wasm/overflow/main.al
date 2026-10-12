// A checked build aborts on an overflow through the direct wasm backend
// too (ADR-0068): the entry is realized from the same overlay the LLVM
// emitter reads.
fn main() -> i32 {
  a := 2147483647i32
  ret a + 1
}
