// The sum is spliced by comp evaluation, not computed at run time: a
// regression in range construction or cursor write-back changes the
// process status (docs/spec/comp.md).
comp fn total() -> i32 {
  mut s := 0
  for i in 0..<5 {
    s += i
  }
  ret s
}

fn main() -> i32 {
  const v := total()
  if v != 10 {
    ret 1
  }
  ret 0
}
