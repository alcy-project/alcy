// A view is cut out of an owned string too.
fn main() -> i32 {
  s := String::from("let x")
  if s.byte_slice(0, 3) != "let" {
    ret 1
  }
  if s.byte_slice(4, 5) != "x" {
    ret 2
  }
  if s.byte_slice(4, 99) != "x" {
    ret 3
  }
  ret 0
}
