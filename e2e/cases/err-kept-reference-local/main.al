// The container outlives the buffer its element views: the block ends
// the buffer's slot while the element still reads it.

struct View {
  bytes: &mut [u8],
}

fn main() -> i32 {
  mut views := Vec::<View>::new()
  {
    mut buf := Vec::<u8>::new()
    buf.push(7u8)
    views.push(View { bytes: buf.as_mut_slice() })
  }
  println("{}", views[0].bytes[0])
  ret 0
}
