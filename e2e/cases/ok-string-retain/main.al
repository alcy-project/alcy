// `retain` keeps only the bytes its predicate accepts.
fn main() -> i32 {
  mut s := format("hello world")
  s.retain((b: u8) -> b != 0x20u8)
  if s.as_str() != "helloworld" {
    ret 1
  }
  mut t := format("abc")
  t.retain((b: u8) -> false)
  if t.len_bytes() != 0 {
    ret 2
  }
  ret 0
}
