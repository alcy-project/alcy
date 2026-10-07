#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Builds and serves the site.

The site under `site/` holds the landing page and the playground page as
templates plus the shared shell: TypeScript and plain HTML and CSS that
is not self-contained. The sources compile to the JavaScript the pages
load, highlighting needs the grammar compiled to wasm and a tree-sitter
binding, and the Check and Run buttons need the compiler's wasm module.
The site's own generator (`site/ssg/`) stamps one page tree per language
(English at the root, `/ja/` beside it) from the templates and the guide
under `docs/guide/`, checking the guide's examples with the same compiler
module. This assembles all of those into `site/dist/`, which any static
file server can host.

    uv run ./tools/site.py build            # assemble site/dist
    uv run ./tools/site.py test             # run the generator's tests
    uv run ./tools/site.py serve            # serve it on :8000

`build` takes whatever compiler artifacts it finds and warns when there
are none, so the site still works for highlighting alone. Pass
`--with-compiler` to build the `playground` target first; it carries the
direct wasm backend and no LLVM (see `compiler/playground/README.md`).

The TypeScript compiler and the tree-sitter web binding are installed
with pnpm into `out/site-cache/`, at the versions `config.toml` pins;
`tree-sitter` itself must be on `PATH`.
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
import tomllib
from dataclasses import dataclass
from functools import partial
from pathlib import Path

from utils.paths import project_root_dir

from build import build

SITE_DIR = project_root_dir / "site"
PLAYGROUND_DIR = SITE_DIR / "playground"
SHARED_DIR = SITE_DIR / "shared"
DEFAULT_DIST_DIR = SITE_DIR / "dist"
# The user-facing guide; the site's generator renders it.
GUIDE_DIR = project_root_dir / "docs" / "guide"
# Where the playground page lives inside the assembled site.
PAGE_SUBDIR = "playground"
# The JavaScript the site loads, as emitted by `tsc`.
BUILD_DIR = SITE_DIR / "build"
GRAMMAR_DIR = project_root_dir / "treesitter"
DEFAULT_BUILD_SUBDIR = "playground"
DEFAULT_WASM_TARGET = "playground"
# The playground build carries no LLVM; `compiler/backends.gni` selects the
# backends, and an empty list is what a browser needs.
DEFAULT_GN_ARG = "alcy_backends=[]"
CACHE_DIR = project_root_dir / "out" / "site-cache"
DEPS_DIR = CACHE_DIR / "deps"
STORE_DIR = CACHE_DIR / "pnpm-store"

# The page's modules, compiled from `site/playground/*.ts` into `build/`.
# `types` carries only types and emits nothing the page loads, so it is
# not here.
TYPESCRIPT_MODULES = [
    "app",
    "assets",
    "compiler.worker",
    "editor",
    "elements",
    "highlight",
    "output",
    "problems",
    "runner.worker",
    "runtime",
    "samples",
    "session",
    "splitter",
    "state",
    "status",
    "tabs",
    "textutil",
    "wasm-api",
    "wasi",
]

PLAIN_FILES = ["style.css"]

# The shared shell: the plain files are copied as they are, and everything
# under `build/shared/` was compiled from `site/shared/*.ts`.
SHARED_PLAIN_FILES = ["boot.js", "site.css"]


@dataclass(frozen=True)
class Versions:
    pnpm: str
    typescript: str
    web_tree_sitter: str


def log(message: str) -> None:
    print(f"site: {message}")


def fail(message: str) -> int:
    print(f"site: error: {message}", file=sys.stderr)
    return 1


def load_versions() -> Versions:
    with open(project_root_dir / "config.toml", "rb") as handle:
        config = tomllib.load(handle)
    return Versions(
        pnpm=config["pnpm_version"],
        typescript=config["typescript_version"],
        web_tree_sitter=config["web_tree_sitter_version"],
    )


def series(version: str) -> str:
    return ".".join(version.split(".")[:2])


def ensure_pnpm(pinned: str) -> str:
    pnpm = shutil.which("pnpm")
    if pnpm is None:
        raise RuntimeError(
            f"pnpm is not on PATH; install {pinned} (see site/playground/README.md)"
        )
    found = subprocess.run(
        [pnpm, "--version"], capture_output=True, text=True, check=True
    ).stdout.strip()
    if series(found) != series(pinned):
        raise RuntimeError(
            f"pnpm {found} is on PATH; this build pins {pinned} "
            "(config.toml). Install the pinned series."
        )
    return pnpm


