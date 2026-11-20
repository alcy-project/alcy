// A comp block may call a method of a generic impl whose receiver is a
// compound type. The callee body belongs to its own instantiation, so
// the comp rules of the block that reaches it do not judge its `ret`,
// and its value parameters resolve there.

fn main() -> i32 {
  const a := [10, 20, 30]
  const fresh := comp { [10, 20, 30].len() }
  const bound := comp { a.len() }
  ret (fresh + bound) as i32 - 6
}
