// A nominal element compares through PartialEq, which the builtin
// element-wise walk does not reach yet, so the tuple has no
// comparison.

struct P {
  x: i32,
}

fn main() -> i32 {
  if (P { x: 1 },) == (P { x: 1 },) {
    ret 0
  }
  ret 1
}
