// Two fields under one name would leave a literal looking for both of
// them and an access reaching the first.
struct S {
  a: i32,
  a: i64,
}

fn main() -> i32 {
  ret 0
}
