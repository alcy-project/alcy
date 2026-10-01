pub spec Show {
  fn show(self: &Self) -> str;
}

pub spec Loud {
  fn show(self: &Self) -> str;
}

pub struct Word {
  s: str,
}

impl Show for Word {
  fn show(self: &Self) -> str {
    ret "word"
  }
}

impl Loud for Word {
  fn show(self: &Self) -> str {
    ret "WORD"
  }
}
