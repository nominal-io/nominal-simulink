# Packaging the blockset by hand

The toolbox holds one S-function binary per platform, and each binary has to be built on its own
platform. You build and test on Windows, Linux and macOS, copy the two other binaries to the
machine that packages, and package there. CI does the same thing in
`.github/workflows/blockset.yml`.

Check out the same commit on every machine. `simulink/VERSION` sets the toolbox's version, and
the binaries have to come from the same source as everything else that gets packaged.

## Set up each machine

Every machine needs CMake, just, and MATLAB with Simulink, with `matlab` on the PATH and a C
compiler set up (`mex -setup C`). For libcurl:

- Windows needs vcpkg with `curl:x64-windows-static-md`.
- Linux needs the libcurl development package.
- macOS needs only the Xcode Command Line Tools.

Install the oldest supported release in its default folder as well: R2021b on Windows and Linux,
R2023b on a Mac. When it's there, `just` builds the binary and the library with it, and runs the
tests in both that release and the one on the PATH. A binary built with a newer release isn't
guaranteed to load in older ones, and a library opens only in the release that saved it or a
later one. Without the oldest release, the toolbox works only in the release that built it or a
later one.

On Linux, build on a system with glibc 2.28, such as RHEL 8 or Rocky Linux 8. A binary needs
the glibc it was linked against or newer, and the install page promises glibc 2.28. CI builds
in a Rocky Linux 8 container for this reason.

## Build and test on each platform

On each of the three machines:

1. `just test` builds the C library and the S-function, and runs the C tests.
2. `just sim-test` builds the library and runs the block's tests in Simulink.

The binary lands in `simulink/blockset/mex/`:

| Platform | Binary |
|---|---|
| Windows | `nominal_sink_sfun.mexw64` |
| Linux | `nominal_sink_sfun.mexa64` |
| macOS (Apple silicon) | `nominal_sink_sfun.mexmaca64` |

## Package

Package on any of the three machines. It needs R2023a or later on the PATH.

1. Copy the other two binaries into `simulink/blockset/mex/` on that machine. Don't run
   `just clean` after this, because it deletes every binary in that folder.
2. Run `just package`. It rebuilds this machine's binary, the library, the example model and the
   help pages, then writes `dist/NominalBlockset-<version>.mltbx`.
3. Check the last line of the output. It lists the platforms the toolbox claims, which follow the
   binaries it found, and the oldest release it installs in. With all three binaries and the
   oldest release installed, it reads `(Win64, Linux, macOS, R2021b and later)`.
4. Run `just install-test`. It installs the toolbox in a clean MATLAB session, runs the block
   from it and uninstalls it again, in each release.

To test the toolbox on the other two platforms, copy the `.mltbx` into `dist/` on each of them
and run `just install-test` there.

The toolbox claims macOS without naming a processor, so someone on an Intel Mac can install it,
but the block won't load there, because there is no Intel Mac binary.
