// Repeated matches come off each end; an empty pattern is left alone.
fn main() -> i32 {
  if "/// doc".trim_start_matches("/") != " doc" {
    ret 1
  }
  if "ababab".trim_start_matches("ab") != "" {
    ret 2
  }
  if "xabc".trim_start_matches("ab") != "xabc" {
    ret 3
  }
  if "abcxx".trim_end_matches("x") != "abc" {
    ret 4
  }
  if "abc".trim_end_matches("x") != "abc" {
    ret 5
  }
  if "abc".trim_start_matches("") != "abc" {
    ret 6
  }
  if "abc".trim_end_matches("") != "abc" {
    ret 7
  }
  s := format("...x...")
  if s.trim_start_matches(".") != "x..." {
    ret 8
  }
  if s.trim_end_matches(".") != "...x" {
    ret 9
  }
  ret 0
}
