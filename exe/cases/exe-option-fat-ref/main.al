// An option over a slice reference is not a niche: the reference is fat,
// so the enum carries a discriminant and the payload travels beside it.
// The case exists because the niche path used to accept the shape and
// then build a scalar null for a two-word slot, which is invalid IR.

fn main() -> i32 {
  a := [1i32, 2i32, 3i32]
  s := &a[0..<3]
  some: Option<&[i32]> := Option::Some(s)
  if !some.is_some() {
    ret 1
  }
  match some {
    Option::Some(v) => {
      if v.len() != 3 || *v.at(0).or_panic_with_message("index 0") != 1 {
        ret 2
      }
    }
    Option::None => {
      ret 3
    }
  }
  none: Option<&[i32]> := Option::None
  if none.is_some() {
    ret 4
  }
  match none {
    Option::Some(v) => {
      if v.len() == 0 {
        ret 5
      }
    }
    Option::None => {}
  }
  ret 0
}
