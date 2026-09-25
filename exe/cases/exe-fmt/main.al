fn main() -> i32 {
  mut buf := [0u8; 5]
  out := write("a={} b={}!", &mut buf, (41i32, true))
  mut bad := 0
  if out.written != 5 {
    bad = 1
  }
  if out.total != 12 {
    bad = 2
  }
  if buf[0] != 97u8 {
    bad = 3
  }
  if buf[1] != 61u8 {
    bad = 4
  }
  if buf[4] != 32u8 {
    bad = 5
  }
  ret bad
}
