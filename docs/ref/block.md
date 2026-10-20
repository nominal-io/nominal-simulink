# Nominal Stream Sink

{.lead}
The block's ports, parameters and the environment variable it reads.

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
**Profile** and **Profile file** in proxy mode, **Proxy host** and **Proxy port** in direct mode,
and **Run title** and **Ref name** when **Create run** is **None**.

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

**Tags (key and value pairs)**
: Default `[]`, no tags. Tags on every channel the block writes, which keep the runs of one
  model apart in a dataset they share. Enter a two-column string array with one row per tag,
  such as `["model" "A"; "execution-id" "7"]`, a cell array of the same shape, a `dictionary`
  or a `containers.Map`. A value can be a number. Keys can't be empty or repeat. See
  [Simulation before hardware](site:/quickstart/quickstart/fundamentals/stories/simulation-first/)
  for how tags and runs fit together.

**Execution ID**
: Default `'auto'`. Sent as the tag `execution-id` on every channel the block writes, so each
  run can be picked out of the dataset. `'auto'` makes a new ID when each run starts, such as
  `20261019-141048-3f2a9c`: the run's start time in UTC, then six hexadecimal digits that differ
  between runs starting in the same second. Text or a number sets the ID yourself, such as a
  variable a script sets for each run. `''` sends no execution ID. **Tags (key and value pairs)**
  can't have an `execution-id` key unless this field is empty. See
  [Run a campaign of simulations](../guides/campaigns.md).

**Mode**
: Default **Direct to Nominal**. **Direct to Nominal** sends over HTTPS from the computer running
  the model. **Via proxy** sends over UDP to `nmproxy`, which writes to Nominal. See
  [Stream through a proxy](../guides/proxy.md).

**Profile**
: Default `'default'`. Direct mode only. The profile in Nominal's config file to read the API key
  and the instance's REST API base URL from, as its `token` and `base_url`. It is the same file
  Nominal's other clients read.
  [API keys setup](site:/quickstart/quickstart/api-keys-setup/) shows what the file holds.

**Profile file**
: Default `''`, the standard location: `~/.config/nominal/config.yml`, or
  `%USERPROFILE%\.config\nominal\config.yml` on Windows. Direct mode only. The path of the config
  file to read **Profile** from, when it is somewhere else.

**Create run**
: Default **None**. **One per execution** creates a run in Nominal when each run of the model
  starts, so it can be watched live, and sets the run's end once the last row is sent. The run
  shows the dataset's points with this execution's tags, **Execution ID** included. **One per
  model** shares one run between every execution with the same title: the first execution creates
  it, later ones extend its end, and it shows the points with the tags in
  **Tags (key and value pairs)**, so every execution's points are in it. Finding the run means
  searching every run whose title contains this one, so the more runs have similar titles, the
  longer an execution takes to start. In direct mode,
  executions that start at the same moment, such as `parsim` workers, can each create the run.
  When a run can't be created or ended, the block warns and the rows still go to the dataset. In
  proxy mode, `nmproxy` creates and ends the run with its own profile, and a run it can't create
  or end shows in [its log](nmproxy.md#log) instead.

**Run title**
: Default `''`, the model's name. One per execution adds the execution ID, as in
  `Model A, execution 7`, or with no execution ID the UTC time the execution started, so each
  execution's run has a title of its own: `'Bench test'` gives `Bench test, execution 7`. One
  per model uses the title as it is. Used only with **Create run**.

**Ref name**
: Default `'default'`. The name the dataset goes under in the run. Workbooks find a channel by
  its ref name and channel name together, so use the ref name your other runs use, such as the
  hardware team's. Used only with **Create run**.

**Proxy host**
: Default `'127.0.0.1'`. Proxy mode only. The name or IP address of the computer running
  `nmproxy`.

**Proxy port**
: Default `15400`. Proxy mode only. The UDP port `nmproxy` listens on, from 1 to 65535.

**Sample time (s) — how often to write, not the solver step**
: Default `0.001`. Seconds between rows, greater than 0. Simulink's `-1`, which inherits a
  sample time, isn't accepted. See
  [Timing and dropped rows](../guides/timing.md#how-often-the-block-writes).

**Buffer capacity (rows)**
: Default `4096`. How many rows the block's buffer holds while they wait to be sent, as a whole
  number greater than 0. The buffer is reserved in memory when the run starts, and rows arriving
  while it is full are dropped. When the run ends, the block spends up to 15 seconds sending
  what is left in the buffer, and counts any rows still unsent as dropped.

**Maximum delay before sending (s)**
: Default `1`. The longest a row waits in the buffer before the block sends it, in seconds
  greater than 0. The block sends sooner as rows arrive faster: in direct mode once 4096 rows
  are waiting, or half the buffer if that is less, and in proxy mode once a packet's worth is,
  as [The frame protocol](frames.md#limits) sizes it. See
  [Timing and dropped rows](../guides/timing.md#how-often-the-block-sends).

**Timestamps**
: Default **Simulation time from start**. **Simulation time from start** stamps each row with the
  time the run started plus its simulation time. **Directly record simulation time (t0=0)**
  stamps each row with its simulation time alone, counted from the Unix epoch, so every run
  starts at the same instant. Choosing it while **Tags (key and value pairs)** and
  **Execution ID** are both empty asks you to confirm, and **Cancel** sets
  **Simulation time from start**. **Host wall clock**
  stamps each row with the computer's clock when the block writes it. See
  [Timing and dropped rows](../guides/timing.md#the-time-on-each-point).

**Add an output port with the discarded-row count**
: Default off. Adds the output described under [Ports](#ports).

## Environment variable

The block reads this when a run starts, in direct mode only.

`NOMINAL_CA_BUNDLE`
: Optional. The path to a PEM file of CA certificates to verify Nominal's HTTPS certificate with.
  Set it when your Nominal instance or your network uses certificates that the operating system
  doesn't trust. Unset or empty uses the operating system's certificates.

## Limits in proxy mode

Each run's channel list travels to `nmproxy` in a single packet, which holds at most 1400 bytes.
The dataset RID, the channel names, the tags, and with **Create run** on the run's title and
**Ref name**, have to fit:

45 + (RID length) + (run title length) + (ref name length) + the sum over channels of
(2 + name length) + the sum over tags of (4 + key length + value length) ≤ 1400

Lengths are in bytes, and the run title and ref name count as 0 when **Create run** is **None**.
**Execution ID** counts as a tag with the key `execution-id`, and an automatic one is 22
characters long. With a 70-character RID, 10-character names, an automatic execution ID as the
only tag and no run, that is 103 channels. The RID, each name, each tag key and value, the
run title and the ref name can each be at most 256 bytes. A block can have at most 64 tags, and
at most 169 channels however short the names are. [The frame protocol](frames.md#limits) has the
reasoning behind these limits.

A block that breaks one of the limits stops when the run starts, with
`in proxy mode, the dataset RID, channel names, tags and run title have to fit in one 1400-byte packet to the relay, and these don't`.
