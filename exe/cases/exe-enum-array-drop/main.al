// A destructible field reached through an enum's byte-addressed
// payload carries its own type: a byte-typed address made the array's
// element projection index the element type itself, which the LLVM
// emitter refused.
enum Maybe<T, N: usize> {
  Some([T; N]),
  None,
}

fn main() -> i32 {
  m := Maybe::<String, 2>::Some([String::from("a"), String::from("bb")])
  match m {
    Maybe::Some(x) => {
      ret x[1].len_bytes() as i32
    }
    Maybe::None => {
      ret 0
    }
  }
}
