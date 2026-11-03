// The buffer is declared before the container that keeps a view of it,
// so the block's reverse-declaration order ends the container first.

struct View {
  bytes: &mut [u8],
}

fn main() -> i32 {
  mut buf := Vec::<u8>::new()
  buf.push(7u8)
  mut views := Vec::<View>::new()
  views.push(View { bytes: buf.as_mut_slice() })
  println("{}", views[0].bytes[0])
  ret 0
}
