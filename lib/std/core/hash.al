// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: XXH3-64 over bytes, the standard library's one hash.
//
// A scalar port of the vendored C library's `XXH3_64bits` at seed 0
// (`third_party/xxhash`), so the same input hashes the same on both
// sides: the parity case pins the published vectors. Every multiply
// and add wraps explicitly, because a hash's arithmetic is defined to
// wrap while a program's is checked. `Map` hashes its keys through
// `xxh3_64`; see `docs/adr/0074-the-stds-hash-lives-in-core.md`.

const PRIME_1: u64 = 0x9e3779b185ebca87u64

const PRIME_2: u64 = 0xc2b2ae3d27d4eb4fu64

const PRIME_3: u64 = 0x165667b19e3779f9u64

const PRIME_4: u64 = 0x85ebca77c2b2ae63u64

const PRIME_5: u64 = 0x27d4eb2f165667c5u64

const PRIME_32_1: u64 = 0x9e3779b1u64

const PRIME_32_2: u64 = 0x85ebca77u64

const PRIME_32_3: u64 = 0xc2b2ae3du64

const PRIME_MX1: u64 = 0x165667919e3779f9u64

const PRIME_MX2: u64 = 0x9fb21c651e98df25u64

const MASK_32: u64 = 0xffffffffu64

const MASK_8: u64 = 0xffu64

const SECRET_0: u64 = 0xbe4ba423396cfeb8u64

const SECRET_1: u64 = 0x1cad21f72c81017cu64

const SECRET_2: u64 = 0xdb979083e96dd4deu64

const SECRET_3: u64 = 0x1f67b3b7a4a44072u64

const SECRET_4: u64 = 0x78e5c0cc4ee679cbu64

const SECRET_5: u64 = 0x2172ffcc7dd05a82u64

const SECRET_6: u64 = 0x8e2443f7744608b8u64

const SECRET_7: u64 = 0x4c263a81e69035e0u64

const SECRET_8: u64 = 0xcb00c391bb52283cu64

const SECRET_9: u64 = 0xa32e531b8b65d088u64

const SECRET_10: u64 = 0x4ef90da297486471u64

const SECRET_11: u64 = 0xd8acdea946ef1938u64

const SECRET_12: u64 = 0x3f349ce33f76faa8u64

const SECRET_13: u64 = 0x1d4f0bc7c7bbdcf9u64

const SECRET_14: u64 = 0x3159b4cd4be0518au64

const SECRET_15: u64 = 0x647378d9c97e9fc8u64

const SECRET_16: u64 = 0xc3ebd33483acc5eau64

const SECRET_17: u64 = 0xeb6313faffa081c5u64

const SECRET_18: u64 = 0x49daf0b751dd0d17u64

const SECRET_19: u64 = 0x9e68d429265516d3u64

const SECRET_20: u64 = 0xfca1477d58be162bu64

const SECRET_21: u64 = 0xce31d07ad1b8f88fu64

const SECRET_22: u64 = 0x280416958f3acb45u64

const SECRET_23: u64 = 0x7e404bbbcafbd7afu64

fn rotl(x: u64, r: u64) -> u64 {
  ret (x << r) | (x >> (64u64 - r))
}

fn secret_word(i: usize) -> u64 {
  if i == 0 {
    ret SECRET_0
  }
  if i == 1 {
    ret SECRET_1
  }
  if i == 2 {
    ret SECRET_2
  }
  if i == 3 {
    ret SECRET_3
  }
  if i == 4 {
    ret SECRET_4
  }
  if i == 5 {
    ret SECRET_5
  }
  if i == 6 {
    ret SECRET_6
  }
  if i == 7 {
    ret SECRET_7
  }
  if i == 8 {
    ret SECRET_8
  }
  if i == 9 {
    ret SECRET_9
  }
  if i == 10 {
    ret SECRET_10
  }
  if i == 11 {
    ret SECRET_11
  }
  if i == 12 {
    ret SECRET_12
  }
  if i == 13 {
    ret SECRET_13
  }
  if i == 14 {
    ret SECRET_14
  }
  if i == 15 {
    ret SECRET_15
  }
  if i == 16 {
    ret SECRET_16
  }
  if i == 17 {
    ret SECRET_17
  }
  if i == 18 {
    ret SECRET_18
  }
  if i == 19 {
    ret SECRET_19
  }
  if i == 20 {
    ret SECRET_20
  }
  if i == 21 {
    ret SECRET_21
  }
  if i == 22 {
    ret SECRET_22
  }
  ret SECRET_23
}

