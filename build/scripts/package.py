#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
import hashlib
import shutil
import sys
import tarfile
import zipfile
from pathlib import Path
import zstandard as zstd

REPO_ROOT = Path(__file__).resolve().parents[2]

VALID_PLATS = {"linux", "macos", "windows", "wasm"}
VALID_MODES = {"debug", "release"}
VALID_ROLES = {"ci", "release"}

METADATA_FILES: tuple[str, ...] = ("LICENSE", "README.md")

WASM_BASE_TARGETS: tuple[str, ...] = ("alcy.js", "alcy.wasm", "libalcy.a")
WASM_TEST_TARGETS: tuple[str, ...] = ("tests.js", "tests.wasm")

NATIVE_BASE_TARGETS: tuple[str, ...] = ("alcy",)
NATIVE_TEST_TARGETS: tuple[str, ...] = ("tests", "benchmarks")
NATIVE_STATIC_LIBS: tuple[str, ...] = ("libalcy.a", "alcy.lib")


def log_error(msg: str) -> None:
    print(f"::error::{msg}", file=sys.stderr)


def create_tar_zst(dist_dir: Path, archive_path: Path) -> None:
    cctx = zstd.ZstdCompressor(level=3, threads=-1)

    with open(archive_path, "wb") as f_out:
        with cctx.stream_writer(f_out) as compressor:
            # Mode "w|" opens an unseekable stream for writing tar
            with tarfile.open(fileobj=compressor, mode="w|") as tar:
                for file_path in sorted(dist_dir.rglob("*")):
                    arcname = file_path.relative_to(dist_dir)
                    tar.add(file_path, arcname=arcname, recursive=False)


def create_archive(dist_dir: Path, archive_path: Path, archive_format: str) -> None:
    if archive_format == "zip":
        with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for file_path in dist_dir.rglob("*"):
                if file_path.is_file():
                    zf.write(file_path, file_path.relative_to(dist_dir))
    elif archive_format == "tar.zst":
        create_tar_zst(dist_dir, archive_path)
    else:
        raise ValueError(f"Unsupported archive format: {archive_format}")


def generate_sha256(file_path: Path) -> Path:
    hasher = hashlib.sha256()
    with open(file_path, "rb") as f:
        while chunk := f.read(65536):
            hasher.update(chunk)

    checksum_path = file_path.with_suffix(file_path.suffix + ".sha256")
    checksum_path.write_text(f"{hasher.hexdigest()}  {file_path.name}\n")
    return checksum_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Package build artifacts.")
    parser.add_argument("--plat", required=True, choices=VALID_PLATS)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--mode", required=True, choices=VALID_MODES)
    parser.add_argument("--role", default="ci", choices=VALID_ROLES)
    parser.add_argument("--archive", default="tar.zst")
    parser.add_argument("--nix", action="store_true")
    parser.add_argument("--build-dir", type=Path, default=Path("out/build"))
    parser.add_argument("--dist-dir", type=Path, default=Path("dist"))
    args = parser.parse_args()

    # Resolve relative paths against repository root
    build_dir: Path = (
        args.build_dir
        if args.build_dir.is_absolute()
        else (REPO_ROOT / args.build_dir).resolve()
    )
    dist_dir: Path = (
        args.dist_dir
        if args.dist_dir.is_absolute()
        else (REPO_ROOT / args.dist_dir).resolve()
    )

    if not build_dir.is_dir():
        log_error(f"Build directory does not exist: {build_dir}")
        return 1

    # Clean staging directory
    if dist_dir.exists():
        shutil.rmtree(dist_dir)
    dist_dir.mkdir(parents=True, exist_ok=True)

    # Copy metadata files
    for doc in METADATA_FILES:
        doc_path = REPO_ROOT / doc
        if doc_path.is_file():
            shutil.copy2(doc_path, dist_dir)

    # Collect required target binaries
    if args.plat == "wasm":
        required_targets = list(WASM_BASE_TARGETS)
        if args.role != "release":
            required_targets.extend(WASM_TEST_TARGETS)
    else:
        exe = ".exe" if args.plat == "windows" else ""
        required_targets = [f"{t}{exe}" for t in NATIVE_BASE_TARGETS]
        if args.role != "release":
            required_targets.extend(f"{t}{exe}" for t in NATIVE_TEST_TARGETS)

    for target in required_targets:
        src = build_dir / target
        if not src.is_file():
            log_error(f"Expected output file missing: {src}")
            return 1
        shutil.copy2(src, dist_dir)

    # Copy optional native static libraries
    if args.plat != "wasm":
        for lib in NATIVE_STATIC_LIBS:
            lib_path = build_dir / lib
            if lib_path.is_file():
                shutil.copy2(lib_path, dist_dir)

    nix_suffix = "-nix" if args.nix else ""
    if args.plat == "wasm":
        archive_name = f"alcy-wasm-{args.arch}-{args.mode}.{args.archive}"
    else:
        archive_name = (
            f"alcy-{args.plat}-{args.arch}-{args.mode}{nix_suffix}.{args.archive}"
        )

    archive_path = REPO_ROOT / archive_name

    try:
        create_archive(dist_dir, archive_path, args.archive)
        generate_sha256(archive_path)
    except Exception as e:
        log_error(f"Packaging failed for {archive_name}: {e}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
