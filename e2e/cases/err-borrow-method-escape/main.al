struct Holder {
  p: &i32,
}

impl Holder {
  fn get(self: &Holder) -> &i32 {
    ret self.p
  }
}

fn main() -> i32 {
  mut n := 5
  mut r := &n
  {
    mut m := 9
    holder := Holder { p: &m }
    r = holder.get()
  }
  ret *r
}
