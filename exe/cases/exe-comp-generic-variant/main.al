// A layout query in a generic body reads the type from its own
// instantiation, so two callers of the same generic see their own
// answer rather than the first one the evaluator happened to record.
fn width(comp n: usize) -> usize {
  ret n
}

fn probe<T>(x: T) -> usize {
  const k := size_of::<T>()
  ret width(k)
}

fn main() -> i32 {
  a := probe(1u8)
  b := probe(1u64)
  println("{} {}", a, b)
  ret 0
}
