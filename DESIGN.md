# Design

How the Nominal Blockset works, and why it is built the way it is. [README.md](README.md) covers
installing it, and [PACKAGING.md](PACKAGING.md) building it.

## How it sends

The block's **Mode** parameter chooses the path. In direct mode, the computer running the model
sends to Nominal over HTTPS, with the API key and URL from a profile in Nominal's config file,
the one Nominal's other clients read. In proxy mode, the block sends UDP packets to `nmproxy`,
which holds the API key and writes to Nominal, so the computer running the model needs no key
and does no HTTPS work.

## A model step never waits for the network

Each step copies one row into a buffer reserved when the model started, so it neither allocates
memory nor blocks. A background thread does the encoding, TLS and HTTP. When the run ends, the
block spends up to 15 seconds sending what is left in the buffer.

## Dropped rows are counted

When sending can't keep up, the block drops rows rather than stall a model that may be driving
hardware. It warns at the end of a run that dropped any, and can report a running count on an
output port.

## The relay protocol has three implementations

It is specified in `cstream/src/nm_frame.h` and implemented in C, Go and MATLAB, all tested
against the same fixtures. The format is meant for others to implement from that specification,
and independent implementations are how we find out whether the specification is enough.

## Why C

The block is written in C so it can be built for Speedgoat real-time targets, which run QNX
through Simulink Real-Time R2025b and Linux from R2026a. QNX ships OpenSSL and has official
builds of curl, the HTTPS library the block uses. Nominal's Rust client would need changes to
several of its dependencies before it built for QNX.

## What is where

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
