// A fill too large to expand into stores: the code generator keeps it
// as a `memset`, which the runtime answers because a freestanding
// program links no libc to answer it (ADR-0052).
fn main() -> i32 {
  mut ones := [1u8; 4096]
  zeros := [0u8; 4096]
  ones[4095] = 9
  println("{} {} {}", ones[0], ones[4095], zeros[4095])
  ret 0
}
