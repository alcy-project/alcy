// `split_at` answers the two views around a scalar boundary;
// anywhere else is `None`.
fn main() -> i32 {
  (a, b) := "hello".split_at(2).or_panic("offset 2 is a boundary")
  if a != "he" {
    ret 1
  }
  if b != "llo" {
    ret 2
  }
  (c, d) := "hello".split_at(0).or_panic("offset 0 is a boundary")
  if c != "" {
    ret 3
  }
  if d != "hello" {
    ret 4
  }
  (e, f) := "hello".split_at(5).or_panic("offset 5 is a boundary")
  if e != "hello" {
    ret 5
  }
  if f != "" {
    ret 6
  }
  s := format("abc")
  (g, h) := s.split_at(1).or_panic("offset 1 is a boundary")
  if g != "a" {
    ret 7
  }
  if h != "bc" {
    ret 8
  }
  ret 0
}
