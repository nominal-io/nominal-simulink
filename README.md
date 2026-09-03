# nominal-simulink

Streams signals from a running Simulink model into Nominal, on the desktop and on
Speedgoat real-time targets — QNX Neutrino for R2021a–R2025b, the Linux-based Antelope
RTOS for R2026a and later.

**[USING.md](USING.md) is the guide**: adding the block to a model, configuring it, and
running the proxy.

## What is here

```
cstream/     the C streaming library — standalone, no Simulink dependency
simulink/    the Simulink block, its mask, and the library that holds it
nmproxy/     the Go relay daemon, for hosts with no headroom to spare
tools/       captures the golden protocol fixtures from the Rust client
```

## Two ways to send

**Direct** — the model's machine talks HTTPS to Nominal itself. Nothing else to deploy.

**Proxy** — the model sends compact UDP datagrams to a relay, which does the protobuf,
TLS and HTTP. For hosts already at their performance limit, and for those that should
hold no credential: in proxy mode the dataset identifier travels in the frames and the
token lives only on the relay.

Both are the same block, selected by a dropdown, so moving from one to the other is a
configuration change rather than rewiring a model.

## Why C

The Simulink block must survive code generation to a Speedgoat target, and the obvious
approach — reusing the existing Rust client — required feature and version changes across
four upstream crates plus a tier-3 Rust toolchain before anything would compile for QNX.
The thing being talked to is one ordinary HTTPS POST, and QNX ships OpenSSL and has
official curl builds. Implementing the write path in C removed all of it.

## Design notes worth knowing

**The step function does almost nothing.** All memory is reserved when the model starts,
so pushing a sample never allocates and never blocks. Encoding, TLS and HTTP happen on a
background thread.

**Loss is bounded and visible.** If the network cannot keep up, samples are discarded and
counted rather than stalling a model that may be driving hardware. The block warns at the
end of a run if anything was lost, and can expose a live count on an output port.

**The protocol is written twice on purpose.** The frame format between host and relay is
implemented independently in C and in Go, pinned to shared golden fixtures. A format
meant for third parties to implement has to be followable from its specification, and
writing it twice is how we find out whether it is.

## Building and testing

```powershell
just build       # library, block, demo
just library     # regenerate the Simulink block library
just test        # C tests: encoder, ring buffer, frame codec
just sim-test    # block tests: loads, sizes ports, steps, proxy mode
```

None of the tests need a token or network access. See [USING.md](USING.md) for the
prerequisites and for streaming to a real dataset.
