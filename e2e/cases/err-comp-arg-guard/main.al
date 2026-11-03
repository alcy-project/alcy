// A guard tests at compile time along with the arm it belongs to, so
// a guard reading a runtime local has no value to decide with; the
// whole match is then not comp-known and the argument is refused.
fn pick(comp n: usize) -> usize {
  ret n
}

fn main() -> i32 {
  mut runtime_val := 3
  n :=
    pick(
      match 1usize {
        k if runtime_val > 0 => k,
        _ => 0usize,
      }
    )
  ret n as i32
}
