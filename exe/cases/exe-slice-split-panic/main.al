// A boundary past the slice's end panics rather than standing for it.
fn main() -> i32 {
  a := [10i32, 20i32, 30i32]
  (head, tail) := a.as_slice().split_at(9)
  ret (head.len() + tail.len()) as i32
}
