#!/usr/bin/env just --justfile

# Recipes run through bash, so use forward slashes in overrides — backslashes are
# eaten as escapes before the path reaches the executable.

exe := if os() == "windows" { ".exe" } else { "" }

# On Windows, vcpkg supplies libcurl. Without its toolchain CMake finds whatever happens
# to be installed, which on one development machine was GStreamer's bundled copy. On
# Linux and macOS, CMake finds the system's libcurl.
# VCPKG_ROOT is normally set with backslashes, which bash eats as escapes, so
# normalise rather than trusting however it happens to be set.
vcpkg  := replace(env_var_or_default("VCPKG_ROOT", "C:/tools/vcpkg"), "\\", "/")

# static-md is deliberate: static libraries so a .mexw64 needs no DLL beside it, with
# the dynamic CRT that MATLAB MEX is built against. It also matches where the targets
# are going, since Simulink Real-Time packs .a libraries into the application itself.
triplet := env_var_or_default("VCPKG_TRIPLET", "x64-windows-static-md")

curl := if os() == "windows" { "-DCMAKE_TOOLCHAIN_FILE=" + vcpkg + "/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=" + triplet } else { "" }

# MATLAB is the one on the PATH, as in nominal-matlab. Not on the PATH? Set MATLAB_ROOT
# to its install folder, with forward slashes.
matlabroot := env("MATLAB_ROOT", "")
matlab     := if matlabroot != "" { matlabroot + "/bin/matlab" + exe } else { "matlab" }

# When the oldest release the blockset supports is installed in its default folder, it
# builds everything that ships, and the tests and docs run in both releases. A library
# opens only in the release that saved it or a later one, and a MEX built against an
# older release loads in newer ones but not the reverse. CI always builds with it.
#
# On a Mac the oldest is R2023b, the first release that runs natively on Apple silicon.
# Both releases load the one MEX, so they have to be the same architecture.
matlabhome := if os() == "windows" { "C:/Program Files/MATLAB/" } else if os() == "macos" { "/Applications/MATLAB_" } else { "/usr/local/MATLAB/" }
oldestrel  := if os() == "macos" { "R2023b.app" } else { "R2021b" }
oldestdir  := matlabhome + oldestrel
hasoldest  := path_exists(oldestdir)
oldestroot := if hasoldest == "true" { oldestdir } else { matlabroot }
oldest     := if hasoldest == "true" { oldestdir + "/bin/matlab" + exe } else { matlab }
releases   := if oldest == matlab { quote(matlab) } else { quote(oldest) + " " + quote(matlab) }

# With no folder to give it, CMake builds against the MATLAB on the PATH.
cmakematlab := if oldestroot != "" { "-DMatlab_ROOT_DIR=" + quote(oldestroot) } else { "" }

config  := "Release"
builddir := "build"

# Visual Studio builds into a folder per configuration. Makefile and Ninja builds don't.
cfgdir := if os() == "windows" { "/" + config } else { "" }
bindir := builddir + "/cstream" + cfgdir

# Default target
default: test

# Configure the CMake build tree
configure:
    cmake -S . -B {{builddir}} {{curl}} {{cmakematlab}}

# Build the library, demo and tests
build: configure
    cmake --build {{builddir}} --config {{config}}

# Build and run the offline test suites (no token or network needed)
test: build
    cd {{builddir}} && ctest -C {{config}} --output-on-failure

# Build the relay for this machine into build/. Release builds for each platform come
# from CI, which also strips symbols and disables cgo.
relay:
    cd nmproxy && go build -trimpath \
      -ldflags "-X main.version=$(tr -d '[:space:]' < ../simulink/VERSION)-dev" \
      -o ../{{builddir}}/nmproxy{{exe}} ./cmd/nmproxy

# Vet and test the relay
relay-test:
    cd nmproxy && go vet ./... && go test ./...

# Usage:
#   just demo ri.catalog....
#   just demo ri.catalog.... https://api.example.nominal.io/api
#
# Stream synthetic data to a real dataset — needs NOMINAL_TOKEN
demo rid base="": build
    {{bindir}}/nmstream-demo{{exe}} {{rid}} {{base}}

# Guards against protocol drift: captures what the Rust client actually puts on the
# wire, and test_encode is pinned byte-for-byte against it. Run after upgrading
# nominal-streaming. If the regenerated fixtures differ from the committed ones,
# libnominalstream needs updating. That is how the body encoding switching from snappy
# to zstd was caught.
#
# Regenerate the golden protobuf fixtures from nominal-streaming
fixtures:
    cd tools/fixturegen && cargo run --release

# The MATLAB and Go frame decoders are tested against cstream/tests/fixtures/frames.txt,
# so all three codecs are checked against the same bytes. This prints the HELLO and DATA
# frames the C encoder makes. Paste them into that file when the frame format changes.
#
# Print the golden host-to-proxy frames for frames.txt
frames: build
    {{bindir}}/test_frame{{exe}} --fixtures

# Regenerate the Simulink library holding the masked sink block
library: build
    "{{oldest}}" -batch "addpath('simulink/tools'); make_library"

# Regenerate the example model the documentation opens
example: library
    "{{oldest}}" -batch "addpath('simulink/tools'); make_example"

# Regenerate the HTML documentation. Each release builds the search index it reads.
docs:
    for m in {{releases}}; do "$m" -batch "addpath('simulink/tools'); make_docs" || exit 1; done

# Build the toolbox installer, build/NominalBlockset-<version>.mltbx
package: example docs
    "{{matlab}}" -batch "addpath('simulink/tools'); package_blockset"

# Install the package into a clean session in each release, check the block works from
# it, and uninstall it again
install-test:
    for m in {{releases}}; do "$m" -batch "addpath('simulink/tests'); assertSuccess(runtests('InstalledToolboxTest'))" || exit 1; done

# Load the block and step a model without touching the network, in each release
sim-test: build library
    for m in {{releases}}; do "$m" -batch "addpath('simulink/tests'); assertSuccess(runtests({'SinkBlockTest', 'AutonamesTest', 'LibraryMetadataTest'}))" || exit 1; done

# Check the MATLAB frame decoder against the same fixtures as the C and Go ones
matlab-test:
    "{{matlab}}" -batch "addpath('matlab', fullfile('matlab','tests')); nmframe_test"

# Usage:
#   just matlab-proxy            listen on 15400 until Ctrl-C
#   just matlab-proxy 15401      listen on 15401
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

# The user docs in docs/, published on the Nominal docs hub at /matlab/simulink/. Python is
# here only for these, through uv. Warnings fail the build, as they do in CI.
#
# Build the docs into docs/_build/dirhtml
build-docs:
    uv run --group docs sphinx-build -E -W --keep-going -b dirhtml docs docs/_build/dirhtml

# Serve the docs on http://127.0.0.1:8000, rebuilding on page or source edits
serve-docs:
    uv run --group docs --with sphinx-autobuild sphinx-autobuild -b dirhtml docs docs/_build/dirhtml --watch simulink --watch cstream/src --watch nmproxy
