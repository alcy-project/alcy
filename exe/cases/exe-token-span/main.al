// A token carries its kind and the byte span it covers, the shape a
// lexer hands a parser.
struct Span {
  start: usize,
  end: usize,
}

struct Token {
  kind: i32,
  span: Span,
}

fn slice_of(src: str, span: Span) -> str {
  ret src.slice(span.start, span.end)
}

fn main() -> i32 {
  src := "let x"
  mut tokens := Vec::<Token>::new()
  tokens.push(Token { kind: 1, span: Span { start: 0, end: 3 } })
  tokens.push(Token { kind: 2, span: Span { start: 4, end: 5 } })
  if tokens.len() != 2 {
    ret 1
  }
  t := tokens.at(0).or_panic_with_message("expected a value")
  if t.kind != 1 {
    ret 2
  }
  if t.span.end - t.span.start != 3 {
    ret 3
  }
  if slice_of(src, t.span) != "let" {
    ret 4
  }
  if slice_of(src, tokens.at(1).or_panic_with_message("expected a value").span) != "x" {
    ret 5
  }
  ret 0
}
