struct H { r: &mut i32 }

fn main() {
  x := 1
  h := H { r: &mut x }
  y := h
  _ := y
  _ := h
}
