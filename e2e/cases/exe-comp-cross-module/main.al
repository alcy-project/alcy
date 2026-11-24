use self::shapes::twice;

fn main() -> i32 {
  const c := twice(21)
  if c != 42 {
    ret 1
  }
  ret 0
}
