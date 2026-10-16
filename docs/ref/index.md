# Reference

{.lead}
Exact behavior of the block, its messages, the relay and the protocol between them.

```{toctree}
:hidden:

block
messages
nmproxy
frames
```

- [Nominal Stream Sink](block.md): the block's ports, every parameter and its default, and the
  environment variables it reads.
- [Errors and warnings](messages.md): every message the block and its dialog show, with the cause
  and the fix.
- [nmproxy](nmproxy.md): the relay's command line, configuration file and log output.
- [Frame protocol](frames.md): the UDP packets proxy mode sends, for writing your own sender or
  relay.
