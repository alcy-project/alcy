{
  description = "alcy development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        llvmPkgs = pkgs.llvmPackages_22;

        stdenv = llvmPkgs.libcxxStdenv;

        runtimeLibs = [
          llvmPkgs.libcxx
        ];

        shellHook = ''
          export LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath runtimeLibs}:$LD_LIBRARY_PATH"
          export LIBCXX_INCLUDE_DIR="${llvmPkgs.libcxx.dev}/include/c++/v1"
          export LIBCXX_LIB_DIR="${llvmPkgs.libcxx}/lib"
        '';
      in
      {
        devShells.default = (pkgs.mkShell.override { inherit stdenv; }) {
          nativeBuildInputs = [
            pkgs.gn
            pkgs.git
            pkgs.ninja
            pkgs.pkg-config
            pkgs.uv
            pkgs.ast-grep
            pkgs.taplo
            llvmPkgs.lld
            llvmPkgs.llvm
            # The grammar is checked against the compiler, so the CLI that
            # builds the parser belongs beside it. config.toml names the
            # version, because the version that generates the parser is the
            # one its output is compared against.
            pkgs.tree-sitter
            pkgs.bun
            # `check.sh` runs the spell check through typos when it is on
            # PATH and only notes its absence, so the shell carries it and
            # the note never has a reason to appear.
            pkgs.typos
          ];

          buildInputs = [];

          inherit shellHook;
        };

        # The wasm gates need Emscripten, a closure far larger than the
        # tools around it; it stays out of the default shell so the nix
        # matrix entries never download it, and `check.sh --nix` runs its
        # wasm gates from here:
        #   nix develop .#wasm --command ./tools/check.sh --nix
        devShells.wasm = (pkgs.mkShell.override { inherit stdenv; }) {
          inputsFrom = [ self.devShells.${system}.default ];
          nativeBuildInputs = [ pkgs.emscripten ];

          inherit shellHook;
        };
      }
    );
}
