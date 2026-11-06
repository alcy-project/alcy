// A unary operator passes the type the position names to an unsuffixed
// literal operand, so `-5` reads at `i8` and a negated literal may
// name the signed minimum. A const initializer folds the same way.

struct Pair {
  small: i8,
  wide: f64,
}

const MINIMUM: i8 = -128

const FRACTION: f32 = -1.5

const ZERO: f64 = -0.0

fn take(value: i8) -> i8 {
  ret value
}

fn least() -> i8 {
  ret -128
}

fn main() -> i32 {
  plain: i8 := -5
  if plain != -5i8 {
    ret 1
  }
  minimum: i64 := -9223372036854775808
  if minimum != -9223372036854775808i64 {
    ret 2
  }
  bits: i8 := ~5
  if bits != -6i8 {
    ret 3
  }
  fraction: f32 := -1.5
  if fraction != -1.5f32 {
    ret 4
  }
  if take(-7) != -7i8 {
    ret 5
  }
  if least() != -128i8 {
    ret 6
  }
  pair := Pair { small: -5, wide: -1.5 }
  if pair.small != -5i8 {
    ret 7
  }
  if MINIMUM != -128i8 || FRACTION != -1.5f32 {
    ret 8
  }
  // A cast's target is a position of the same kind, and it reaches the
  // digits through the `-` or `~` the literal wears.
  if -9223372036854775808 as i64 != minimum {
    ret 9
  }
  wide: i64 := 3000000000
  if -3000000000 as i64 != 0i64 - wide {
    ret 10
  }
  if ~4294967295 as u32 != 0u32 {
    ret 11
  }
  // The target claims the digits only where it holds them: a magnitude
  // the default `i32` holds keeps it, and the cast then truncates.
  if -300 as u8 != 212u8 {
    ret 12
  }
  println("{} {} {} {}", plain, minimum, bits, ZERO)
  ret 0
}
