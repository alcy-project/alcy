enum E { A(&mut i32), B }

fn main() {
  mut x := 1
  v := E::A(&mut x)
  r := match v {
    E::A(y) => 1,
    E::B => 0,
  }
  _ := v
  _ := r
}
