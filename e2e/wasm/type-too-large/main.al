// A size is the offset a pointer carries: 2^32 bytes fits the 64-bit
// `isize` of a native target and not the 32-bit one a wasm module
// addresses, so this target refuses the shape rather than wrapping.

fn main() -> i32 {
  println("{}", size_of::<[[u8; 65536]; 65536]>())
  ret 0
}
