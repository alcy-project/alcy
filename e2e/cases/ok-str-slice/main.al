// A view is cut by byte offsets into a span.
fn main() -> i32 {
  s := "let x"
  if s.byte_slice(0, 3) != "let" {
    ret 1
  }
  if s.byte_slice(4, 5) != "x" {
    ret 2
  }
  if s.byte_slice(4, 99) != "x" {
    ret 3
  }
  if s.byte_slice(99, 100) != "" {
    ret 4
  }
  if s.byte_slice(3, 1) != "" {
    ret 5
  }
  if s.byte_slice(0, 0) != "" {
    ret 6
  }
  if s.byte_slice(1, 5) != "et x" {
    ret 7
  }
  ret 0
}
