// A match arm's value is comp-known only when the scrutinee and every
// arm are; a runtime scrutinee makes the whole match a runtime value,
// so the argument is refused at the call rather than reaching the
// compile-time evaluator.
fn double(comp n: usize) -> usize {
  ret n * 2
}

fn fallback() -> usize {
  ret 4usize
}

fn main() -> i32 {
  mut runtime_val := 3
  n :=
    double(
      match runtime_val {
        0 => 2usize,
        _ => fallback(),
      }
    )
  ret n as i32
}