// Eight secret bytes at any byte offset: the merge and last-stripe
// reads are not word aligned, so a word and its neighbour compose.
fn secret_at(off: usize) -> u64 {
  w := off / 8
  s := (off - w * 8) * 8
  if s == 0 {
    ret secret_word(w)
  }
  su := s as u64
  ret (secret_word(w) >> su) | (secret_word(w + 1) << (64u64 - su))
}

fn lane(s: str, off: usize) -> u64 {
  mut v := str_byte(s, off) as u64
  v = v | ((str_byte(s, off + 1) as u64) << 8u64)
  v = v | ((str_byte(s, off + 2) as u64) << 16u64)
  v = v | ((str_byte(s, off + 3) as u64) << 24u64)
  v = v | ((str_byte(s, off + 4) as u64) << 32u64)
  v = v | ((str_byte(s, off + 5) as u64) << 40u64)
  v = v | ((str_byte(s, off + 6) as u64) << 48u64)
  v = v | ((str_byte(s, off + 7) as u64) << 56u64)
  ret v
}

fn lane32(s: str, off: usize) -> u64 {
  mut v := str_byte(s, off) as u64
  v = v | ((str_byte(s, off + 1) as u64) << 8u64)
  v = v | ((str_byte(s, off + 2) as u64) << 16u64)
  v = v | ((str_byte(s, off + 3) as u64) << 24u64)
  ret v
}

fn swap64(x: u64) -> u64 {
  mut v := (x & MASK_8) << 56u64
  v = v | (((x >> 8u64) & MASK_8) << 48u64)
  v = v | (((x >> 16u64) & MASK_8) << 40u64)
  v = v | (((x >> 24u64) & MASK_8) << 32u64)
  v = v | (((x >> 32u64) & MASK_8) << 24u64)
  v = v | (((x >> 40u64) & MASK_8) << 16u64)
  v = v | (((x >> 48u64) & MASK_8) << 8u64)
  v = v | ((x >> 56u64) & MASK_8)
  ret v
}

// The high word of a 64 by 64 multiply, through 32-bit halves.
fn mul_hi(a: u64, b: u64) -> u64 {
  p00 := wrapping_mul(a & MASK_32, b & MASK_32)
  p01 := wrapping_mul(a & MASK_32, b >> 32u64)
  p10 := wrapping_mul(a >> 32u64, b & MASK_32)
  p11 := wrapping_mul(a >> 32u64, b >> 32u64)
  mid := wrapping_add(wrapping_add(p00 >> 32u64, p01 & MASK_32), p10 & MASK_32)
  ret wrapping_add(wrapping_add(p11, p01 >> 32u64), wrapping_add(p10 >> 32u64, mid >> 32u64))
}

fn mul_fold(a: u64, b: u64) -> u64 {
  ret wrapping_mul(a, b) ^ mul_hi(a, b)
}

fn avalanche(h: u64) -> u64 {
  mut v := h ^ (h >> 37u64)
  v = wrapping_mul(v, PRIME_MX1)
  v = v ^ (v >> 32u64)
  ret v
}

fn avalanche64(h: u64) -> u64 {
  mut v := h ^ (h >> 33u64)
  v = wrapping_mul(v, PRIME_2)
  v = v ^ (v >> 29u64)
  v = wrapping_mul(v, PRIME_3)
  v = v ^ (v >> 32u64)
  ret v
}

fn rrmxmx(h: u64, len: u64) -> u64 {
  mut v := h ^ rotl(h, 49u64) ^ rotl(h, 24u64)
  v = wrapping_mul(v, PRIME_MX2)
  v = v ^ wrapping_add(v >> 35u64, len)
  v = wrapping_mul(v, PRIME_MX2)
  v = v ^ (v >> 28u64)
  ret v
}

fn mix16(s: str, at: usize, base: usize) -> u64 {
  lo := lane(s, at) ^ secret_at(base)
  hi := lane(s, at + 8) ^ secret_at(base + 8)
  ret mul_fold(lo, hi)
}

