// A recursive emission appends a semantic value while the outer string
// is still held. Copying it keeps the byte read valid.
fn main() -> i32 {
  const c := comp { str_byte("abc", str_len("x")) }
  if c != 98u8 {
    ret 1
  }
  ret 0
}
