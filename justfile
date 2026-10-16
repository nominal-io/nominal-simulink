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

# MATLAB is often not on PATH. Override with the install root, forward slashes only:
#   just matlabroot='C:/Program Files/MATLAB/R2025b' sim-test
#
# Two releases, with different jobs. The oldest one the blockset supports builds
# everything that ships: a library opens only in the release that saved it or a later
# one, and a MEX built against an older release loads in newer ones but not the
# reverse. The newest runs the tooling that needs it — packaging needs R2023a+, and
# code generation needs Simulink Coder, which is only installed there.
oldestroot := env("MATLAB_OLDEST", "C:/Program Files/MATLAB/R2021b")
matlabroot := env("MATLABROOT", "C:/Program Files/MATLAB/R2026a")
oldest     := oldestroot + "/bin/matlab.exe"
matlab     := matlabroot + "/bin/matlab.exe"

config  := "Release"
builddir := "build"

# Default target
default: test

# Configure the CMake build tree
configure:
    cmake -S . -B {{builddir}} \
      -DCMAKE_TOOLCHAIN_FILE={{vcpkg}}/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET={{triplet}} \
      -DMatlab_ROOT_DIR="{{oldestroot}}"

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

# Pins the MATLAB and Go frame decoders against the C encoder, so three independent
# codec implementations agree by construction rather than by inspection.
#
# Regenerate the golden host-to-proxy frames
frames: build
    {{builddir}}/cstream/{{config}}/test_frame.exe --fixtures

# Regenerate the Simulink library holding the masked sink block
library: build
    "{{oldest}}" -batch "addpath('simulink/tools'); make_library"

# Regenerate the example model the documentation opens
example: library
    "{{oldest}}" -batch "addpath('simulink/tools'); make_example"

# Regenerate the HTML documentation. Each release builds the search index it reads.
docs:
    "{{oldest}}" -batch "addpath('simulink/tools'); make_docs"
    "{{matlab}}" -batch "addpath('simulink/tools'); make_docs"

# Build the toolbox installer, build/NominalBlockset-<version>.mltbx
package: example docs
    "{{matlab}}" -batch "addpath('simulink/tools'); package_blockset"

# Install the package into a clean session on both releases, check the block works from
# it, and uninstall it again
install-test:
    "{{oldest}}" -batch "addpath('simulink/tests'); assertSuccess(runtests('InstalledToolboxTest'))"
    "{{matlab}}" -batch "addpath('simulink/tests'); assertSuccess(runtests('InstalledToolboxTest'))"

# Load the block and step a model without touching the network, on both releases
sim-test: build library
    "{{oldest}}" -batch "addpath('simulink/tests'); assertSuccess(runtests({'SinkBlockTest', 'AutonamesTest', 'LibraryMetadataTest'}))"
    "{{matlab}}" -batch "addpath('simulink/tests'); assertSuccess(runtests({'SinkBlockTest', 'AutonamesTest', 'LibraryMetadataTest'}))"

# Check the MATLAB frame decoder against the same fixtures as the C and Go ones
matlab-test:
    "{{matlab}}" -batch "addpath('matlab', fullfile('matlab','tests')); nmframe_test"

# Usage:
#   just matlab-proxy            listen on 15400 until Ctrl-C
#   just matlab-proxy port=15401
#
# Run the readable reference proxy — decodes and reports, writes nothing
matlab-proxy port="15400":
    "{{matlab}}" -batch "addpath('matlab'); nmproxy_reference('Port', {{port}})"

# Delete the build tree and everything generated into the blockset
clean:
    rm -rf {{builddir}} simulink/blockset/mex/*.mex* simulink/blockset/library/*.slx \
      simulink/blockset/examples simulink/blockset/doc/html simulink/blockset/doc/helptoc.xml \
      simulink/blockset/doc/helpsearch-* simulink/blockset/script/Contents.m \
      simulink/blockset/codegen

# Show what is available
help:
    @just --list
