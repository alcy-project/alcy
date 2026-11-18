// A `str` method reads its receiver through a reference and a comparison
// orders two strings by their bytes; comp evaluation reads both, so a
// comp block splices the value the runtime produces.

comp fn ordered(a: str, b: str) -> bool {
  ret a < b
}

comp fn first_two(s: str) -> str {
  match s.byte_slice(1, 3) {
    Option::Some(v) => {
      ret v
    }
    Option::None => {
      ret ""
    }
  }
}

fn main() {
  println("len {}", comp { "aébc".len_bytes() })
  println("empty {}", comp { "".is_empty() })
  println("byte {}", comp { "abc".byte(1) })
  println("slice {}", comp { first_two("abcdef") })
  println("lt {}", comp { "abc" < "abd" })
  println("prefix {}", comp { "ab" < "abc" })
  println("ge {}", comp { "abc" >= "abc" })
  println("fn {}", comp { ordered("abc", "abd") })
}