fn len_1to3(s: str, len: usize) -> u64 {
  c1 := str_byte(s, 0) as u64
  c2 := str_byte(s, len / 2) as u64
  c3 := str_byte(s, len - 1) as u64
  combined := (c1 << 16u64) | (c2 << 24u64) | c3 | ((len as u64) << 8u64)
  bitflip := (secret_word(0) & MASK_32) ^ (secret_word(0) >> 32u64)
  ret avalanche64(combined ^ bitflip)
}

fn len_4to8(s: str, len: usize) -> u64 {
  input1 := lane32(s, 0)
  input2 := lane32(s, len - 4)
  bitflip := secret_word(1) ^ secret_word(2)
  input64 := wrapping_add(input2, input1 << 32u64)
  ret rrmxmx(input64 ^ bitflip, len as u64)
}

fn len_9to16(s: str, len: usize) -> u64 {
  bitflip1 := secret_word(3) ^ secret_word(4)
  bitflip2 := secret_word(5) ^ secret_word(6)
  lo := lane(s, 0) ^ bitflip1
  hi := lane(s, len - 8) ^ bitflip2
  acc := wrapping_add(wrapping_add(wrapping_add(len as u64, swap64(lo)), hi), mul_fold(lo, hi))
  ret avalanche(acc)
}

fn len_0to16(s: str, len: usize) -> u64 {
  if len > 8 {
    ret len_9to16(s, len)
  }
  if len >= 4 {
    ret len_4to8(s, len)
  }
  if len > 0 {
    ret len_1to3(s, len)
  }
  ret avalanche64(secret_word(7) ^ secret_word(8))
}

fn len_17to128(s: str, len: usize) -> u64 {
  mut acc := wrapping_mul(len as u64, PRIME_1)
  if len > 32 {
    if len > 64 {
      if len > 96 {
        acc = wrapping_add(acc, mix16(s, 48, 96))
        acc = wrapping_add(acc, mix16(s, len - 64, 112))
      }
      acc = wrapping_add(acc, mix16(s, 32, 64))
      acc = wrapping_add(acc, mix16(s, len - 48, 80))
    }
    acc = wrapping_add(acc, mix16(s, 16, 32))
    acc = wrapping_add(acc, mix16(s, len - 32, 48))
  }
  acc = wrapping_add(acc, mix16(s, 0, 0))
  acc = wrapping_add(acc, mix16(s, len - 16, 16))
  ret avalanche(acc)
}

fn len_129to240(s: str, len: usize) -> u64 {
  mut acc := wrapping_mul(len as u64, PRIME_1)
  mut i := 0 as usize
  while i < 8 {
    acc = wrapping_add(acc, mix16(s, 16 * i, 16 * i))
    i = i + 1
  }
  mut acc_end := mix16(s, len - 16, 119)
  acc = avalanche(acc)
  rounds := len / 16
  i = 8 as usize
  while i < rounds {
    acc_end = wrapping_add(acc_end, mix16(s, 16 * i, 16 * (i - 8) + 3))
    i = i + 1
  }
  ret avalanche(wrapping_add(acc, acc_end))
}

fn round_lane(a: u64, k: u64) -> u64 {
  ret wrapping_add(wrapping_mul(k & MASK_32, (k >> 32u64) & MASK_32), a)
}

struct Acc {
  a0: u64,
  a1: u64,
  a2: u64,
  a3: u64,
  a4: u64,
  a5: u64,
  a6: u64,
  a7: u64,
}

fn acc_init() -> Acc {
  ret Acc {
    a0: PRIME_32_3,
    a1: PRIME_1,
    a2: PRIME_2,
    a3: PRIME_3,
    a4: PRIME_4,
    a5: PRIME_32_2,
    a6: PRIME_5,
    a7: PRIME_32_1,
  }
}

