#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Builds and serves the playground site.

The site under `playground/` is plain HTML, CSS, and JavaScript, but it
is not self-contained: highlighting needs the grammar compiled to wasm
and a tree-sitter binding, and the Check and Run buttons need the
compiler's wasm module. This assembles all of those into one directory
that any static file server can host.

    uv run ./tools/playground.py build            # assemble playground/dist
    uv run ./tools/playground.py serve            # serve it on :8000

`build` takes whatever compiler artifacts it finds and warns when there
are none, so the site still works for highlighting alone. Pass
`--with-compiler` to build the `playground` target first; it carries the
direct wasm backend and no LLVM (see `compiler/playground/README.md`).
"""

import argparse
import gzip
import http.server
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tomllib
from functools import partial
from pathlib import Path

from utils.paths import project_root_dir

from build import build

PLAYGROUND_DIR = project_root_dir / "playground"
DEFAULT_DIST_DIR = PLAYGROUND_DIR / "dist"
GRAMMAR_DIR = project_root_dir / "treesitter"
DEFAULT_BUILD_SUBDIR = "playground"
DEFAULT_WASM_TARGET = "playground"
# The playground build carries no LLVM; `compiler/backends.gni` selects the
# backends, and an empty list is what a browser needs.
DEFAULT_GN_ARG = "alcy_backends=[]"
CACHE_DIR = project_root_dir / "out" / "playground-cache"

STATIC_FILES = [
    "app.js",
    "compiler.worker.js",
    "highlight.js",
    "i18n.js",
    "index.html",
    "runner.worker.js",
    "style.css",
    "textutil.js",
    "wasm-api.js",
    "wasi.js",
]


def log(message: str) -> None:
    print(f"playground: {message}")


def fail(message: str) -> int:
    print(f"playground: error: {message}", file=sys.stderr)
    return 1


def web_tree_sitter_version() -> str:
    with open(project_root_dir / "config.toml", "rb") as handle:
        return tomllib.load(handle)["web_tree_sitter_version"]


def copy_static(dist_dir: Path) -> None:
    for name in STATIC_FILES:
        source = PLAYGROUND_DIR / name
        if not source.is_file():
            raise FileNotFoundError(f"{source} is missing")
        shutil.copy2(source, dist_dir / name)


def copy_samples(dist_dir: Path) -> None:
    samples_dir = PLAYGROUND_DIR / "samples"
    manifest_path = samples_dir / "samples.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"{manifest_path} is missing")
    with open(manifest_path, "rb") as handle:
        manifest = json.loads(handle.read().decode("utf-8"))
    samples = manifest["samples"]
    target = dist_dir / "samples"
    target.mkdir(parents=True, exist_ok=True)
    # The manifest the page reads carries no repository paths; a sample
    # sourced from the repository's own `samples/` suite is copied under
    # its site name like the local ones.
    site_manifest = {
        "default": manifest.get("default"),
        "samples": [
            {key: value for key, value in sample.items() if key != "source"}
            for sample in samples
        ],
    }
    (target / "index.json").write_text(
        json.dumps(site_manifest, indent=2) + "\n", encoding="utf-8"
    )
    for sample in samples:
        source = (
            project_root_dir / sample["source"]
            if "source" in sample
            else samples_dir / sample["file"]
        )
        if not source.is_file():
            raise FileNotFoundError(f"{source} is missing")
        shutil.copy2(source, target / sample["file"])


def build_grammar(dist_dir: Path) -> None:
    grammar_out = dist_dir / "grammar"
    grammar_out.mkdir(parents=True, exist_ok=True)
    highlights = GRAMMAR_DIR / "queries" / "alcy" / "highlights.scm"
    shutil.copy2(highlights, grammar_out / "highlights.scm")
    tree_sitter = shutil.which("tree-sitter")
    if tree_sitter is None:
        raise RuntimeError(
            "tree-sitter is not on PATH; install it (see CONTRIBUTING.md) "
            "or pass --skip-grammar"
        )
    output = grammar_out / "tree-sitter-alcy.wasm"
    subprocess.run(
        [tree_sitter, "build", "--wasm", "-o", str(output), str(GRAMMAR_DIR)],
        check=True,
        cwd=project_root_dir,
    )
    log(f"grammar: {output}")


def fetch_web_tree_sitter(dist_dir: Path, version: str) -> None:
    vendor_out = dist_dir / "vendor"
    vendor_out.mkdir(parents=True, exist_ok=True)
    cache = CACHE_DIR / f"web-tree-sitter-{version}"
    binding = cache / "package" / "web-tree-sitter.js"
    runtime = cache / "package" / "web-tree-sitter.wasm"
    if not binding.is_file() or not runtime.is_file():
        npm = shutil.which("npm")
        if npm is None:
            raise RuntimeError("npm is not on PATH; needed to fetch web-tree-sitter")
        CACHE_DIR.mkdir(parents=True, exist_ok=True)
        log(f"fetching web-tree-sitter@{version}")
        subprocess.run(
            [
                npm,
                "pack",
                f"web-tree-sitter@{version}",
                "--pack-destination",
                str(CACHE_DIR),
            ],
            check=True,
            cwd=project_root_dir,
            capture_output=True,
            text=True,
        )
        tarball = CACHE_DIR / f"web-tree-sitter-{version}.tgz"
        if not tarball.is_file():
            raise FileNotFoundError(f"npm did not produce {tarball}")
        if cache.exists():
            shutil.rmtree(cache)
        cache.mkdir(parents=True)
        with tarfile.open(tarball, "r:gz") as archive:
            archive.extractall(cache, filter="data")
    shutil.copy2(binding, vendor_out / "web-tree-sitter.js")
    shutil.copy2(runtime, vendor_out / "web-tree-sitter.wasm")
    log(f"vendor: web-tree-sitter {version}")


def find_compiler_dir(build_dir: Path) -> Path | None:
    if (build_dir / "alcy_playground.js").is_file():
        return build_dir
    matches = sorted(build_dir.rglob("alcy_playground.js"))
    return matches[0].parent if matches else None


def copy_compiler(dist_dir: Path, build_dir: Path, compiler_dir: Path | None) -> bool:
    source = compiler_dir if compiler_dir is not None else find_compiler_dir(build_dir)
    if source is None:
        log(
            f"compiler: no alcy_playground.js under {build_dir}; "
            "Check and Run will be unavailable (build it with --with-compiler)"
        )
        return False
    glue = source / "alcy_playground.js"
    binary = source / "alcy_playground.wasm"
    if not glue.is_file() or not binary.is_file():
        raise FileNotFoundError(
            "both alcy_playground.js and alcy_playground.wasm are needed; "
            f"{source} has one"
        )
    target = dist_dir / "compiler"
    target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(glue, target / "alcy_playground.js")
    shutil.copy2(binary, target / "alcy_playground.wasm")
    log(f"compiler: {glue} ({binary.stat().st_size} bytes of wasm)")
    return True


def command_build(args: argparse.Namespace) -> int:
    if args.with_compiler:
        code = build(
            target=args.with_compiler,
            mode=args.mode,
            build_subdir=args.build_subdir,
            target_os="emscripten",
            gn_args_extra=DEFAULT_GN_ARG if args.gn_arg is None else args.gn_arg,
        )
        if code != 0:
            return code

    dist_dir = args.dist.resolve()
    if dist_dir.exists():
        shutil.rmtree(dist_dir)
    dist_dir.mkdir(parents=True)

    copy_static(dist_dir)
    copy_samples(dist_dir)

    if not args.skip_grammar:
        build_grammar(dist_dir)
    fetch_web_tree_sitter(dist_dir, args.web_tree_sitter_version)
    copy_compiler(
        dist_dir, project_root_dir / "out" / args.build_subdir, args.compiler_dir
    )

    log(f"site ready: {dist_dir}")
    log(f"serve it with: uv run ./tools/playground.py serve --dist {dist_dir}")
    return 0


def command_serve(args: argparse.Namespace) -> int:
    dist_dir = args.dist.resolve()
    if not (dist_dir / "index.html").is_file():
        return fail(f"{dist_dir} has no index.html; run `playground.py build` first")

    # GitHub Pages compresses the site's text assets; the local server does
    # the same so what is measured here matches what is deployed. The
    # grammar wasm compresses to about an eighth of its size.
    compressible = {
        ".al",
        ".css",
        ".html",
        ".js",
        ".json",
        ".mjs",
        ".scm",
        ".svg",
        ".txt",
        ".wasm",
    }

    class Handler(http.server.SimpleHTTPRequestHandler):
        extensions_map = {
            **http.server.SimpleHTTPRequestHandler.extensions_map,
            ".al": "text/plain; charset=utf-8",
            ".mjs": "text/javascript; charset=utf-8",
            ".scm": "text/plain; charset=utf-8",
            ".wasm": "application/wasm",
        }

        def end_headers(self) -> None:
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def send_head(self):
            path = self.translate_path(self.path)
            if (
                not os.path.isdir(path)
                and os.path.splitext(path)[1] in compressible
                and "gzip" in self.headers.get("Accept-Encoding", "")
            ):
                try:
                    with open(path, "rb") as handle:
                        body = handle.read()
                except OSError:
                    self.send_error(404, "File not found")
                    return None
                compressed = gzip.compress(body, 6)
                self.send_response(200)
                self.send_header("Content-Type", self.guess_type(path))
                self.send_header("Content-Encoding", "gzip")
                self.send_header("Vary", "Accept-Encoding")
                self.send_header("Content-Length", str(len(compressed)))
                self.end_headers()
                return io.BytesIO(compressed)
            return super().send_head()

    handler = partial(Handler, directory=str(dist_dir))
    server = http.server.ThreadingHTTPServer((args.host, args.port), handler)
    log(f"serving {dist_dir} at http://{args.host}:{server.server_port}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Build and serve the playground site.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    build_parser = subparsers.add_parser("build", help="assemble playground/dist")
    build_parser.add_argument("--mode", choices=["debug", "release"], default="release")
    build_parser.add_argument("--build-subdir", default=DEFAULT_BUILD_SUBDIR)
    build_parser.add_argument("--dist", type=Path, default=DEFAULT_DIST_DIR)
    build_parser.add_argument("--compiler-dir", type=Path, default=None)
    build_parser.add_argument(
        "--with-compiler",
        nargs="?",
        const=DEFAULT_WASM_TARGET,
        default=None,
        metavar="TARGET",
        help=f"Build the compiler wasm first (default target: {DEFAULT_WASM_TARGET})",
    )
    build_parser.add_argument(
        "--gn-arg",
        default=None,
        help="GN arguments for --with-compiler, passed through verbatim "
        f'(default: "{DEFAULT_GN_ARG}")',
    )
    build_parser.add_argument("--skip-grammar", action="store_true")
    build_parser.add_argument("--web-tree-sitter-version", default=None)
    build_parser.set_defaults(handler=command_build)

    serve_parser = subparsers.add_parser("serve", help="serve the assembled site")
    serve_parser.add_argument("--dist", type=Path, default=DEFAULT_DIST_DIR)
    serve_parser.add_argument("--host", default="0.0.0.0")
    serve_parser.add_argument("--port", type=int, default=8000)
    serve_parser.set_defaults(handler=command_serve)

    args = parser.parse_args()
    if (
        getattr(args, "web_tree_sitter_version", None) is None
        and args.command == "build"
    ):
        args.web_tree_sitter_version = web_tree_sitter_version()
    try:
        return args.handler(args)
    except (FileNotFoundError, RuntimeError, subprocess.CalledProcessError) as error:
        return fail(str(error))


if __name__ == "__main__":
    sys.exit(main())
