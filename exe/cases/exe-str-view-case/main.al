// A view folds ASCII case into a new string.
fn main() -> i32 {
  a := "MiXeD".to_ascii_lowercase()
  if a.as_str() != "mixed" {
    ret 1
  }
  b := "MiXeD".to_ascii_uppercase()
  if b.as_str() != "MIXED" {
    ret 2
  }
  if "".to_ascii_lowercase().len_bytes() != 0 {
    ret 3
  }
  c := "aZ".to_ascii_lowercase()
  if c.as_str() != "az" {
    ret 4
  }
  ret 0
}
