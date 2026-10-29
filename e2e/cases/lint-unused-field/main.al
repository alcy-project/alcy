struct P { used: i32, unused: i32 }

fn main() -> i32 {
  p := P { used: 1, unused: 2 }
  ret p.used
}
