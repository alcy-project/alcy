// A scanner: bytes to tokens with spans, the first pass of a front end.
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

fn lex(src: str) -> Vec<Token> {
  mut out := Vec::<Token>::new()
  n := src.len()
  mut i := 0 as usize
  while i < n {
    b := src.byte(i)
    if b == 0x20u8 {
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
    out.push(Token { kind: 3, span: Span { start: i - 1, end: i } })
  }
  ret out
}

fn text_of(src: str, t: &Token) -> str {
  ret src.slice(t.span.start, t.span.end)
}

fn main() -> i32 {
  src := "let x1 = 42"
  tokens := lex(src)
  if tokens.len() != 4 {
    ret 1
  }
  if text_of(src, tokens.at(0).or_panic_with_message("expected a value")) != "let" {
    ret 2
  }
  if text_of(src, tokens.at(1).or_panic_with_message("expected a value")) != "x1" {
    ret 3
  }
  if tokens.at(2).or_panic_with_message("expected a value").kind != 3 {
    ret 4
  }
  if text_of(src, tokens.at(3).or_panic_with_message("expected a value")) != "42" {
    ret 5
  }
  ret 0
}
