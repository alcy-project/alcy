# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

import subprocess
import sys


def build_llvm(
    build_type,
    configure_script,
    llvm_src_dir,
    llvm_build_dir,
    llvm_install_dir,
    compiler_launcher,
    c_compiler,
    cxx_compiler,
    generator,
    triple,
    assertions,
    libcxx,
):
    """
    Simulates the LLVM build step logic.
    It runs the configuration script followed by the cmake build/install command.
    """

    # Prepare the Configuration Command
    configure_cmd = [
        "sh",
        configure_script,
        llvm_src_dir,
        llvm_build_dir,
        llvm_install_dir,
        build_type,
        assertions,
        triple,
        libcxx,
        generator,
        compiler_launcher or "",
        c_compiler or "",
        cxx_compiler or "",
    ]

    # Prepare the Build Command
    build_cmd = ["cmake", "--build", llvm_build_dir, "--target", "install", "-j"]

    try:
        # Run Configuration
        print(f"--> Configuring LLVM in: {llvm_build_dir}")
        subprocess.run(configure_cmd, check=True)

        # Run Build/Install
        print(f"--> Building and Installing LLVM...")
        subprocess.run(build_cmd, check=True)

        print("--> LLVM Build and Install completed successfully.")
        return 0
    except subprocess.CalledProcessError as e:
        print(f"Error during LLVM build process: {e}", file=sys.stderr)
        return 1
