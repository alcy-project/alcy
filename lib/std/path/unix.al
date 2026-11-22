// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Paths over bytes, the platform's own separator. A path holds valid
// UTF-8: text becomes its bytes directly, and `from_bytes` checks bytes
// that come from elsewhere. A path is what the operating system was
// handed, but only within the UTF-8 names this type can represent.

use core::string::is_utf8;

#[when(target.os != "windows")]
pub struct Path {
  bytes: Vec<u8>,
}

#[when(target.os != "windows")]
impl Path {
  // The path a string names, its bytes as they are.
  pub fn from_str(s: str) -> Path {
    ret Path { bytes: Vec::<u8>::from_slice(String::from(s).as_bytes()) }
  }

  // The path the bytes name, or `None` when they are not UTF-8. The
  // check is what `to_str` undoes, so a path always round-trips.
  pub fn from_bytes(bytes: &[u8]) -> Option<Path> {
    if !is_utf8(bytes) {
      ret Option::None
    }
    ret Option::Some(Path { bytes: Vec::<u8>::from_slice(bytes) })
  }

  // The bytes the path is made of.
  pub fn len_bytes(self: &Self) -> usize {
    ret self.bytes.len()
  }

  // The bytes the path is made of, borrowed.
  pub fn as_bytes(self: &Self) -> &[u8] {
    ret self.bytes.as_slice()
  }

  // A copy of the bytes, for the calls that take them by value.
  pub fn encoded_bytes(self: &Self) -> Vec<u8> {
    ret Vec::<u8>::from_slice(self.as_bytes())
  }

  // The path checked as text, or `None` when its bytes are not UTF-8.
  pub fn to_str(self: &Self) -> Option<String> {
    ret String::from_slice(self.as_bytes())
  }

  // Whether the path names a root: absolute paths start with the
  // separator.
  pub fn is_absolute(self: &Self) -> bool {
    ret !self.bytes.is_empty() && self.bytes[0] == 47u8
  }

  // The path with `other` joined onto the end. An absolute `other`
  // replaces the whole path; otherwise one separator is inserted
  // between two components that are not already separated.
  pub fn join(self: &Self, other: &Path) -> Path {
    if other.is_absolute() {
      ret other.dup()
    }
    mut out := self.dup()
    n := out.bytes.len()
    if n > 0 && out.bytes[n - 1] != 47u8 && !other.bytes.is_empty() && other.bytes[0] != 47u8 {
      out.bytes.push(47u8)
    }
    mut i := 0 as usize
    while i < other.bytes.len() {
      out.bytes.push(other.bytes[i])
      i = i + 1
    }
    ret out
  }

  // The last component, or `None` for a path of nothing but
  // separators.
  pub fn file_name(self: &Self) -> Option<String> {
    mut end := self.bytes.len()
    while end > 0 && self.bytes[end - 1] == 47u8 {
      end = end - 1
    }
    if end == 0 {
      ret Option::None
    }
    mut start := end
    while start > 0 && self.bytes[start - 1] != 47u8 {
      start = start - 1
    }
    ret String::from_slice(self.bytes.as_slice()[start..<end])
  }

  // The path above the last component, or `None` for the root. A
  // component-less relative path answers the empty path.
  pub fn parent(self: &Self) -> Option<Path> {
    mut end := self.bytes.len()
    while end > 0 && self.bytes[end - 1] == 47u8 {
      end = end - 1
    }
    if end == 0 {
      ret Option::None
    }
    mut start := end
    while start > 0 && self.bytes[start - 1] != 47u8 {
      start = start - 1
    }
    mut bytes := Vec::<u8>::from_slice(self.bytes.as_slice()[0..<start])
    mut keep := bytes.len()
    while keep > 1 && bytes[keep - 1] == 47u8 {
      keep = keep - 1
    }
    while bytes.len() > keep {
      _ := bytes.pop()
    }
    ret Option::Some(Path { bytes: bytes })
  }

