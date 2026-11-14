// A repeat of a value whose bytes are all equal lowers to one byte fill,
// and a repeat of any other value lowers to one store plus copies of the
// prefix already written.
fn main() {
  zeros := [0u8; 64]
  mut zero_sum := 0
  for x in zeros {
    zero_sum += x as i32
  }
  println("{}", zero_sum)

  sevens := [7u8; 12]
  mut seven_sum := 0
  for x in sevens {
    seven_sum += x as i32
  }
  println("{}", seven_sum)

  flags := [true; 40]
  mut trues := 0
  for f in flags {
    if f {
      trues += 1
    }
  }
  println("{}", trues)

  even_bytes := [0x0101u16; 8]
  mut even_sum := 0
  for v in even_bytes {
    even_sum += v as i32
  }
  println("{}", even_sum)

  mixed_bytes := [0x0102u16; 8]
  mut mixed_sum := 0
  for v in mixed_bytes {
    mixed_sum += v as i32
  }
  println("{}", mixed_sum)

  // A count no power of two reaches, so the copies end on a range
  // shorter than the prefix they read.
  odd_half_words := [0x0102u16; 5]
  mut odd_half_sum := 0
  for v in odd_half_words {
    odd_half_sum += v as i32
  }
  println("{}", odd_half_sum)

  words := [0x01020304u32; 7]
  mut word_sum := 0
  for v in words {
    word_sum += v as i32
  }
  println("{}", word_sum)

  bytes := 3u8
  runtime := [bytes; 100]
  mut runtime_sum := 0
  for v in runtime {
    runtime_sum += v as i32
  }
  println("{}", runtime_sum)
}
