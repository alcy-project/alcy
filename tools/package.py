#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

import zstandard as zstd

REPO_ROOT = Path(__file__).resolve().parents[1]

VALID_PLATS = {"linux", "macos", "windows", "wasm"}
VALID_MODES = {"debug", "release"}
VALID_ROLES = {"ci", "release"}

METADATA_FILES: tuple[str, ...] = ("LICENSE", "README.md")

WASM_BASE_TARGETS: tuple[str, ...] = ("alcy.js", "alcy.wasm", "libalcy.a")
WASM_TEST_TARGETS: tuple[str, ...] = ("tests.js", "tests.wasm")
# The playground's compiler module, built by the `playground` GN target
# with no LLVM; a site loads these two files beside its own.
WASM_PLAYGROUND_TARGETS: tuple[str, ...] = (
    "alcy_playground.js",
    "alcy_playground.wasm",
)

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


def extract_archive(archive_path: Path, dest_dir: Path) -> None:
    if archive_path.suffix == ".zip":
        with zipfile.ZipFile(archive_path, "r") as zf:
            zf.extractall(dest_dir)
    else:
        dctx = zstd.ZstdDecompressor()
        with open(archive_path, "rb") as f_in, dctx.stream_reader(f_in) as reader:
            with tarfile.open(fileobj=reader, mode="r|") as tar:
                tar.extractall(dest_dir, filter="data")


def required_targets(plat: str, role: str) -> list[str]:
    if plat == "wasm":
        targets = list(WASM_BASE_TARGETS) + list(WASM_PLAYGROUND_TARGETS)
        if role != "release":
            targets += WASM_TEST_TARGETS
        return targets
    exe = ".exe" if plat == "windows" else ""
    targets = [f"{t}{exe}" for t in NATIVE_BASE_TARGETS]
    if role != "release":
        targets += [f"{t}{exe}" for t in NATIVE_TEST_TARGETS]
    return targets


def archive_name(plat: str, arch: str, mode: str, archive: str, nix: bool) -> str:
    nix_suffix = "-nix" if nix else ""
    if plat == "wasm":
        return f"alcy-wasm-{arch}-{mode}.{archive}"
    return f"alcy-{plat}-{arch}-{mode}{nix_suffix}.{archive}"


def verify_archive(archive_path: Path, plat: str, required_targets: list[str]) -> bool:
    """Unpacks the archive and asks the binary it carries to run.

    The archive is the artifact a consumer downloads, so packaging that
    stops at writing the file proves the runner could write it. The wasm
    archive carries a module no host runs directly, so its check is the
    file list.
    """
    with tempfile.TemporaryDirectory(prefix="alcy_package_") as temp:
        dest_dir = Path(temp)
        try:
            extract_archive(archive_path, dest_dir)
        except Exception as e:
            log_error(f"Verify could not unpack {archive_path.name}: {e}")
            return False

        missing = [name for name in required_targets if not (dest_dir / name).is_file()]
        if missing:
            log_error(f"Archive is missing: {', '.join(missing)}")
            return False

        if plat == "wasm":
            return True

        # The extraction filter does not promise the mode bits, and a
        # binary the host cannot execute is a binary the smoke cannot ask.
        binary = dest_dir / required_targets[0]
        os.chmod(binary, 0o755)
        run = subprocess.run([str(binary), "--version"], capture_output=True, text=True)
        if run.returncode != 0 or not run.stdout.strip():
            log_error(f"The packaged binary failed to run: exit={run.returncode}")
            if run.stdout:
                log_error(f"stdout: {run.stdout.strip()}")
            if run.stderr:
                log_error(f"stderr: {run.stderr.strip()}")
            return False
        return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Package build artifacts.")
    parser.add_argument("--plat", required=True, choices=VALID_PLATS)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--mode", required=True, choices=VALID_MODES)
    parser.add_argument("--role", default="ci", choices=VALID_ROLES)
    parser.add_argument("--archive", default="tar.zst")
    parser.add_argument(
        "--verify",
        action="store_true",
        help="Unpack the archive, check its contents, and run the binary "
        "it carries; the wasm archive stops at the contents",
    )
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="verify the archive for these flags without packaging: the "
        "one the package step just wrote is what a consumer would hold",
    )
    parser.add_argument("--nix", action="store_true")
    parser.add_argument("--build-dir", type=Path, default=Path("out/build"))
    parser.add_argument("--dist-dir", type=Path, default=Path("dist"))
    args = parser.parse_args()

    if args.verify_only:
        verify_targets = required_targets(args.plat, args.role)
        archive_path = REPO_ROOT / archive_name(
            args.plat, args.arch, args.mode, args.archive, args.nix
        )
        if not archive_path.is_file():
            log_error(
                f"verify found no archive at {archive_path}; run package.py first"
            )
            return 1
        return 0 if verify_archive(archive_path, args.plat, verify_targets) else 1

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
    targets = required_targets(args.plat, args.role)

    for target in targets:
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

    archive_path = REPO_ROOT / archive_name(
        args.plat, args.arch, args.mode, args.archive, args.nix
    )

    try:
        create_archive(dist_dir, archive_path, args.archive)
        generate_sha256(archive_path)
    except Exception as e:
        log_error(f"Packaging failed for {archive_path.name}: {e}")
        return 1

    if args.verify and not verify_archive(archive_path, args.plat, targets):
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
