#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import os
import urllib.request
import urllib.error
import tarfile
import zipfile
import zstandard


def archive_ext(triple):
    """Determine the archive extension based on the target triple."""
    if "windows" in triple:
        return "zip"
    return "tar.zst"
    # return "tar.gz"


def archive_file_name(build_type, triple):
    """Generate the filename for the archive."""
    ext = archive_ext(triple)
    return f"llvm-{build_type}-{triple}.{ext}"


def release_url(tag, triple, build_type):
    """Construct the GitHub release download URL."""
    filename = archive_file_name(build_type, triple)
    return f"https://github.com/alcy-project/llvm-alcy-fork/releases/download/{tag}/{filename}"


def check_release_exists(url):
    """Check if the release asset exists using an HTTP HEAD request."""
    try:
        req = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(req) as response:
            if response.status == 200:
                return True
    except urllib.error.URLError:
        return False


def extract_tar_zst(archive_path, install_dir):
    """Decompress and extract .tar.zst files."""
    with open(archive_path, "rb") as fh:
        dctx = zstandard.ZstdDecompressor()
        with dctx.stream_reader(fh) as reader:
            with tarfile.open(fileobj=reader, mode="r|*") as tar_ref:
                tar_ref.extractall(install_dir)
    return


def download_and_extract(tag, triple, build_type, download_dir, install_dir):
    """Download the LLVM archive and extract it to the specified directory."""
    archive_name = archive_file_name(build_type, triple)
    archive_path = os.path.join(download_dir, archive_name)

    # Ensure directories exist
    os.makedirs(download_dir, exist_ok=True)
    os.makedirs(install_dir, exist_ok=True)

    url = release_url(tag, triple, build_type)
    if not check_release_exists(url):
        return 1

    # Download
    print(f"Downloading {url} to {archive_path}...")
    try:
        urllib.request.urlretrieve(url, archive_path)
    except Exception as e:
        print(f"Failed to download: {e}")
        return 1

    # Extract
    print(f"Extracting {archive_path} to {install_dir}...")
    try:
        if archive_path.endswith(".zip"):
            with zipfile.ZipFile(archive_path, "r") as zip_ref:
                zip_ref.extractall(install_dir)
        elif archive_path.endswith(".tar.zst"):
            extract_tar_zst(archive_path, install_dir)
        elif archive_path.endswith(".tar.gz"):
            with tarfile.open(archive_path, "r:gz") as tar_ref:
                tar_ref.extractall(install_dir)
        print("Extraction completed successfully.")
        return 0
    except Exception as e:
        print(f"Failed to extract: {e}")
        return 2
