fn main() -> i32 {
  v := util::double(21)
  if v == 42 {
    println("pkg-ok")
    ret 0
  }
  ret 1
}
