// A block whose tail reads a runtime local is a runtime value, so a
// call site that reads it as a compile-time argument is refused even
// though the block itself is not marked comp.
fn double(comp n: usize) -> usize {
  ret n * 2
}

fn main() -> i32 {
  mut runtime_val := 3
  n :=
    double(
      {
        y := runtime_val
        y as usize
      }
    )
  ret n as i32
}
