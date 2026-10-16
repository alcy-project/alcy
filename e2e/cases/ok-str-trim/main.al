// ASCII whitespace comes off a view without allocating.
fn main() -> i32 {
  if "  hi  ".trim() != "hi" {
    ret 1
  }
  if "  hi  ".trim_start() != "hi  " {
    ret 2
  }
  if "  hi  ".trim_end() != "  hi" {
    ret 3
  }
  if "   ".trim() != "" {
    ret 4
  }
  if "abc".trim() != "abc" {
    ret 5
  }
  s := format("\t\n ok \r\n")
  if s.trim() != "ok" {
    ret 6
  }
  if s.trim().len() != 2 {
    ret 7
  }
  ret 0
}
