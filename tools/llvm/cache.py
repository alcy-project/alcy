#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""A shared, pinned LLVM install for local worktrees.

Each worktree used to unpack the same fork artifact into its own `out/`
tree, which cost hundreds of megabytes per clone. The key here is the
repo's pinned tag, the host triple, and the build type, so two branches
that pin different LLVM builds cannot share an install by accident.
"""

import os
import platform
import shutil
import sys
import tempfile
from pathlib import Path

TAG_CACHE_NAME = ".llvm_tag_cache"


def cache_root() -> Path:
    """The OS's cache location, overridable with XDG_CACHE_HOME."""
    xdg = os.environ.get("XDG_CACHE_HOME")
    if xdg:
        return Path(xdg) / "alcy" / "llvm"
    if os.name == "nt":
        local = os.environ.get("LOCALAPPDATA")
        if not local:
            local = str(Path.home() / "AppData" / "Local")
        return Path(local) / "alcy" / "llvm"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Caches" / "alcy" / "llvm"
    return Path.home() / ".cache" / "alcy" / "llvm"


def host_triple() -> str:
    """The triple the fork artifact is named after."""
    arch = platform.machine().lower()
    arch = {
        "amd64": "x86_64",
        "arm64": "aarch64",
    }.get(arch, arch)
    system = platform.system().lower()
    if system == "darwin":
        return f"{arch}-apple-darwin"
    if system == "windows":
        return f"{arch}-pc-windows-msvc"
    if system == "linux":
        return f"{arch}-unknown-linux-gnu"
    return f"{arch}-unknown-unknown"


def cache_path(tag: str, triple: str, build_type: str) -> Path:
    return cache_root() / tag / triple / build_type.lower()


def valid(path: Path, tag: str) -> bool:
    """Whether `path` is an install that matches this repo's pin."""
    marker = path / TAG_CACHE_NAME
    try:
        return (
            marker.is_file()
            and marker.read_text().strip() == tag
            and (path / "include").is_dir()
            and (path / "lib").is_dir()
        )
    except OSError:
        return False


def enabled() -> bool:
    """The shared cache is on unless the caller opts out."""
    return os.environ.get("ALCY_LLVM_CACHE", "1").lower() not in (
        "0",
        "false",
        "no",
    )


def populate(tag: str, triple: str, build_type: str) -> Path | None:
    """Downloads and unpacks the pinned artifact into the shared cache.

    Extraction goes to a temporary sibling and is renamed into place, so
    a worker that dies mid-download never leaves a partial install that
    another worktree would accept. On Windows `os.replace` cannot overwrite
    a non-empty directory; if another worker won the race, the existing
    directory is validated and used instead.
    """
    if not tag:
        return None
    target = cache_path(tag, triple, build_type)
    if valid(target, tag):
        return target

    try:
        from .download_llvm import download_and_extract
    except ImportError:
        try:
            from download_llvm import download_and_extract
        except ImportError:
            return None

    target.parent.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix=".llvm-", dir=target.parent))
    try:
        install = scratch / "install"
        result = download_and_extract(
            tag=tag,
            triple=triple,
            build_type=build_type.lower(),
            download_dir=str(scratch / "download"),
            install_dir=str(install),
        )
        if result != 0:
            return None
        (install / TAG_CACHE_NAME).write_text(tag.strip() + "\n")
        try:
            os.replace(install, target)
        except OSError:
            pass
        return target if valid(target, tag) else None
    except OSError, ValueError:
        return None
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def resolve(tag: str, triple: str, build_type: str) -> Path | None:
    """A valid shared install, populated on first use when possible."""
    if not tag or not enabled():
        return None
    path = cache_path(tag, triple, build_type)
    if valid(path, tag):
        return path
    return populate(tag, triple, build_type)
