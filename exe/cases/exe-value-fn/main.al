// Generic functions with value parameters run: each turbofish count is
// its own instance, arrays keep that many elements, and a value
// parameter may shape only the signature (ADR-0054).

fn first<T, N: usize>(a: [T; N]) -> T {
  ret a[0]
}

fn tag<T, N: usize>(x: T) -> T {
  ret x
}

fn main() -> i32 {
  a := first::<i32, 3>([11, 2, 3])
  b := first::<u8, 5>([7 as u8, 1 as u8, 2 as u8, 3 as u8, 4 as u8])
  if a != 11 {
    ret 1
  }
  if b != 7 as u8 {
    ret 2
  }
  if tag::<i32, 4>(20i32) != 20 {
    ret 3
  }
  // Index through results: the element type flows out of the value
  // parameter.
  xs := [first::< i32, 3 > ([31, 0, 0]), first::< i32, 5 > ([42, 0, 0, 0, 0])]
  if xs[0] != 31 || xs[1] != 42 {
    ret 4
  }
  ret 0
}
