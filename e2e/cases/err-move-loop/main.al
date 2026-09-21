struct H { r: &mut i32 }

fn main() {
  mut x := 0
  h := H { r: &mut x }
  mut i := 0
  while i < 3 {
    g := h
    _ := g
    i = i + 1
  }
}
