# Golden fixtures

Reference bytes used to test this library's encoders **offline**, with no server, no API key
and no network. The `body_*` files are byte-exact captures of what the Rust client
(`nominal-streaming`) puts on the wire. `frames.txt` holds the host-to-relay frames that the
MATLAB and Go decoders are checked against.

## What the body files encode

One `WriteRequestNominal` with two channels, three `DoublePoint`s each, all timestamped from
`2026-01-01T00:00:00Z` (1767225600 s):

| Channel | Tags | Points (offset ns, value) |
|---|---|---|
| `demo.rpm` | `bank=1` | (0, 1500.0) (1000000, 1510.5) (2000000, 1521.25) |
| `demo.egt` | — | (0, 700.0) (1000000, 701.5) (2000000, 703.0) |

`session_name` is unset. Only one tag per channel is used on purpose: protobuf map fields have
no defined ordering, so a two-tag channel would not encode reproducibly.

## Files

| File | Bytes | What it is |
|---|---|---|
| `body_identity.pb` | 177 | The bare protobuf. What our encoder must produce byte-for-byte. |
| `body_zstd.bin` | 113 | zstd level 1 — **what the current client actually sends** (`Content-Encoding: zstd`). |
| `body_snappy.bin` | 141 | snappy framed — what the client sent before 0.9.6. Kept to probe whether the server still accepts it. |
| `frames.txt` | — | Golden HELLO and DATA frames from the C encoder, decoded by the MATLAB and Go tests. |

## Headers the Rust client sends

```
POST {base_url}/storage/writer/v1/nominal/{dataset_rid}
content-type: application/x-protobuf
content-encoding: zstd
authorization: Bearer <token>
```

## Why these exist

The body encoding changed from `x-snappy-framed` to `zstd` between `nominal-streaming` 0.9.4
and 0.9.6 — two patch releases days apart. Reading the protocol out of a checked-out copy of the
source gave the wrong answer; capturing it from the running client gave the right one.

Since this repository reimplements that protocol, it needs to notice when the protocol moves.
After upgrading `nominal-streaming`, regenerate these and diff: a change in the captured
headers or body framing is the signal that this library needs updating.

## Regenerating

`just fixtures` regenerates the `body_*` files. It runs a small Rust harness
(`tools/fixturegen/`) that constructs the request with `nominal-api-proto`'s prost types and runs
it through `nominal_streaming::client::encode_request`.

`just frames` prints the frames in `frames.txt` from the C encoder.
