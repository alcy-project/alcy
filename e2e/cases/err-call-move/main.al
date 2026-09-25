struct H { r: &mut i32 }

fn consume(h: H) -> i32 {
  ret *h.r
}

fn main() {
  mut x := 1
  h := H { r: &mut x }
  n := consume(h)
  _ := n
  _ := h.r
}
