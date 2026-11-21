// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `hash` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// Hash values and the algorithms that compute them. `str` implements
// the spec through xxh3; integer keys mix through `mix64` and get a
// cheap, well-spread digest.

pub use package::hash::Hash;
pub use package::mix::mix2;
pub use package::mix::mix64;
pub use package::xxh3::xxh3_64;
