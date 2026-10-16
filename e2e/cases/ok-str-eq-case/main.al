// ASCII case folding only; other bytes must match exactly.
fn main() -> i32 {
  if !"Hello".eq_ignore_ascii_case("hello") {
    ret 1
  }
  if !"HELLO".eq_ignore_ascii_case("hello") {
    ret 2
  }
  if "hello".eq_ignore_ascii_case("world") {
    ret 3
  }
  if "hello".eq_ignore_ascii_case("hell") {
    ret 4
  }
  if !"abc123".eq_ignore_ascii_case("ABC123") {
    ret 5
  }
  if "abc".eq_ignore_ascii_case("abd") {
    ret 6
  }
  s := format("MiXeD")
  if !s.eq_ignore_ascii_case("mixed") {
    ret 7
  }
  if !"".eq_ignore_ascii_case("") {
    ret 8
  }
  ret 0
}
