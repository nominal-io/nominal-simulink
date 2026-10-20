# Building, testing and packaging

Build and test the blockset on the machine you're on with the recipes below. The toolbox holds
one S-function binary per platform, and each binary has to be built on its own platform, so a
toolbox for all three platforms means building on Windows, Linux and macOS and packaging on one
of them. CI does the same thing in `.github/workflows/blockset.yml`.

## Set up a machine

Every machine needs CMake, just, and MATLAB with Simulink, with `matlab` on the PATH and a C
compiler set up (`mex -setup C`). For libcurl:

- Windows needs vcpkg with `curl:x64-windows-static-md`.
- Linux needs the libcurl development package.
- macOS needs only the Xcode Command Line Tools.

`nmproxy` needs Go 1.25, `just fixtures` needs Rust, and the docs need uv.

Install the oldest supported release in its default folder as well: R2021b on Windows and Linux,
R2023b on a Mac. When it's there, `just` builds the binary and the library with it, and runs the
tests in both that release and the one on the PATH. A binary built with a newer release isn't
guaranteed to load in older ones, and a library opens only in the release that saved it or a
later one. Without the oldest release, the toolbox works only in the release that built it or a
later one.

On Linux, build on a system with glibc 2.28, such as RHEL 8 or Rocky Linux 8. A binary needs
the glibc it was linked against or newer, and the install page promises glibc 2.28. CI builds
in a Rocky Linux 8 container for this reason.

## Build and test

```shell
just build         # the C library, the S-function and the demo
just test          # the C library's tests
just sim-test      # the block's tests, in both MATLAB releases
just package       # everything that ships, then dist/NominalBlockset-<version>.mltbx
just install-test  # installs that toolbox in a clean session and runs the block from it
just relay-all     # the relay for every platform, into dist/
just build-docs    # the user docs, into docs/_build/dirhtml
```

`just --list` shows the rest. None of the tests need an API key or network access.

Everything generated goes into `simulink/blockset/`, which is exactly what the toolbox contains.
The library, the example model and the help pages are generated rather than committed, so the
block's definition stays in source that a diff can review. To use the blockset from a clone
without installing it, run `simulink/nominal_setup.m`.

The binary lands in `simulink/blockset/mex/`:

| Platform | Binary |
|---|---|
| Windows | `nominal_sink_sfun.mexw64` |
| Linux | `nominal_sink_sfun.mexa64` |
| macOS (Apple silicon) | `nominal_sink_sfun.mexmaca64` |

## Debugging the block

Configure CMake with `-DNM_SFUN_TRACE=ON` and the S-function logs each callback as it enters,
which narrows a crash or hang to one callback. `just demo <dataset RID> [profile]` streams
synthetic rows to a dataset without Simulink, with the `default` profile unless you name another.

## Package for every platform

Check out the same commit on every machine. `simulink/VERSION` sets the toolbox's version, and
the binaries have to come from the same source as everything else that gets packaged.

1. On each of the three machines, run `just test` and `just sim-test`.
2. On the machine that packages, which needs R2023a or later on the PATH, copy the other two
   binaries into `simulink/blockset/mex/`. Don't run `just clean` after this, because it deletes
   every binary in that folder.
3. Run `just package`. It rebuilds this machine's binary, the library, the example model and the
   help pages, then writes `dist/NominalBlockset-<version>.mltbx`.
4. Check the last line of the output. It lists the platforms the toolbox claims, which follow the
   binaries it found, and the oldest release it installs in. With all three binaries and the
   oldest release installed, it reads `(Win64, Linux, macOS, R2021b and later)`.
5. Run `just install-test`. It installs the toolbox in a clean MATLAB session, runs the block
   from it and uninstalls it again, in each release.

To test the toolbox on the other two platforms, copy the `.mltbx` into `dist/` on each of them
and run `just install-test` there.

The toolbox claims macOS without naming a processor, so someone on an Intel Mac can install it,
but the block won't load there, because there is no Intel Mac binary.

CI builds the S-function on Windows, Linux and macOS, packages all three into the toolbox, tests
the installed toolbox on each platform in its oldest and newest release, and attaches the
toolbox and the relay archives to a GitHub release for each `v*` tag.
