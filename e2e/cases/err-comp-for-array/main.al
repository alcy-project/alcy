// The slice cursor a `for` loop iterates carries raw pointers, so the
// evaluator cannot follow it: the refusal names constant evaluation
// rather than a construct the lowering could not express
// (docs/spec/comp.md).
comp fn total() -> i32 {
  mut s := 0
  for x in [1, 2, 3] {
    s += x
  }
  ret s
}

fn main() {
  const x := total()
  _ := x
}
