// The environment owns a moved capture, and the closure may run again:
// moving it out is moving out of borrowed content.
struct R { name: String, x: i32 }

fn take(r: R) -> i32 {
  ret r.x
}

fn main() {
  mut t := String::new()
  t.push_str("hi")
  r := R { name: t, x: 1 }
  f := [r] () -> {
    ret take(r)
  }
  _ := f()
}