def install_dependencies(versions: Versions) -> Path:
    pnpm = ensure_pnpm(versions.pnpm)
    DEPS_DIR.mkdir(parents=True, exist_ok=True)
    manifest = DEPS_DIR / "package.json"
    wanted = json.dumps(
        {
            "name": "alcy-site-deps",
            "private": True,
            "version": "0.0.0",
            "dependencies": {
                "typescript": versions.typescript,
                "web-tree-sitter": versions.web_tree_sitter,
            },
        },
        indent=2,
    )
    if not manifest.is_file() or manifest.read_text(encoding="utf-8") != wanted:
        manifest.write_text(wanted + "\n", encoding="utf-8")
    subprocess.run(
        [
            pnpm,
            "install",
            "--dir",
            str(DEPS_DIR),
            "--store-dir",
            str(STORE_DIR),
            "--ignore-scripts",
            "--no-frozen-lockfile",
            "--prefer-offline",
            "--reporter",
            "silent",
        ],
        check=True,
    )
    modules = DEPS_DIR / "node_modules"
    log(
        f"deps: typescript {versions.typescript}, "
        f"web-tree-sitter {versions.web_tree_sitter}"
    )
    return modules


def build_typescript(modules: Path) -> None:
    node = shutil.which("node")
    if node is None:
        raise RuntimeError("node is not on PATH; needed to run tsc")
    tsc = modules / "typescript" / "lib" / "tsc.js"
    if not tsc.is_file():
        raise FileNotFoundError(f"{tsc} is missing")
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    for config in ("tsconfig.json", "tsconfig.workers.json", "tsconfig.ssg.json"):
        subprocess.run(
            [node, str(tsc), "-p", config],
            check=True,
            cwd=SITE_DIR,
        )
    # The generator and its tests run under node as ES modules; the marker
    # is what tells node to read the compiled `.js` files that way.
    (BUILD_DIR / "package.json").write_text('{"type": "module"}\n', encoding="utf-8")
    log(f"typescript: {BUILD_DIR}")


def copy_static(dist_dir: Path) -> None:
    target = dist_dir / PAGE_SUBDIR
    target.mkdir(parents=True, exist_ok=True)
    for name in PLAIN_FILES:
        source = PLAYGROUND_DIR / name
        if not source.is_file():
            raise FileNotFoundError(f"{source} is missing")
        shutil.copy2(source, target / name)
    for module in TYPESCRIPT_MODULES:
        source = BUILD_DIR / PAGE_SUBDIR / f"{module}.js"
        if not source.is_file():
            raise FileNotFoundError(f"{source} is missing; run the TypeScript build")
        shutil.copy2(source, target / source.name)
        source_map = source.parent / f"{module}.js.map"
        if source_map.is_file():
            shutil.copy2(source_map, target / source_map.name)


def copy_shared(dist_dir: Path) -> None:
    target = dist_dir / "shared"
    target.mkdir(parents=True, exist_ok=True)
    for name in SHARED_PLAIN_FILES:
        source = SHARED_DIR / name
        if not source.is_file():
            raise FileNotFoundError(f"{source} is missing")
        shutil.copy2(source, target / name)
    for source in sorted((BUILD_DIR / "shared").glob("*.js")):
        shutil.copy2(source, target / source.name)
        source_map = source.with_suffix(".js.map")
        if source_map.is_file():
            shutil.copy2(source_map, target / source_map.name)


def copy_samples(dist_dir: Path) -> None:
    samples_dir = PLAYGROUND_DIR / "samples"
    manifest_path = samples_dir / "samples.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"{manifest_path} is missing")
    with open(manifest_path, "rb") as handle:
        manifest = json.loads(handle.read().decode("utf-8"))
    samples = manifest["samples"]
    target = dist_dir / PAGE_SUBDIR / "samples"
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
    grammar_out = dist_dir / PAGE_SUBDIR / "grammar"
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


def copy_web_tree_sitter(modules: Path, dist_dir: Path, version: str) -> None:
    vendor_out = dist_dir / PAGE_SUBDIR / "vendor"
    vendor_out.mkdir(parents=True, exist_ok=True)
    sources = modules / "web-tree-sitter"
    for name in ("web-tree-sitter.js", "web-tree-sitter.wasm"):
        source = sources / name
        if not source.is_file():
            raise FileNotFoundError(f"{source} is missing")
        shutil.copy2(source, vendor_out / name)
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
    target = dist_dir / PAGE_SUBDIR / "compiler"
    target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(glue, target / "alcy_playground.js")
    shutil.copy2(binary, target / "alcy_playground.wasm")
    log(f"compiler: {glue} ({binary.stat().st_size} bytes of wasm)")
    return True