  // The extension after the last dot in the last component, or
  // `None` when the component has none or opens with one.
  pub fn extension(self: &Self) -> Option<String> {
    mut end := self.bytes.len()
    while end > 0 && self.bytes[end - 1] == 47u8 {
      end = end - 1
    }
    if end == 0 {
      ret Option::None
    }
    mut start := end
    while start > 0 && self.bytes[start - 1] != 47u8 {
      start = start - 1
    }
    mut dot := 0 as usize
    mut found := false
    mut i := start
    while i < end {
      if self.bytes[i] == 46u8 {
        found = true
        dot = i
      }
      i = i + 1
    }
    if !found {
      ret Option::None
    }
    if dot == start {
      ret Option::None
    }
    ret String::from_slice(self.bytes.as_slice()[(dot + 1)..<end])
  }

  // Whether this path starts with the other, component by component.
  pub fn starts_with(self: &Self, base: &Path) -> bool {
    mine := self.segments()
    theirs := base.segments()
    if theirs.len() > mine.len() {
      ret false
    }
    mut i := 0 as usize
    while i < theirs.len() {
      a := mine.at(i).or_panic_with_message("the component is in the path")
      b := theirs.at(i).or_panic_with_message("the component is in the base")
      if !slice_eq(a.as_slice(), b.as_slice()) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // Whether this path ends with the other, component by component.
  pub fn ends_with(self: &Self, base: &Path) -> bool {
    mine := self.segments()
    theirs := base.segments()
    if theirs.len() > mine.len() {
      ret false
    }
    mut offset := mine.len() - theirs.len()
    mut i := 0 as usize
    while i < theirs.len() {
      a := mine.at(offset + i).or_panic_with_message("the component is in the path")
      b := theirs.at(i).or_panic_with_message("the component is in the base")
      if !slice_eq(a.as_slice(), b.as_slice()) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  fn dup(self: &Self) -> Path {
    ret Path { bytes: Vec::<u8>::from_slice(self.as_bytes()) }
  }

  // The components, separators removed.
  fn segments(self: &Self) -> Vec<Vec<u8>> {
    mut out := Vec::<Vec<u8>>::new()
    mut start := 0 as usize
    mut i := 0 as usize
    while i < self.bytes.len() {
      if self.bytes[i] == 47u8 {
        if i > start {
          out.push(Vec::<u8>::from_slice(self.bytes.as_slice()[start..<i]))
        }
        start = i + 1
      }
      i = i + 1
    }
    if start < self.bytes.len() {
      out.push(Vec::<u8>::from_slice(self.bytes.as_slice()[start..<self.bytes.len()]))
    }
    ret out
  }
}

#[when(target.os != "windows")]
fn slice_eq(a: &[u8], b: &[u8]) -> bool {
  if a.len() != b.len() {
    ret false
  }
  mut i := 0 as usize
  while i < a.len() {
    if a[i] != b[i] {
      ret false
    }
    i = i + 1
  }
  ret true
}

#[when(testing)] {
  #[when(target.os != "windows")]
  #[test] fn from_str_round_trips_through_to_str() {
    p := Path::from_str("/usr/lib")
    match p.to_str() {
      Option::Some(text) => {
        assert(text.as_str() == "/usr/lib", "the text comes back out")
      }
      Option::None => {
        panic("a UTF-8 path hands back its text")
      }
    }
  }

  #[when(target.os != "windows")]
  #[test] fn from_bytes_refuses_bytes_that_are_not_utf8() {
    bad := [0xffu8, 0xfeu8]
    assert(Path::from_bytes(bad.as_slice()).is_none(), "not scalar-shaped bytes are refused")
  }

  #[when(target.os != "windows")]
  #[test] fn relative_path_is_not_absolute() {
    assert(!Path::from_str("some/relative/name").is_absolute(), "no root, no absolute")
  }

