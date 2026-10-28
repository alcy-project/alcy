// A \`for\` body's fixed-size slots belong to the frame too: a body that
// holds a large buffer reuses one slot per turn rather than taking a
// fresh one, so the loop keeps the stack it started with. The counts are
// sized past a default stack if every turn took its own slot.
fn spin(count: i32) -> i32 {
  mut total := 0
  for i in 0..<count {
    mut buf := [0u8; 4096]
    buf[0] = i as u8
    total = total ^ (buf[0] as i32)
  }
  ret total
}

fn stride(count: i32) -> i32 {
  mut total := 0
  for i in (0..<count).step_by(2) {
    mut buf := [0u8; 4096]
    buf[0] = i as u8
    total = total ^ (buf[0] as i32)
  }
  ret total
}

fn main() -> i32 {
  if spin(8192) < 0 {
    ret 1
  }
  if stride(16384) < 0 {
    ret 2
  }
  ret 0
}
