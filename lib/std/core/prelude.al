// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `core` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// Core is the one package of the suite with no dependencies; see
// docs/adr/0016-suites-and-the-std-split.md.

pub use package::anchor::Anchor;
pub use package::callable::Fn;
pub use package::cmp::Eq;
pub use package::cmp::Ordering;
pub use package::cmp::Ord;
pub use package::cmp::PartialEq;
pub use package::cmp::PartialOrd;
pub use package::index::Index;
pub use package::index::IndexMut;
pub use package::iterator::EnumerateIter;
pub use package::iterator::FilterIter;
pub use package::iterator::Iterator;
pub use package::iterator::MapIter;
pub use package::iterator::SkipIter;
pub use package::iterator::TakeIter;
pub use package::iterator::all;
pub use package::iterator::any;
pub use package::iterator::enumerate;
pub use package::iterator::filter;
pub use package::iterator::skip;
pub use package::iterator::take;
pub use package::iterator::count;
pub use package::iterator::find;
pub use package::iterator::fold;
pub use package::iterator::last;
pub use package::iterator::position;
pub use package::mem::memcopy;
pub use package::mem::ptr_offset;
pub use package::mem::ptr_offset_mut;
pub use package::mem::panic;
pub use package::mem::size_of;
pub use package::mem::align_of;
pub use package::mem::drop_in_place;
pub use package::mem::read;
pub use package::mem::wrapping_add;
pub use package::mem::wrapping_sub;
pub use package::mem::wrapping_mul;
pub use package::hash::xxh3_64;
pub use package::mem::file_size;
pub use package::mem::read_file_into;
pub use package::mem::write_file_all;
pub use package::mem::arg_count;
pub use package::mem::arg;
pub use package::ops::Add;
pub use package::ops::Div;
pub use package::ops::Mul;
pub use package::ops::Neg;
pub use package::ops::Rem;
pub use package::ops::Sub;
pub use package::option::Option;
pub use package::range::Bound;
pub use package::range::Range;
pub use package::range::RangeIter;
pub use package::result::Result;
pub use package::slice::slice_len;
pub use package::slice::slice_from_parts;
pub use package::slice::slice_from_parts_mut;
pub use package::slice::slice_at;
pub use package::slice::slice_at_mut;
pub use package::slice::ArrayIntoIter;
pub use package::slice::SliceIterMut;
pub use package::string::str_len;
pub use package::string::str_byte;
pub use package::string::str_slice;
pub use package::string::str_from_parts;
pub use package::string::LinesIter;
pub use package::string::SplitWhitespace;
pub use package::string::SplitIter;
pub use package::string::StrIter;
pub use package::slice::SliceIter;
