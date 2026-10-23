// The compile-time evaluator and the runtime answer the same word:
// every constant below is checked against the same computation over
// runtime operands, so a fold that disagrees fails here.
fn d(a: i32, b: i32) -> i32 {
  ret a / b
}

fn m(a: i32, b: i32) -> i32 {
  ret a % b
}

fn shl(a: i32, b: i32) -> i32 {
  ret a << b
}

fn shr(a: i32, b: i32) -> i32 {
  ret a >> b
}

fn band(a: i32, b: i32) -> i32 {
  ret a & b
}

fn bor(a: i32, b: i32) -> i32 {
  ret a | b
}

fn bxor(a: i32, b: i32) -> i32 {
  ret a ^ b
}

fn to_u8(a: i32) -> i32 {
  ret (a as u8) as i32
}

fn to_i8(a: i32) -> i32 {
  ret (a as i8) as i32
}

fn sub(a: i32, b: i32) -> i32 {
  ret a - b
}

fn mul(a: i32, b: i32) -> i32 {
  ret a * b
}

const K1: i32 = (0 - 7) / 2

const K2: i32 = (0 - 7) % 2

const K3: i32 = 7 / (0 - 2)

const K4: i32 = 7 % (0 - 2)

const K5: i32 = 1 << 31

const K6: i32 = (0 - 1) >> 1

const K7: i32 = 12 & 10

const K8: i32 = 12 | 10

const K9: i32 = 12 ^ 10

const K10: i32 = (0 - 1) & 255

const K11: i32 = (300 as u8) as i32

const K12: i32 = ((0 - 300) as u8) as i32

const K13: i32 = ((0 - 1) as u8) as i32

const K14: i32 = ((0 - 200) as i8) as i32

const K15: i32 = 1000 * 1000

const K16: i32 = (0 - 5) - 1

fn main() -> i32 {
  mut bad := 0
  if K1 != d(0 - 7, 2) {
    bad = bad + 1
  }
  if K2 != m(0 - 7, 2) {
    bad = bad + 1
  }
  if K3 != d(7, 0 - 2) {
    bad = bad + 1
  }
  if K4 != m(7, 0 - 2) {
    bad = bad + 1
  }
  if K5 != shl(1, 31) {
    bad = bad + 1
  }
  if K6 != shr(0 - 1, 1) {
    bad = bad + 1
  }
  if K7 != band(12, 10) {
    bad = bad + 1
  }
  if K8 != bor(12, 10) {
    bad = bad + 1
  }
  if K9 != bxor(12, 10) {
    bad = bad + 1
  }
  if K10 != band(0 - 1, 255) {
    bad = bad + 1
  }
  if K11 != to_u8(300) {
    bad = bad + 1
  }
  if K12 != to_u8(0 - 300) {
    bad = bad + 1
  }
  if K13 != to_u8(0 - 1) {
    bad = bad + 1
  }
  if K14 != to_i8(0 - 200) {
    bad = bad + 1
  }
  if K15 != mul(1000, 1000) {
    bad = bad + 1
  }
  if K16 != sub(sub(0, 5), 1) {
    bad = bad + 1
  }
  ret bad
}
