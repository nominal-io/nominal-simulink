# nominal-simulink

A Simulink block that streams signals from a running model into a Nominal dataset, and the
relay it can stream through.

The user docs are in [docs/](docs/): installing the blockset, a quickstart, and every
parameter and message. `just serve-docs` serves them on http://127.0.0.1:8000. The block's
in-toolbox help is generated from `simulink/doc-src/`.

## What is here

```
cstream/            the C streaming library the block is built on, with no Simulink dependency
simulink/blockset/  the toolbox's contents as they ship: library, mask callbacks, S-function, help
simulink/doc-src/   the source of the in-toolbox help
simulink/src/       the S-function's source
simulink/tools/     scripts that generate the library, the example, the help and the toolbox
simulink/tests/     the block's tests
nmproxy/            the relay that proxy mode sends to, in Go
matlab/             a readable MATLAB decoder for the relay protocol, and a relay that only prints
docs/               the user docs
tools/              captures direct mode's protobuf fixtures from the nominal-streaming Rust crate
cmake/              the toolchain file for cross-compiling to QNX
```

## How it sends

The block's **Mode** parameter chooses the path. In direct mode, the computer running the
model sends to Nominal over HTTPS, with the API key and URL from a profile in Nominal's
config file, the one Nominal's other clients read. In proxy mode, the block sends UDP packets to `nmproxy`, which holds the API key and
writes to Nominal, so the computer running the model needs no key and does no HTTPS work.

## Design

**A model step never waits for the network.** Each step copies one row into a buffer reserved
when the model started, so it neither allocates memory nor blocks. A background thread does the
encoding, TLS and HTTP. When the run ends, the block spends up to 15 seconds sending what is
left in the buffer.

**Dropped rows are counted.** When sending can't keep up, the block drops rows rather than stall
a model that may be driving hardware. It warns at the end of a run that dropped any, and can
report a running count on an output port.

**The relay protocol has three implementations.** It is specified in `cstream/src/nm_frame.h`
and implemented in C, Go and MATLAB, all tested against the same fixtures. The format is meant
for others to implement from that specification, and independent implementations are how we
find out whether the specification is enough.

## Why C

Reusing Nominal's Rust client would have taken feature and version changes in four upstream
crates, and a tier-3 Rust target, before it compiled for QNX, the operating system of Speedgoat
real-time targets up to R2025b. The block needs only one HTTPS POST, and QNX ships OpenSSL and
has official curl builds, so the write path is in C.

## Building and testing

You need CMake, just, and MATLAB R2023a or later with Simulink, with `matlab` on the PATH and a
C compiler set up (`mex -setup C`). For libcurl, Windows needs vcpkg with
`curl:x64-windows-static-md`, Linux needs the libcurl development package, and macOS needs only
the Xcode Command Line Tools. `nmproxy` needs Go 1.25, `just fixtures` needs Rust, and the docs
need uv.

If R2021b, the oldest release the blockset supports, is installed in its default folder, it
builds the S-function and the library, and the tests run in both releases. A library opens only
in the release that saved it or a later one, so a toolbox built without R2021b installs only in
the release that built it or a later one. On a Mac, R2023b takes the place of R2021b, because it
is the first release that runs natively on Apple silicon.

```shell
just build         # the C library, the S-function and the demo
just test          # the C library's tests
just sim-test      # the block's tests, in both MATLAB releases
just package       # everything that ships, then build/NominalBlockset-<version>.mltbx
just install-test  # installs that toolbox in a clean session and runs the block from it
just build-docs    # the user docs, into docs/_build/dirhtml
```

`just --list` shows the rest. None of the tests need an API key or network access.
[PACKAGING.md](PACKAGING.md) covers building on each platform and packaging the toolbox by hand.


Everything generated goes into `simulink/blockset/`, which is exactly what the toolbox contains.
The library, the example model and the help pages are generated rather than committed, so the
block's definition stays in source that a diff can review. To use the blockset from a clone
without installing it, run `simulink/nominal_setup.m`.

`simulink/VERSION` sets the version. `.github/workflows/blockset.yml` builds the S-function on
Windows, Linux and macOS, packages all three into the toolbox, tests the installed toolbox on
each platform in its oldest and newest release, and attaches the toolbox to a GitHub release for
each `v*` tag.

## Debugging the block

Configure CMake with `-DNM_SFUN_TRACE=ON` and the S-function logs each callback as it enters,
which narrows a crash or hang to one callback. `just demo <dataset RID> [profile]` streams
synthetic rows to a dataset without Simulink, with the `default` profile unless you name another.
