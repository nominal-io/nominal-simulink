# Frame protocol

{.lead}
The UDP packets the block sends to `nmproxy` in proxy mode, for anyone writing their own sender
or relay.

Each UDP packet holds one frame. A frame is either a HELLO, which announces a dataset RID, an
ordered list of channel names and the tags every channel carries, or a DATA frame, which carries
rows: a timestamp and one value per channel, in the HELLO's order. Both carry a `config_id`, a
hash of the RID, the channel names and the tags, and a DATA frame is matched to its HELLO through
the `config_id`.

A sender sends a HELLO when it starts and repeats it every second for as long as it runs. A
relay that starts, or restarts, partway through a run learns the channel list within a second,
and drops DATA frames whose `config_id` it hasn't seen in a HELLO until then. Nothing travels
back from the relay to the sender, and frames carry no authentication.

Version 1 is the version described here.

## Encoding

Every numeric field is little-endian. The magic is the four bytes `NMS1` in that order, so it
reads forwards in a packet capture. No frame is longer than 1400 bytes, which keeps it out of IP
fragmentation.

Both frame types start with the same 24-byte header:

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | `4E 4D 53 31`, `NMS1` |
| 4 | 1 | version | `1`. A relay rejects frames of any other version. |
| 5 | 1 | type | `1` for HELLO, `2` for DATA |
| 6 | 2 | reserved | `0` |
| 8 | 8 | session_id | Distinguishes runs. Any value that differs between concurrent runs. |
| 16 | 8 | config_id | The [hash](#config-id) of the dataset RID, channel names and tags |

In ABNF ([RFC 5234](https://www.rfc-editor.org/rfc/rfc5234)), where `nOCTET` is exactly n
octets and `%xNN` a literal octet:

```{frame-spec}
```

The grammar can't express these rules, which every frame also follows:

1. A HELLO has exactly `channel-count` channel names and exactly `tag-count` tags.
2. A DATA frame has exactly `row-count` rows.
3. Each row has exactly `channel-count` values.
4. A DATA frame's `channel-count` equals that of the HELLO with the same `config_id`.
5. No frame is longer than 1400 bytes.
6. `channel-count` is 1 to 169, `tag-count` is at most 64, and a DATA frame's `row-count` is
   at least 1.

A decoder has to check every count against the bytes it actually received. A frame can match
the grammar and still claim more rows than it carries.

## The sequence number

`sequence` counts DATA frames within a session, going up by one per frame. A sender advances it
even when sending a frame fails, so a relay that sees a jump knows frames were lost on the way. A
lower number than one already seen is a frame that arrived out of order, not a lost one. The
first DATA frame of a session can carry any sequence number.

(config-id)=
## config_id

`config_id` is the 64-bit FNV-1a hash of the dataset RID, followed by each channel name in
order, then each tag's key and value in order. Each of those strings goes into the hash preceded
by its length as a 4-byte little-endian integer, which keeps `["ab", "c"]` and `["a", "bc"]`
apart. That length prefix is 4 bytes, unlike the 2-byte lengths in the frame itself. With no
tags, the hash covers the RID and names alone.

```
h = 0xcbf29ce484222325                        offset basis
for each string s in [rid, name1, name2, ..., key1, value1, key2, value2, ...]:
    for each byte b in u32_le(len(s)) followed by the bytes of s:
        h = (h XOR b) * 0x100000001b3         modulo 2^64
```

To check an implementation, FNV-1a/64 of the single byte `a` is `0xaf63dc4c8601ec8c`. Go's
standard library has the hash as `hash/fnv` (`fnv.New64a`). A relay drops a HELLO whose
`config_id` doesn't match its contents.

## Limits

A DATA row is 8 bytes of timestamp plus 8 bytes per channel, and at least one row has to fit
after the header and the fixed DATA fields:

8 × channels ≤ 1400 − 24 − 12 − 8, so at most 169 channels.

A DATA frame holds ⌊1364 ÷ (8 + 8 × channels)⌋ rows, which is 42 rows at 3 channels.

A HELLO holds the whole channel list and every tag. A sender doesn't split them across frames,
so they have to fit in one:

30 + (RID length) + the sum over channels of (2 + name length) + the sum over tags of
(4 + key length + value length) ≤ 1400

The RID, each name, each tag key and each tag value can be at most 256 bytes, and a HELLO carries
at most 64 tags.

## Example

The two frames below are the shared fixtures that every implementation in the repository is
tested against. They encode this input:

| | |
|---|---|
| dataset RID | `ri.catalog.test.dataset.abc` |
| channels | `engine.rpm`, `engine.egt`, `engine.psi` |
| tags | `model` = `A`, `execution-id` = `7` |
| session_id | `0x1122334455667788` |
| config_id | `0x5bbb5bdfcbf6bf55` |
| sequence | 42 |
| rows | 1000000000: 1500.0, 700.0, 30.0<br>1000001000: 1510.5, 701.5, 30.1<br>1000002000: 1521.25, 703.0, 30.2 |

The HELLO, 120 bytes:

```
4e4d5331                    magic "NMS1"
01                          version 1
01                          type HELLO
0000                        reserved
8877665544332211            session_id
55bff6cbdf5bbb5b            config_id
1b00                        RID length 27
72692e636174616c6f672e746573742e646174617365742e616263
                            "ri.catalog.test.dataset.abc"
0300                        3 channels
0a00 656e67696e652e72706d   "engine.rpm"
0a00 656e67696e652e656774   "engine.egt"
0a00 656e67696e652e707369   "engine.psi"
0200                        2 tags
0500 6d6f64656c             key "model"
0100 41                     value "A"
0c00 657865637574696f6e2d6964
                            key "execution-id"
0100 37                     value "7"
```

The DATA frame, 132 bytes:

```
4e4d5331 01 02 0000         magic, version 1, type DATA, reserved
8877665544332211            session_id
55bff6cbdf5bbb5b            config_id
2a00000000000000            sequence 42
0300                        3 channels
0300                        3 rows
00ca9a3b00000000            timestamp 1000000000
0000000000709740            1500.0
0000000000e08540            700.0
0000000000003e40            30.0
e8cd9a3b00000000            timestamp 1000001000
00000000009a9740            1510.5
0000000000ec8540            701.5
9a99999999193e40            30.1
d0d19a3b00000000            timestamp 1000002000
0000000000c59740            1521.25
0000000000f88540            703.0
3333333333333e40            30.2
```

## Implementations

The [nominal-simulink repository](https://github.com/nominal-io/nominal-simulink) has the
protocol in three languages. The block's encoder in C (`cstream/src/nm_frame.c`) produces the
frames above, and two decoders are tested against them: the relay's in Go (`nmproxy/frame/`) and
one in MATLAB (`matlab/nmframe_decode.m`). `matlab/nmproxy_reference.m` is a small relay in MATLAB that
decodes and prints what arrives without writing to Nominal, written to be read alongside this
page.
