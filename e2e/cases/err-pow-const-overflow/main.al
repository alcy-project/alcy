// A constant power overflows when the mathematical result does not
// fit, whatever the squaring path was.
const N: i32 = 3 ** 20

fn main() -> i32 {
  ret N
}
