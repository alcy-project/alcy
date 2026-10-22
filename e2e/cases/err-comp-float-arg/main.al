// A float is not a type a comp-known value can have
// (docs/spec/comp.md), so a const item of one is not comp-known even
// though the binding is. The argument is refused where it is written
// rather than in the evaluator.
comp fn twice(comp x: f64) -> f64 {
  ret x * 2.0
}

const K: f64 = 2.5

fn main() {
  const y := twice(K)
  _ := y
}
