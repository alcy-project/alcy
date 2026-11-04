// A repeat of a value whose bytes are all equal lowers to one byte fill,
// and a value whose bytes differ keeps one store per slot.
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

  bytes := 3u8
  runtime := [bytes; 100]
  mut runtime_sum := 0
  for v in runtime {
    runtime_sum += v as i32
  }
  println("{}", runtime_sum)
}
