struct Holder {
  p: &i32,
}

fn main() -> i32 {
  mut m := 9
  holder := Holder { p: &m }
  ret *holder.p
}
