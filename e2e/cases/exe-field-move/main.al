// A field handed to a new owner moves out of its owner: the drop walk
// sees the partial move, so the buffer is freed once. The exit codes
// check the values, and the sanitized run is what checks the frees.

struct P {
  name: String,
  tag: i32,
}

fn take(p: P) -> String {
  ret p.name
}

fn main() -> i32 {
  mut a := String::new()
  a.push_str("hi")
  n := take(P { name: a, tag: 1 })
  if n.len_bytes() != 2 {
    ret 1
  }
  println("{}", n.len_bytes())

  mut v := Vec::<String>::new()
  mut b := String::new()
  b.push_str("ok")
  q := P { name: b, tag: 2 }
  v.push(q.name)
  if v.len() != 1 {
    ret 2
  }
  if v[0].len_bytes() != 2 {
    ret 3
  }
  ret 0
}
