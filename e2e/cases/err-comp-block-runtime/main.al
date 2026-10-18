// A comp block cannot read a runtime local either: the block body is
// evaluated at compile time, so the read has no value to take.
fn double(comp n: usize) -> usize {
  ret n * 2
}

fn main() -> i32 {
  mut runtime_val := 3usize
  n := comp { double(comp { y := runtime_val; y }) }
  ret n as i32
}
