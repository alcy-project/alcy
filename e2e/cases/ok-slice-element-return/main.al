// A read through a slice view answers a plain element, so returning it
// does not hand out the view: the element is a copy the caller owns.

fn first(s: &[i32]) -> i32 {
  ret s[0]
}

fn main() -> i32 {
  a := [1i32, 2i32, 3i32]
  s := a.as_slice()
  if first(s) != 1 {
    ret 1
  }
  x := s[2]
  if x != 3 {
    ret 2
  }
  ret 0
}
