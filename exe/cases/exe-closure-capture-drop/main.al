// A closure value that owns its environment ends it with the scope:
// the capture's heap storage is freed when the closure dies, whether
// the value is bound or handed to a `fn()` parameter.
fn apply(f: fn (i32) -> i32, x: i32) -> i32 {
  ret f(x)
}

fn main() -> i32 {
  s := format("ab{}", 0)
  g := [s](x: i32) -> x + s.len() as i32
  if g(3) != 6 {
    ret 1
  }
  t := format("cd{}", 1)
  if apply([t](x: i32) -> x + t.len() as i32, 3) != 6 {
    ret 2
  }
  ret 0
}
