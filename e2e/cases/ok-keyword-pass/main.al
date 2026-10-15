// The keyword table and a digit fold, the next step after scanning.
fn classify(word: str, keywords: &Map<i32>) -> i32 {
  match keywords.get(word) {
    Option::Some(kind) => {
      ret *kind
    }
    Option::None => {
      ret 0
    }
  }
}

fn parse_digits(text: str) -> i64 {
  mut value := 0 as i64
  mut i := 0 as usize
  while i < text.len() {
    b := text.byte(i)
    if b < 0x30u8 || b > 0x39u8 {
      break
    }
    value = value * 10 + ((b - 0x30u8) as i64)
    i = i + 1
  }
  ret value
}

fn main() -> i32 {
  mut keywords := Map::<i32>::new()
  _ := keywords.insert("let", 10)
  _ := keywords.insert("fn", 11)
  if classify("fn", &keywords) != 11 { ret 1 }
  if classify("let", &keywords) != 10 { ret 2 }
  if classify("x", &keywords) != 0 { ret 3 }
  if parse_digits("42") != 42 { ret 4 }
  if parse_digits("0") != 0 { ret 5 }
  ret 0
}
