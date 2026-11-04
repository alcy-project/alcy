// An argument that calls a method formats through `Display` as well: the
// argument records the call it makes, and the piece records the dispatch
// it reaches, so the two stay apart.

fn main() -> i32 {
  upper := format("[{}]", "ab".to_ascii_uppercase())
  if upper.as_str() != "[AB]" {
    ret 1
  }
  replaced := format("[{}]", "a-b".replace("-", "+"))
  if replaced.as_str() != "[a+b]" {
    ret 2
  }
  ret 0
}
