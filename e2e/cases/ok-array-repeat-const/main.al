// A repeat count is a literal, a value parameter, or a path to a
// const integer value, the same grammar the array type's length takes.
const WIDTH: usize = 4

fn fill<N: usize>(x: u8) -> [u8; N] {
  ret [x; N]
}

fn main() -> i32 {
  a: [u8; WIDTH] := [0u8; WIDTH]
  b := fill::<3>(7u8)
  ret (a.len() + b.len()) as i32
}
