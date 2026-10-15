// A diagnostic list rendered in source order, the reporting step of a
// front end.
struct Span {
  start: usize,
  end: usize,
}

struct Diagnostic {
  span: Span,
  message: String,
}

fn diag(start: usize, message: str) -> Diagnostic {
  ret Diagnostic { span: Span { start: start, end: start + 1 }, message: String::from(message) }
}

fn render(msgs: Vec<Diagnostic>) -> String {
  mut ordered := msgs
  ordered.sort_by((a: &Diagnostic, b: &Diagnostic) -> a.span.start < b.span.start)
  mut out := String::new()
  mut i := 0 as usize
  while i < ordered.len() {
    d := ordered.at(i).unwrap()
    if i > 0 {
      out.push(0x0au8)
    }
    out.push_str(d.message.as_str())
    i = i + 1
  }
  ret out
}

fn main() -> i32 {
  mut msgs := Vec::<Diagnostic>::new()
  msgs.push(diag(5, "second"))
  msgs.push(diag(1, "first"))
  text := render(msgs)
  if text.as_str() != "first\nsecond" { ret 1 }
  ret 0
}
