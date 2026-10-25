// A miniature pipeline: read a source, scan it, report what the keyword
// table does not know, write the report, and read it back.
struct Span {
  start: usize,
  end: usize,
}

struct Token {
  kind: i32,
  span: Span,
}

fn is_digit(b: u8) -> bool {
  ret b >= 0x30u8 && b <= 0x39u8
}

fn is_alpha(b: u8) -> bool {
  ret (b >= 0x61u8 && b <= 0x7au8) || (b >= 0x41u8 && b <= 0x5au8)
}

fn is_alnum(b: u8) -> bool {
  ret is_alpha(b) || is_digit(b)
}

fn is_space(b: u8) -> bool {
  ret b == 0x20u8 || b == 0x0au8
}

fn lex(src: str) -> Vec<Token> {
  mut out := Vec::<Token>::new()
  n := src.len_bytes()
  mut i := 0 as usize
  while i < n {
    b := src.byte(i)
    if is_space(b) {
      i = i + 1
      continue
    }
    if is_alpha(b) {
      start := i
      while i < n && is_alnum(src.byte(i)) {
        i = i + 1
      }
      out.push(Token { kind: 1, span: Span { start: start, end: i } })
      continue
    }
    if is_digit(b) {
      start := i
      while i < n && is_digit(src.byte(i)) {
        i = i + 1
      }
      out.push(Token { kind: 2, span: Span { start: start, end: i } })
      continue
    }
    i = i + 1
  }
  ret out
}

fn report(src: str, tokens: &Vec<Token>, known: &Map<i32>) -> Option<String> {
  mut out := String::new()
  mut bad := 0
  mut i := 0 as usize
  while i < tokens.len() {
    t := tokens.at(i).or_panic_with_message("expected a value")
    if t.kind == 1 {
      word := src.byte_slice(t.span.start, t.span.end)
      if !known.contains(word) {
        text := format("unknown {}\n", word)
        out.push_str(text.as_str())
        bad = bad + 1
      }
    }
    i = i + 1
  }
  if bad == 0 {
    ret Option::None
  }
  ret Option::Some(out)
}

fn main() -> i32 {
  src := "let x 42"
  mut known := Map::<i32>::new()
  _ := known.insert("let", 1)
  tokens := lex(src)
  if tokens.len() != 3 {
    ret 1
  }
  match report(src, &tokens, &known) {
    Option::Some(msgs) => {
      if !write_file("report.txt", msgs.as_str()) {
        ret 2
      }
      match read_file("report.txt") {
        Option::Some(back) => {
          if back.as_str() != "unknown x\n" {
            ret 3
          }
        }
        Option::None => {
          ret 4
        }
      }
    }
    Option::None => {
      ret 5
    }
  }
  ret 0
}
