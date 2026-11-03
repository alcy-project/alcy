// A comp block may call a method of a generic impl whose receiver is a
// compound type; the callee body is checked as its own item, so the
// block's comp context does not judge its `ret`.

fn main() {
  const a := [10, 20, 30]
  _ := comp { [10, 20, 30].len() }
  _ := comp { a.len() }
}