  #[when(target.os != "windows")]
  #[test] fn root_path_is_absolute() {
    assert(Path::from_str("/").is_absolute(), "the separator itself is a root")
  }

  #[when(target.os != "windows")]
  #[test] fn join_puts_one_separator_between_two_components() {
    p := Path::from_str("/usr").join(&Path::from_str("lib"))
    match p.to_str() {
      Option::Some(text) => {
        assert(text.as_str() == "/usr/lib", "one separator between the components")
      }
      Option::None => {
        panic("the joined path is still UTF-8")
      }
    }
  }

  #[when(target.os != "windows")]
  #[test] fn join_prefers_an_absolute_argument() {
    p := Path::from_str("/usr").join(&Path::from_str("/etc"))
    match p.to_str() {
      Option::Some(text) => {
        assert(text.as_str() == "/etc", "an absolute component replaces the path")
      }
      Option::None => {
        panic("the argument is UTF-8")
      }
    }
  }

  #[when(target.os != "windows")]
  #[test] fn file_name_strips_trailing_separators() {
    p := Path::from_str("/usr/lib/")
    match p.file_name() {
      Option::Some(name) => {
        assert(name.as_str() == "lib", "the last component is the answer")
      }
      Option::None => {
        panic("the trailing separators fall off")
      }
    }
  }

  #[when(target.os != "windows")]
  #[test] fn file_name_of_a_root_is_none() {
    assert(Path::from_str("/").file_name().is_none(), "a root has no last component")
  }

  #[when(target.os != "windows")]
  #[test] fn parent_of_a_root_is_none() {
    assert(Path::from_str("/").parent().is_none(), "a root has no parent")
  }

  #[when(target.os != "windows")]
  #[test] fn parent_of_a_relative_component_is_the_empty_path() {
    match Path::from_str("one").parent() {
      Option::Some(p) => {
        assert(p.len_bytes() == 0, "the empty path is the parent")
      }
      Option::None => {
        panic("a relative component has the empty path above it")
      }
    }
  }

  #[when(target.os != "windows")]
  #[test] fn parent_climbs_one_component() {
    match Path::from_str("/usr/lib").parent() {
      Option::Some(p) => {
        match p.to_str() {
          Option::Some(text) => {
            assert(text.as_str() == "/usr", "one component fell off")
          }
          Option::None => {
            panic("the parent is UTF-8")
          }
        }
      }
      Option::None => {
        panic("a component path has a parent")
      }
    }
  }

  #[when(target.os != "windows")]
  #[test] fn extension_reads_the_suffix() {
    match Path::from_str("lib/libz.a").extension() {
      Option::Some(ext) => {
        assert(ext.as_str() == "a", "the suffix after the last dot")
      }
      Option::None => {
        panic("a dotted file has an extension")
      }
    }
  }

  #[when(target.os != "windows")]
  #[test] fn extension_of_a_dotfile_is_none() {
    assert(Path::from_str(".hidden").extension().is_none(), "a leading dot is not an extension")
  }

  #[when(target.os != "windows")]
  #[test] fn extension_of_a_name_without_a_dot_is_none() {
    assert(Path::from_str("Makefile").extension().is_none(), "no dot, no extension")
  }

  #[when(target.os != "windows")]
  #[test] fn starts_with_matches_components_not_bytes() {
    assert(
      Path::from_str("/usr/lib").starts_with(&Path::from_str("/usr")),
      "the prefix is a component",
    )
    assert(
      !Path::from_str("/usr/lib").starts_with(&Path::from_str("/us")),
      "a byte prefix is not a component",
    )
  }

  #[when(target.os != "windows")]
  #[test] fn ends_with_matches_components_not_bytes() {
    assert(
      Path::from_str("/usr/lib").ends_with(&Path::from_str("lib")),
      "the suffix is a component",
    )
    assert(
      !Path::from_str("/usr/lib").ends_with(&Path::from_str("ib")),
      "a byte suffix is not a component",
    )
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
