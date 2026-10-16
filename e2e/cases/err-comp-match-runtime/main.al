// A comp match scrutinee must be known at compile time: a runtime local
// is refused rather than evaluated while the program is being built.
fn double(comp n: usize) -> usize {
  ret n * 2
}

fn main() -> i32 {
  mut runtime_val := 3usize
  n := comp {
    double(
      match runtime_val {
        _ => 2usize,
      }
    )
  }
  ret n as i32
}
