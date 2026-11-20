// `**` is repeated squaring: a set exponent bit multiplies into the
// result and a surviving exponent squares the base, on every integer
// width and sign the same way.
fn main() -> i32 {
  // Literal exponents fold at compile time; variable ones run the loop.
  if 2i32 ** 16 != 65536 {
    ret 1
  }
  mut exponent := 5
  if 3 ** exponent != 243 {
    ret 2
  }
  // A zero exponent answers one, even for a zero base.
  if 7u64 ** 0u64 != 1u64 {
    ret 3
  }
  if 0 ** 0 != 1 {
    ret 4
  }
  // Narrow widths wrap their own type.
  if 3u8 ** 4u8 != 81u8 {
    ret 5
  }
  // A high unsigned exponent walks its bits without a sign to preserve.
  mut many := 200u8
  if 1u8 ** many != 1u8 {
    ret 6
  }
  // A negative base carries its sign by the exponent's parity, and the
  // smallest i64 is reachable: (-2) ** 63 is exactly MIN.
  if (-2i64) ** 63i64 != -9223372036854775808i64 {
    ret 7
  }
  if (-2i64) ** 62i64 != 4611686018427387904i64 {
    ret 8
  }
  ret 0
}
