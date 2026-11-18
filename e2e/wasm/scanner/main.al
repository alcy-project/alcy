// The input layer over WASI preview1: stdin is at end of input
// in the runner, so a typed read reports the end.

fn main() -> i32 {
  mut sc := Scanner::stdin()
  match sc.next::<usize>() {
    Result::Ok(_) => {
      ret 1
    }
    Result::Err(ScanError::Parse(_)) => {
      ret 2
    }
    Result::Err(ScanError::End) => {
      print("end\n")
      ret 0
    }
    Result::Err(ScanError::Io(_)) => {
      print("io\n")
      ret 0
    }
  }
}
