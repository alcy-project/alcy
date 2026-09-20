#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

import argparse
import sys
import os
import platform

import download_llvm

script_dir = os.path.dirname(__file__)
scripts_root = os.path.dirname(script_dir)
sys.path.append(scripts_root)
from utils.config import get_llvm_fork_tag

root_dir = os.path.dirname(scripts_root)
default_llvm_out_dir = os.path.join(root_dir, "out", "third_party", "llvm")
default_llvm_install_dir = os.path.join(default_llvm_out_dir, "install")
default_llvm_download_dir = os.path.join(default_llvm_out_dir, "download")

default_tag_cache_file = os.path.join(script_dir, ".llvm_tag_cache")


def host_triple():
    # Detect Architecture
    arch = platform.machine().lower()
    # Normalize arch names to match LLVM conventions
    arch_map = {
        "amd64": "x86_64",
        "x86_64": "x86_64",
        "arm64": "aarch64",
        "aarch64": "aarch64",
    }
    arch = arch_map.get(arch, arch)

    # Detect OS and Vendor/ABI
    system = platform.system().lower()

    if system == "darwin":
        return f"{arch}-apple-darwin"

    elif system == "windows":
        # We assume MSVC for Python-based builds on Windows
        return f"{arch}-pc-windows-msvc"

    elif system == "linux":
        # Check for GNU vs MUSL
        # Using 'ldd --version' to check the C library
        try:
            import subprocess

            ldd_out = subprocess.check_output(
                ["ldd", "--version"], stderr=subprocess.STDOUT
            ).decode()
            if "musl" in ldd_out.lower():
                return f"{arch}-unknown-linux-musl"
        except Exception:
            pass  # Fallback to gnu
        return f"{arch}-unknown-linux-gnu"

    return f"{arch}-unknown-unknown"


def write_tag_cache(tag, cache_file):
    try:
        with open(cache_file, "w") as f:
            f.write(tag.strip())
        print(f"Successfully cached tag '{tag}' to {cache_file}")
    except Exception as e:
        print(f"Warning: Failed to update tag cache file: {e}")


def main():
    parser = argparse.ArgumentParser(description="Setups LLVM.")

    parser.add_argument(
        "--install-dir",
        default=default_llvm_install_dir,
        help="LLVM installation directory",
    )
    parser.add_argument(
        "--tag-cache-file",
        default=default_tag_cache_file,
        help="Path to the tag cache file",
    )

    # Build Options
    parser.add_argument(
        "--type",
        default="Release",
        choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
        help="Build type of the prebuilt binary",
    )
    parser.add_argument(
        "--triple",
        default=host_triple(),
        help="Target triple (e.g., x86_64-unknown-linux-gnu)",
    )
    parser.add_argument(
        "--tag",
        default="",
        help="Release tag to download from alcy fork (defaults to llvm_fork_tag in config.toml)",
    )

    parser.add_argument(
        "--download-dir",
        default=default_llvm_download_dir,
        help="Directory to store downloaded archives",
    )

    parser.add_argument(
        "--disable-cache-llvm",
        action="store_true",
        help="Force re-download even if matching tag is installed",
    )

    args = parser.parse_args()

    if not args.tag:
        args.tag = get_llvm_fork_tag()
        if not args.tag:
            print("Error: Could not determine LLVM fork tag from config.toml")
            return -1

    include_dir = os.path.join(args.install_dir, "include")
    lib_dir = os.path.join(args.install_dir, "lib")
    enable_cache_llvm = not args.disable_cache_llvm
    lower_type = args.type.lower()

    # Check tag cache file
    cached_tag = None
    if os.path.isfile(args.tag_cache_file):
        try:
            with open(args.tag_cache_file, "r") as f:
                cached_tag = f.read().strip()
        except Exception:
            cached_tag = None

    tag_matches = cached_tag == args.tag
    has_include = os.path.isdir(include_dir)
    has_lib = os.path.isdir(lib_dir)

    if enable_cache_llvm and tag_matches and has_include and has_lib:
        print(
            f"Preinstalled LLVM matching tag '{args.tag}' found in '{args.install_dir}'. Skipping download."
        )
        return 0

    url = download_llvm.release_url(
        tag=args.tag, triple=args.triple, build_type=lower_type
    )
    print(f"Target release URL: {url}")

    if download_llvm.check_release_exists(url):
        print(f"Downloading prebuilt LLVM for tag '{args.tag}'...")
        res = download_llvm.download_and_extract(
            tag=args.tag,
            triple=args.triple,
            build_type=lower_type,
            download_dir=args.download_dir,
            install_dir=args.install_dir,
        )
        if res == 0:
            write_tag_cache(args.tag, args.tag_cache_file)
            return 0
        else:
            print(f"Error: Download/extraction failed with return code {res}.")
            return res
    else:
        print(f"Error: Release asset does not exist at {url}")
        return -2


if __name__ == "__main__":
    sys.exit(main())
