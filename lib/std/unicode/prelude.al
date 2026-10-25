// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `unicode` package.
//
// This file is the package's facade: the re-exports below are the
// package's implicit surface. The modules beside this one hold the
// definitions and are reachable by path.
//
// Text as Unicode over `core`'s byte view. Grapheme segmentation, case,
// and width arrive here as their callers do.

pub use package::utf8::valid;
