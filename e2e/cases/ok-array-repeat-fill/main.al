// A repeat of a value whose bytes are all equal lowers to one byte fill,
// and a value whose bytes differ keeps one store per slot. Both must land
// the same bytes in the array.
fn main() -> i32 {
  mut ok := true

  zeros := [0u8; 64]
  mut zero_sum := 0
  for x in zeros {
    zero_sum += x as i32
  }
  if zero_sum != 0 {
    ok = false
  }

  sevens := [7u8; 12]
  mut seven_sum := 0
  for x in sevens {
    seven_sum += x as i32
  }
  if seven_sum != 84 {
    ok = false
  }

  flags := [true; 40]
  mut trues := 0
  for f in flags {
    if f {
      trues += 1
    }
  }
  if trues != 40 {
    ok = false
  }

  even_bytes := [0x0101u16; 8]
  mut even_sum := 0
  for v in even_bytes {
    even_sum += v as i32
  }
  if even_sum != 2056 {
    ok = false
  }

  mixed_bytes := [0x0102u16; 8]
  mut mixed_sum := 0
  for v in mixed_bytes {
    mixed_sum += v as i32
  }
  if mixed_sum != 2064 {
    ok = false
  }

  zero_floats := [0.0f64; 16]
  mut float_sum := 0.0
  for v in zero_floats {
    float_sum += v
  }
  if float_sum != 0.0 {
    ok = false
  }

  byte := 3u8
  runtime := [byte; 100]
  mut runtime_sum := 0
  for v in runtime {
    runtime_sum += v as i32
  }
  if runtime_sum != 300 {
    ok = false
  }

  wide := [200u8; 5]
  mut wide_sum := 0
  for v in wide {
    wide_sum += v as i32
  }
  if wide_sum != 1000 {
    ok = false
  }

  if !ok {
    ret 1
  }
  ret 0
}
