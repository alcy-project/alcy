// A constant loop that would run without bound stops at the engine's
// quota and says so, the way the specification asks for a distinct
// unbounded-evaluation error (docs/spec/comp.md).
comp fn count(comp n: i32) -> i32 {
  ret comp {
    mut i := 0
    while i < n {
      i = i + 1
    }
    i
  }
}

fn main() {
  const x := count(3000000)
  _ := x
}
