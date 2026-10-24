// A scanner with a string literal and a line comment.
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
  ret b == 0x20u8 || b == 0x09u8 || b == 0x0au8
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
    if b == 0x2fu8 && i + 1 < n && src.byte(i + 1) == 0x2fu8 {
      while i < n && src.byte(i) != 0x0au8 {
        i = i + 1
      }
      continue
    }
    if b == 0x22u8 {
      start := i
      i = i + 1
      while i < n && src.byte(i) != 0x22u8 {
        i = i + 1
      }
      if i < n {
        i = i + 1
      }
      out.push(Token { kind: 4, span: Span { start: start, end: i } })
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
    out.push(Token { kind: 3, span: Span { start: i - 1, end: i } })
  }
  ret out
}

fn text_of(src: str, t: &Token) -> str {
  ret src.byte_slice(t.span.start, t.span.end)
}

fn main() -> i32 {
  src := "let s = \"hi\" // note\n+"
  tokens := lex(src)
  if tokens.len() != 5 {
    ret 1
  }
  if text_of(src, tokens.at(0).or_panic_with_message("expected a value")) != "let" {
    ret 2
  }
  if text_of(src, tokens.at(3).or_panic_with_message("expected a value")) != "\"hi\"" {
    ret 3
  }
  if text_of(src, tokens.at(4).or_panic_with_message("expected a value")) != "+" {
    ret 4
  }
  ret 0
}
