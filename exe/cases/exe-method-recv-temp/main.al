// A temporary method receiver is materialized and retired.
fn main() -> i32 {
  s := format("MiXeD").to_ascii_lowercase()
  if s.as_str() != "mixed" { ret 1 }
  t := format("AB").trim_start_matches("A")
  if t != "B" { ret 2 }
  ret 0
}
