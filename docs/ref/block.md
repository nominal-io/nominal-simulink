# Nominal Stream Sink

{.lead}
The block's ports, parameters and the environment variables it reads.

The block is in the library `nominal_lib`, listed as **Nominal** in the Simulink Library Browser.
Its label on the canvas shows the mode it sends in: **(direct)** or **(proxy)**.

## Ports

The block has one input, a vector of doubles whose width equals the number of channel names.
The first element is written to the first channel, the second to the second, and so on.

The block has no output unless **Add an output port with the discarded-row count** is on. The output
is then a double scalar: the number of rows dropped so far in the current run.

## Parameters

Text parameters take a MATLAB expression, so enter text in single or double quotes, or the name
of a variable that holds the text. No parameter can change while the model runs. The dialog greys out
**API base URL** in proxy mode, and **Proxy host** and **Proxy port** in direct mode.

**Channel names (comma-separated)**
: Default `'engine.rpm, engine.egt, engine.psi'`. The channel names, in the order of the
  input's elements. The number of names sets the input's width. Spaces around each name are
  ignored, and so are empty entries such as a trailing comma.

**Detect from connected signal**
: A button. Fills in the channel names from whatever is wired into the block.
  [Choose and name channels](../guides/channels.md#detect-the-names-from-the-model) lists where
  the names come from.

**Dataset RID**
: Default `''`. The RID of the dataset to write to. The dataset has to exist already. Required.

**Mode**
: Default **Direct to Nominal**. **Direct to Nominal** sends over HTTPS from the computer running
  the model. **Via proxy** sends over UDP to `nmproxy`, which writes to Nominal. See
  [Stream through a proxy](../guides/proxy.md).

**API base URL**
: Default `'https://api.gov.nominal.io/api'`, the standard cloud instance. Direct mode only. The
  REST API base URL of the Nominal instance the dataset belongs to: the **API URL** on your
  organization's **API keys** page. Required in direct mode.

**Proxy host**
: Default `'127.0.0.1'`. Proxy mode only. The name or IP address of the computer running
  `nmproxy`.

**Proxy port**
: Default `15400`. Proxy mode only. The UDP port `nmproxy` listens on, from 1 to 65535.

**Sample time (s) — how often to write, not the solver step**
: Default `0.001`. Seconds between rows. A value of 0 or less, including `-1`, uses `0.001`.
  See [Timing and dropped rows](../guides/timing.md#how-often-the-block-writes).

**Buffer capacity (rows)**
: Default `4096`. How many rows the block's buffer holds while they wait to be sent. The buffer
  is reserved in memory when the run starts, and rows arriving while it is full are dropped. `0`
  uses `4096`. When the run ends, the block spends up to 15 seconds sending what is left in the
  buffer, and counts any rows still unsent as dropped.

**Timestamps**
: Default **Simulation time from start**. **Simulation time from start** stamps each row with the
  time the run started plus its simulation time. **Host wall clock** stamps each row with the
  computer's clock when the block writes it. See
  [Timing and dropped rows](../guides/timing.md#the-time-on-each-point).

**Add an output port with the discarded-row count**
: Default off. Adds the output described under [Ports](#ports).

## Environment variables

The block reads these when a run starts. Proxy mode uses neither.

`NOMINAL_TOKEN`
: The API key. Required in direct mode. Set it in MATLAB with `setenv('NOMINAL_TOKEN', '<API key>')`,
  or in the operating system's environment before MATLAB starts.

`NOMINAL_CA_BUNDLE`
: Optional. The path to a PEM file of CA certificates to verify Nominal's HTTPS certificate with.
  Set it when your Nominal instance or your network uses certificates that the operating system
  doesn't trust. Unset or empty uses the operating system's certificates.

## Limits in proxy mode

Each run's channel list travels to `nmproxy` in a single packet, which holds at most 1400 bytes.
The dataset RID plus the channel names have to fit:

28 + (RID length) + the sum over channels of (2 + name length) ≤ 1400

Lengths are in bytes. With a 70-character RID and 10-character names, that is 108
channels. Each name and the RID can be at most 256 bytes, and a block can have at most 169
channels however short the names are. [The frame protocol](frames.md#limits) has the reasoning
behind these limits.

A block that breaks one of the limits stops when the run starts, with
`in proxy mode, the dataset RID and channel names have to fit in one 1400-byte packet to the relay, and these don't`.
