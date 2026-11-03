// A size that fits `isize` is a size, however large it is.

fn main() -> i32 {
  println("{}", size_of::<[[u8; 65536]; 65536]>())
  ret 0
}
