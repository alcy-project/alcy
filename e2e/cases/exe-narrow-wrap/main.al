// A narrow integer is the width its type names: every 8-bit pair wraps
// modulo 256, and a 16-bit pair the same way, so a result that leaked a
// wider type shows up as a mismatch.

fn to_i8(b: u8) -> i8 {
  if b < 128u8 {
    ret b as i8
  }
  ret (b as i32 - 256) as i8
}

fn to_i16(w: u16) -> i16 {
  if w < 32768u16 {
    ret w as i16
  }
  ret (w as i32 - 65536) as i16
}

fn to_u8(x: i64) -> u8 {
  ret (x & 255) as u8
}

fn to_u16(x: i64) -> u16 {
  ret (x & 65535) as u16
}

fn main() -> i32 {
  mut outer := 0
  while outer < 256 {
    a := to_u8(outer as i64)
    mut inner := 0
    while inner < 256 {
      b := to_u8(inner as i64)
      if wrapping_add(a, b) != (((a as u32) + (b as u32)) & 255u32) as u8 {
        ret 1
      }
      if wrapping_sub(a, b) != (((a as u32) + 256u32 - (b as u32)) & 255u32) as u8 {
        ret 2
      }
      if wrapping_mul(a, b) != (((a as u32) * (b as u32)) & 255u32) as u8 {
        ret 3
      }
      if a << 1u8 != ((((a as u32) << 1u32) & 255u32) as u8) {
        ret 4
      }
      if (a | b) != ((a as u32) | (b as u32)) as u8 {
        ret 5
      }
      if (a & b) != ((a as u32) & (b as u32)) as u8 {
        ret 6
      }
      if (a ^ b) != ((a as u32) ^ (b as u32)) as u8 {
        ret 7
      }
      if b != 0u8 {
        if a / b != ((a as u32) / (b as u32)) as u8 {
          ret 8
        }
        if a % b != ((a as u32) % (b as u32)) as u8 {
          ret 9
        }
      }

      x := to_i8(a)
      y := to_i8(b)
      if wrapping_add(x, y) != to_i8(to_u8((x as i64) + (y as i64))) {
        ret 10
      }
      if wrapping_sub(x, y) != to_i8(to_u8((x as i64) - (y as i64))) {
        ret 11
      }
      if wrapping_mul(x, y) != to_i8(to_u8((x as i64) * (y as i64))) {
        ret 12
      }
      if (x < y) != ((x as i32) < (y as i32)) {
        ret 13
      }
      if y != 0i8 && !(x == -128i8 && y == -1i8) {
        if x / y != ((x as i32) / (y as i32)) as i8 {
          ret 14
        }
        if x % y != ((x as i32) % (y as i32)) as i8 {
          ret 15
        }
      }
      inner = inner + 1
    }
    outer = outer + 1
  }

  // The 16-bit widths wrap the same way, over pairs the generator walks
  // rather than all of them.
  mut seed := 123456789 as u64
  mut step := 0
  while step < 4096 {
    seed = wrapping_add(wrapping_mul(seed, 6364136223846793005u64), 1442695040888963407u64)
    p := to_u16(((seed >> 33u64) & 65535u64) as i64)
    q := to_u16(((seed >> 12u64) & 65535u64) as i64)
    if wrapping_add(p, q) != to_u16((p as i64) + (q as i64)) {
      ret 16
    }
    if wrapping_sub(p, q) != to_u16((p as i64) - (q as i64)) {
      ret 17
    }
    if wrapping_mul(p, q) != to_u16((p as i64) * (q as i64)) {
      ret 18
    }
    sp := to_i16(p)
    sq := to_i16(q)
    if wrapping_add(sp, sq) != to_i16(to_u16((sp as i64) + (sq as i64))) {
      ret 19
    }
    if wrapping_sub(sp, sq) != to_i16(to_u16((sp as i64) - (sq as i64))) {
      ret 20
    }
    if wrapping_mul(sp, sq) != to_i16(to_u16((sp as i64) * (sq as i64))) {
      ret 21
    }
    if q != 0u16 {
      if p / q != ((p as u32) / (q as u32)) as u16 {
        ret 22
      }
      if p % q != ((p as u32) % (q as u32)) as u16 {
        ret 23
      }
    }
    if sq != 0i16 && !(sp == -32768i16 && sq == -1i16) {
      if sp / sq != ((sp as i32) / (sq as i32)) as i16 {
        ret 24
      }
      if sp % sq != ((sp as i32) % (sq as i32)) as i16 {
        ret 25
      }
    }
    step = step + 1
  }
  ret 0
}
