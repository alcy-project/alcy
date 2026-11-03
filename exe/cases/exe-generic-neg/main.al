// A builtin scalar satisfies an arithmetic bound without an
// implementation: `-x` on a parameter replaces with the subtraction
// the concrete body writes (ADR-0053, ADR-0079).

fn negate<T: Neg>(x: T) -> T {
  ret -x
}

fn main() -> i32 {
  if negate(5i32) != -5 {
    ret 1
  }
  if negate(-3i32) != 3 {
    ret 2
  }
  if negate(2.5f32) != -2.5f32 {
    ret 3
  }
  if negate(1.5f64) != -1.5 {
    ret 4
  }
  ret 0
}
