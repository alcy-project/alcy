// An element a type parameter accepts is kept in storage the callee
// owns. A view taken of an owning temporary names a slot in this frame,
// so storing it leaves the element reading a buffer that has ended.

struct View {
  bytes: &mut [u8],
}

fn payload() -> String {
  ret String::with_capacity(4usize)
}

fn main() -> i32 {
  mut views := Vec::<View>::new()
  views.push(View { bytes: payload().as_mut_slice() })
  ret 0
}
