// Generic functions with value parameters: the turbofish names the
// count, the instance's parameter type carries it, and a value slot
// never infers from an array. An associated function reached through a
// value-argument path resolves the same way (ADR-0054).

fn first<T, N: usize>(a: [T; N]) -> T {
  ret a[0]
}

fn tag<T, N: usize>(x: T) -> T {
  ret x
}

struct Arr<T, N: usize> { data: [T; N] }

impl<T, N: usize> Arr<T, N> {
  fn make() -> Self {
    ret Arr { data: [7, 8, 9] }
  }
}

fn main() -> i32 {
  a := first::<i32, 3>([11, 2, 3])
  b := first::<u8, 5>([7 as u8, 2 as u8, 3 as u8, 4 as u8, 5 as u8])
  n := tag::<i32, 9>(4i32)
  m := Arr::<i32, 3>::make().data[2]
  ret a + (b as i32) + n + m - 31
}