def build_guide(dist_dir: Path) -> None:
    generator = BUILD_DIR / "ssg" / "src" / "main.js"
    if not generator.is_file():
        raise FileNotFoundError(f"{generator} is missing; run the TypeScript build")
    command = [
        "node",
        str(generator),
        "--docs",
        str(GUIDE_DIR),
        "--dist",
        str(dist_dir),
        "--site",
        str(SITE_DIR),
        "--repo-root",
        str(project_root_dir),
        "--alcy",
        str(project_root_dir / "compiler" / "playground" / "js" / "alcy.mjs"),
    ]
    grammar = dist_dir / PAGE_SUBDIR / "grammar"
    if (grammar / "tree-sitter-alcy.wasm").is_file():
        command += [
            "--grammar",
            str(grammar / "tree-sitter-alcy.wasm"),
            "--highlights",
            str(grammar / "highlights.scm"),
            "--tree-sitter",
            str(DEPS_DIR / "node_modules" / "web-tree-sitter" / "web-tree-sitter.js"),
        ]
    else:
        log("guide: grammar wasm is missing; code fences are not highlighted")
    glue = dist_dir / PAGE_SUBDIR / "compiler" / "alcy_playground.js"
    if glue.is_file():
        command += ["--glue", str(glue)]
    else:
        log("guide: compiler wasm is missing; alcy fences were not compiled")
    subprocess.run(command, check=True, cwd=project_root_dir)


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

    versions = load_versions()
    modules = install_dependencies(versions)
    build_typescript(modules)

    dist_dir = args.dist.resolve()
    if dist_dir.exists():
        shutil.rmtree(dist_dir)
    dist_dir.mkdir(parents=True)

    copy_static(dist_dir)
    copy_shared(dist_dir)
    copy_samples(dist_dir)

    if not args.skip_grammar:
        build_grammar(dist_dir)
    copy_web_tree_sitter(modules, dist_dir, versions.web_tree_sitter)
    copy_compiler(
        dist_dir, project_root_dir / "out" / args.build_subdir, args.compiler_dir
    )
    build_guide(dist_dir)

    log(f"site ready: {dist_dir}")
    log(f"serve it with: uv run ./tools/site.py serve --dist {dist_dir}")
    return 0


def command_test(_args: argparse.Namespace) -> int:
    versions = load_versions()
    modules = install_dependencies(versions)
    build_typescript(modules)
    node = shutil.which("node")
    if node is None:
        raise RuntimeError("node is not on PATH; needed to run the tests")
    subprocess.run(
        [node, "--test", str(BUILD_DIR / "ssg" / "test")],
        check=True,
        cwd=project_root_dir,
    )
    log("generator tests passed")
    return 0


def command_serve(args: argparse.Namespace) -> int:
    dist_dir = args.dist.resolve()
    if not (dist_dir / "index.html").is_file():
        return fail(f"{dist_dir} has no index.html; run `site.py build` first")

    # GitHub Pages compresses the site's text assets; the local server does
    # the same so what is measured here matches what is deployed. The
    # grammar wasm compresses to about an eighth of its size.
    compressible = {
        ".al",
        ".css",
        ".html",
        ".js",
        ".json",
        ".map",
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
    parser = argparse.ArgumentParser(description="Build and serve the site.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    build_parser = subparsers.add_parser("build", help="assemble site/dist")
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
    build_parser.set_defaults(handler=command_build)

    test_parser = subparsers.add_parser("test", help="run the generator's tests")
    test_parser.set_defaults(handler=command_test)

    serve_parser = subparsers.add_parser("serve", help="serve the assembled site")
    serve_parser.add_argument("--dist", type=Path, default=DEFAULT_DIST_DIR)
    serve_parser.add_argument("--host", default="0.0.0.0")
    serve_parser.add_argument("--port", type=int, default=8000)
    serve_parser.set_defaults(handler=command_serve)

    args = parser.parse_args()
    try:
        return args.handler(args)
    except (FileNotFoundError, RuntimeError, subprocess.CalledProcessError) as error:
        return fail(str(error))


if __name__ == "__main__":
    sys.exit(main())