// One 64-byte stripe into the accumulators, lanes in order: lane 2k
// moves into the odd slot and rounds the even one, lane 2k+1 the other
// way around, exactly as the reference scalar round does.
fn stripe(st: Acc, s: str, at: usize, base: usize) -> Acc {
  mut x0 := st.a0
  mut x1 := st.a1
  mut x2 := st.a2
  mut x3 := st.a3
  mut x4 := st.a4
  mut x5 := st.a5
  mut x6 := st.a6
  mut x7 := st.a7
  mut d := lane(s, at)
  mut k := d ^ secret_at(base)
  x1 = wrapping_add(x1, d)
  x0 = round_lane(x0, k)
  d = lane(s, at + 8)
  k = d ^ secret_at(base + 8)
  x0 = wrapping_add(x0, d)
  x1 = round_lane(x1, k)
  d = lane(s, at + 16)
  k = d ^ secret_at(base + 16)
  x3 = wrapping_add(x3, d)
  x2 = round_lane(x2, k)
  d = lane(s, at + 24)
  k = d ^ secret_at(base + 24)
  x2 = wrapping_add(x2, d)
  x3 = round_lane(x3, k)
  d = lane(s, at + 32)
  k = d ^ secret_at(base + 32)
  x5 = wrapping_add(x5, d)
  x4 = round_lane(x4, k)
  d = lane(s, at + 40)
  k = d ^ secret_at(base + 40)
  x4 = wrapping_add(x4, d)
  x5 = round_lane(x5, k)
  d = lane(s, at + 48)
  k = d ^ secret_at(base + 48)
  x7 = wrapping_add(x7, d)
  x6 = round_lane(x6, k)
  d = lane(s, at + 56)
  k = d ^ secret_at(base + 56)
  x6 = wrapping_add(x6, d)
  x7 = round_lane(x7, k)
  ret Acc { a0: x0, a1: x1, a2: x2, a3: x3, a4: x4, a5: x5, a6: x6, a7: x7 }
}

fn scramble(st: Acc, base: usize) -> Acc {
  mut x0 := st.a0
  mut x1 := st.a1
  mut x2 := st.a2
  mut x3 := st.a3
  mut x4 := st.a4
  mut x5 := st.a5
  mut x6 := st.a6
  mut x7 := st.a7
  x0 = wrapping_mul((x0 ^ (x0 >> 47u64)) ^ secret_at(base), PRIME_32_1)
  x1 = wrapping_mul((x1 ^ (x1 >> 47u64)) ^ secret_at(base + 8), PRIME_32_1)
  x2 = wrapping_mul((x2 ^ (x2 >> 47u64)) ^ secret_at(base + 16), PRIME_32_1)
  x3 = wrapping_mul((x3 ^ (x3 >> 47u64)) ^ secret_at(base + 24), PRIME_32_1)
  x4 = wrapping_mul((x4 ^ (x4 >> 47u64)) ^ secret_at(base + 32), PRIME_32_1)
  x5 = wrapping_mul((x5 ^ (x5 >> 47u64)) ^ secret_at(base + 40), PRIME_32_1)
  x6 = wrapping_mul((x6 ^ (x6 >> 47u64)) ^ secret_at(base + 48), PRIME_32_1)
  x7 = wrapping_mul((x7 ^ (x7 >> 47u64)) ^ secret_at(base + 56), PRIME_32_1)
  ret Acc { a0: x0, a1: x1, a2: x2, a3: x3, a4: x4, a5: x5, a6: x6, a7: x7 }
}

fn len_over_240(s: str, len: usize) -> u64 {
  mut st := acc_init()
  blocks := (len - 1) / 1024
  mut n := 0 as usize
  while n < blocks {
    mut stripe_n := 0 as usize
    while stripe_n < 16 {
      st = stripe(st, s, n * 1024 + stripe_n * 64, stripe_n * 8)
      stripe_n = stripe_n + 1
    }
    st = scramble(st, 128)
    n = n + 1
  }
  rest := n * 1024
  stripes := ((len - 1) - rest) / 64
  mut stripe_n := 0 as usize
  while stripe_n < stripes {
    st = stripe(st, s, rest + stripe_n * 64, stripe_n * 8)
    stripe_n = stripe_n + 1
  }
  st = stripe(st, s, len - 64, 121)
  mut result := wrapping_mul(len as u64, PRIME_1)
  result = wrapping_add(result, mul_fold(st.a0 ^ secret_at(11), st.a1 ^ secret_at(19)))
  result = wrapping_add(result, mul_fold(st.a2 ^ secret_at(27), st.a3 ^ secret_at(35)))
  result = wrapping_add(result, mul_fold(st.a4 ^ secret_at(43), st.a5 ^ secret_at(51)))
  result = wrapping_add(result, mul_fold(st.a6 ^ secret_at(59), st.a7 ^ secret_at(67)))
  ret avalanche(result)
}

pub fn xxh3_64(s: str) -> u64 {
  len := str_len(s)
  if len > 240 {
    ret len_over_240(s, len)
  }
  if len > 128 {
    ret len_129to240(s, len)
  }
  if len > 16 {
    ret len_17to128(s, len)
  }
  ret len_0to16(s, len)
}
