fn main() -> i32 {
  const a := true
  const b := 1i32
  const c := 2i32
  const d := 3i32
  if a && b < c < d {
    ret 0
  }
  ret 9
}
