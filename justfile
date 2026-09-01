#!/usr/bin/env just --justfile

# Recipes run through bash, so use forward slashes in overrides — backslashes are
# eaten as escapes before the path reaches the executable.

# vcpkg supplies libcurl. Without its toolchain CMake finds whatever happens to be
# installed, which on this machine was GStreamer's bundled copy.
# VCPKG_ROOT is normally set with backslashes, which bash eats as escapes, so
# normalise rather than trusting however it happens to be set.
vcpkg  := replace(env_var_or_default("VCPKG_ROOT", "C:/tools/vcpkg"), "\\", "/")

# static-md is deliberate: static libraries so a .mexw64 needs no DLL beside it, with
# the dynamic CRT that MATLAB MEX is built against. It also matches where the targets
# are going, since Simulink Real-Time packs .a libraries into the application itself.
triplet := env_var_or_default("VCPKG_TRIPLET", "x64-windows-static-md")

config  := "Release"
builddir := "build"

# Default target
default: test

# Configure the CMake build tree
configure:
    cmake -S . -B {{builddir}} \
      -DCMAKE_TOOLCHAIN_FILE={{vcpkg}}/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET={{triplet}}

# Build the library, demo and tests
build: configure
    cmake --build {{builddir}} --config {{config}}

# Build and run the offline test suites (no token or network needed)
test: build
    cd {{builddir}} && ctest -C {{config}} --output-on-failure

# Usage:
#   just demo rid=ri.catalog....
#   just demo rid=ri.catalog.... base=https://api.example.nominal.io/api
#
# Stream synthetic data to a real dataset — needs NOMINAL_TOKEN
demo rid base="": build
    {{builddir}}/cstream/{{config}}/nmstream-demo.exe {{rid}} {{base}}

# Guards against protocol drift: captures what the Rust client actually puts on the
# wire, and test_encode is pinned byte-for-byte against it. Run after upgrading
# nominal-streaming — a change here means this library needs updating, and it is how we
# caught the body encoding switching from snappy to zstd.
#
# Regenerate the golden protobuf fixtures from nominal-streaming
fixtures:
    cd tools/fixturegen && cargo run --release

# Delete the build tree
clean:
    rm -rf {{builddir}}

# Show what is available
help:
    @just --list
