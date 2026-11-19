// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// alcy/std/path: one public name, the platform's own spelling.

#[when(target.os != "windows")]
pub use package::unix::Path;

#[when(target.os == "windows")]
pub use package::windows::Path;
