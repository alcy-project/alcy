// A stack machine: op 1 pushes the next word, op 2 adds the top two.
fn run(code: &[i32]) -> i64 {
  mut stack := Vec::<i64>::new()
  mut i := 0 as usize
  while i < code.len() {
    op := *code.at(i).unwrap()
    if op == 1 {
      i = i + 1
      stack.push(*code.at(i).unwrap() as i64)
      i = i + 1
      continue
    }
    if op == 2 {
      b := stack.pop().unwrap()
      a := stack.pop().unwrap()
      stack.push(a + b)
      i = i + 1
      continue
    }
    i = i + 1
  }
  ret *stack.at(0).unwrap()
}

fn main() -> i32 {
  code := [1, 2, 1, 3, 2]
  if run(code.as_slice()) != 5 {
    ret 1
  }
  ret 0
}
