mod util;

fn main() -> i32 {
  v := util::double(21)
  if v == 42 {
    print("pkg-ok")
    ret 0
  }
  ret 1
}
