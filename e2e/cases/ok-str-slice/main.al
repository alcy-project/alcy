// A view is cut by byte offsets into a span.
fn main() -> i32 {
  s := "let x"
  if s.byte_slice(0, 3).or_panic("boundaries") != "let" {
    ret 1
  }
  if s.byte_slice(4, 5).or_panic("boundaries") != "x" {
    ret 2
  }
  if s.byte_slice(4, 99).is_some() {
    ret 3
  }
  if s.byte_slice(99, 100).is_some() {
    ret 4
  }
  if s.byte_slice(3, 1).is_some() {
    ret 5
  }
  if s.byte_slice(0, 0).or_panic("empty boundaries") != "" {
    ret 6
  }
  if s.byte_slice(1, 5).or_panic("boundaries") != "et x" {
    ret 7
  }
  ret 0
}
