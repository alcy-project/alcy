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
      in
      {
        devShells.default = (pkgs.mkShell.override { inherit stdenv; }) {
          nativeBuildInputs = [
            pkgs.gn
            pkgs.ninja
            pkgs.pkg-config
            pkgs.uv
            llvmPkgs.lld
            llvmPkgs.llvm
          ];

          buildInputs = [];

          shellHook = ''
            export LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath runtimeLibs}:$LD_LIBRARY_PATH"
            export LIBCXX_INCLUDE_DIR="${llvmPkgs.libcxx.dev}/include/c++/v1"
            export LIBCXX_LIB_DIR="${llvmPkgs.libcxx}/lib"
          '';
        };
      }
    );
}
