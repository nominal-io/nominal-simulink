# Streaming from Simulink to Nominal

How to get signals out of a running Simulink model and into a Nominal dataset.

The block sends on a background thread, so no network or TLS work happens in your
model's step function and no memory is allocated there. If the network cannot keep up it
discards samples and tells you, rather than stalling the model — stopping a simulation
that is driving real hardware is worse than losing telemetry.

---

## 1. Install

Open `NominalBlockset-<version>.mltbx` in MATLAB R2021b or later — double-click it, or:

```matlab
matlab.addons.install('NominalBlockset-0.1.0.mltbx')
```

That is all the setup there is. The block appears in the Library Browser under
**Nominal**, its Help button opens its documentation, and the full documentation is in
the Help browser under **Supplemental Software**. To build the toolbox yourself, see
[Building from source](#8-building-from-source).

---

## 2. Put the block in a model

Open the Library Browser, expand **Nominal**, and drag **Nominal Stream Sink** into your
model. Or type *Nominal* on the canvas and pick it from Quick Insert.

Wire the signals you want to record into its single input. For more than one channel use
a **Mux** — the input width must equal the number of channel names you configure, and
values map to channels in the order you list them.

```
   [ Sine Wave  ]──┐
   [ Sine Wave  ]──┼──►[ Mux ]──►[ Nominal Stream Sink ]
   [ Ramp       ]──┘                   (direct)
```

The block label shows which transport it is using, so you can tell at a glance.

---

## 3. Configure it

Double-click the block.

| Field | What it does |
|---|---|
| **Channel names (comma-separated)** | Names as they will appear in Nominal, e.g. `engine.rpm, engine.egt, engine.psi`. The count sets the input port width. Use the button below rather than typing them. |
| **Detect from connected signal** | Reads the names off whatever is wired into the input. See below. |
| **Dataset RID** | An existing Nominal dataset. The block writes points; it does not create datasets. |
| **Mode** | `Direct to Nominal` or `Via proxy`. See below. |
| **API base URL** | Direct mode only. Must match the RID's environment — a `cerulean-staging` RID will not resolve against the production host. |
| **Proxy host / Proxy port** | Proxy mode only. Where the relay is listening. |
| **Sample time (s)** | How often to write. This is *decimation*, not your solver step — a 1 ms solver step with a 10 ms sample time writes every tenth step. |
| **Buffer capacity (rows)** | Rows held in memory while the network drains. Reserved at model start and never grown. At 1 kHz, 60000 rows is a minute of headroom. |
| **Timestamps** | `Simulation time from start` gives a time axis matching the model, which is what an offline run wants. `Host wall clock` is for real-time and HIL. |
| **Add an output port with the discarded-row count** | Off by default. Ticking it adds an output you can wire to a Display to watch loss live. |

Fields the selected mode ignores are greyed out.

### Detecting channel names

Wire up your signals first, then click **Detect from connected signal**. It fills the
channel list in from the model, which is worth doing rather than typing: the list sets
the input port width, so one name too many or too few misaligns every column after it.

What it reads, depending on what you connected:

| Connected | Names come from |
|---|---|
| **Bus Creator** | the bus element names — the best case, since adding a signal later means editing the bus rather than the block |
| **Mux** | each incoming line's signal name, or its source block if the line is unnamed |
| **Bus Selector** | the selected signals |
| anything else | the signal's name, or its source block's |

Duplicates are made unique with a `_2` suffix rather than silently collapsing two
signals into one channel.

One limitation: it does not compile the model, so a button press is instant and works on
a half-finished diagram. The cost is that a *vector* signal of width N carrying a single
name yields one name, not N — there is no way to know the width without compiling. The
block then reports a width mismatch, which is a clear error rather than silent
misalignment; name the elements or use a Mux.

### The token

The access token is read from the `NOMINAL_TOKEN` environment variable and is
deliberately **not** a block parameter — mask values are saved inside the `.slx`, so a
token typed into the dialog would be a credential committed to a model file.

Set it before starting MATLAB, or from the MATLAB prompt:

```matlab
setenv('NOMINAL_TOKEN', '<your token>')
```

**Proxy mode needs no token at all.** The dataset RID travels in the frames and the
credential stays on the relay.

---

## 4. Direct mode

The simple case: the model's machine talks HTTPS to Nominal itself. Nothing else to run.

Set **Mode** to `Direct to Nominal`, fill in the dataset RID and API base URL, set
`NOMINAL_TOKEN`, and run the model.

### Seeing data arrive live

A model normally runs as fast as it can, so a 60-second simulation finishes in about a
second and — with wall-clock timestamps — everything lands in one instant.

For a demonstration, turn on **Run → Simulation Pacing → Enable pacing, 1× real time**.
Simulation time then tracks the clock and you can watch points appear in Nominal while
the model runs.

---

## 5. Proxy mode

Direct mode is cheap in the step function but the background thread still pays for
protobuf encoding, TLS and HTTP on the same machine. On a host already at its
performance limit that can be too much.

Proxy mode moves all of it elsewhere. The model sends compact UDP datagrams; a relay
process does the encoding and the HTTPS. The host is left doing little more than a
`sendto`, and holds no credential.

```
   Simulink host                     relay machine
   ┌───────────────┐   UDP frames   ┌──────────┐   HTTPS
   │ model + block │ ─────────────► │ nmproxy  │ ────────► Nominal
   └───────────────┘                └──────────┘
     no token here                   token here
```

### Configure the relay

```powershell
cd nmproxy
copy nmproxy.example.yaml nmproxy.yaml
```

Edit `nmproxy.yaml`. Every field is documented in the file; the two that matter:

```yaml
nominal:
  token: "<your token>"

allowed_datasets:
  - "ri.catalog.your-env.dataset.0000..."
```

**Set `allowed_datasets`.** Frames are not authenticated, so anything that can reach the
listening port can otherwise write to any dataset the token permits — and tokens are
usually broad. Listing the datasets you actually expect bounds the damage. The daemon
warns at startup if you leave it empty.

`nmproxy.yaml` holds a token, so it is gitignored and should be `chmod 600` on
Unix — the daemon warns if it is not.

### Run it

```powershell
go run ./cmd/nmproxy -config nmproxy.yaml
```

Or build a standalone binary to deploy anywhere:

```powershell
go build -o nmproxy.exe ./cmd/nmproxy
```

It logs a summary line periodically:

```
msg=stats configs=1 sessions=1 rows=2001 gaps=0 reorders=0 bad_frames=0 write_errors=0
```

`gaps` counts datagrams the sequence numbers say never arrived. Non-zero means UDP loss
between the model and the relay.

### Point the block at it

Set **Mode** to `Via proxy`, then **Proxy host** and **Proxy port** to match `listen` in
the config. Nothing else changes — the same channel names, dataset RID and sample time
apply.

### Reading the protocol, or implementing your own relay

There is a second, deliberately readable proxy written in plain MATLAB:

```powershell
just matlab-proxy            # listens on 15400 until Ctrl-C
just matlab-proxy port=15401
```

It decodes frames and prints what arrived; it writes nothing to Nominal. Use it to
confirm a model really is sending, or as a worked example if you need a relay in a
language we do not ship — the decoder is about 150 lines of `.m` with no toolboxes and
no MEX, so the protocol is visible rather than hidden behind a library.

`matlab/nmframe_decode.m` is the decoder, `matlab/nmframe_configid.m` the hash. Both are
checked against the same golden fixtures as the C and Go implementations:

```powershell
just matlab-test
```

### Trying it without a token

To check frames are arriving before involving real credentials, set `dry_run: true` in
the config. The daemon decodes and reports everything and writes nothing, which needs no
token and no allowlist.

---

## 6. When it does not work

**No data appears, no error.** Check the API base URL matches the dataset's environment.
A staging RID against the production host is the usual cause.

**A warning at the end of the run.** The block reports loss when the simulation stops:

```
Warning: Nominal Stream Sink: 412 of 2001 rows did not reach Nominal
(2 requests failed, last HTTP status 0). Last error: Could not connect to server
```

This appears whether or not you enabled the output port. The number after "of" is what
the model produced, so the ratio tells you whether it was a blip or a total failure.

**Rows dropped with no error.** The buffer filled — the model produced faster than the
network drained. Raise **Buffer capacity**, increase **Sample time** to write less often,
or move to proxy mode.

**Nothing reaches the relay.** Confirm the port matches on both sides, and that the
daemon is bound somewhere the model can reach — `listen: "127.0.0.1:15400"` only accepts
from the same machine. Check its log for `bad_frames` or `wrong_version`: a version
mismatch is otherwise indistinguishable from a network fault, because there is no
back-channel to report it.

**Diagnosing a crash or a hang.** Rebuild with callback tracing:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/tools/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DMatlab_ROOT_DIR="C:/Program Files/MATLAB/R2026a" -DNM_SFUN_TRACE=ON
cmake --build build --config Release
```

Each S-function callback then logs on entry, which narrows a fault to one of them.

---

## 7. Checking the build

```powershell
just test          # C library: encoder, ring buffer, frame codec
just sim-test      # the block, on R2021b and R2026a: ports, stepping, proxy, metadata
just install-test  # install the packaged toolbox into a clean session and run it
```

None needs a token or network access.

There is also a command-line streamer, useful for testing the path without Simulink:

```powershell
.\build\cstream\Release\nmstream-demo.exe "<dataset RID>"                    # direct
.\build\cstream\Release\nmstream-demo.exe -p 127.0.0.1:15400 "<dataset RID>" # via relay
```

---

## 8. Building from source

Requires CMake, vcpkg with `curl:x64-windows-static-md` installed, and two MATLAB
releases: the oldest supported (R2021b) builds what ships, because a library opens only
in the release that saved it or later; R2026a packages it.

```powershell
just package    # MEX, library, example and docs, then build/NominalBlockset-<version>.mltbx
```

If MATLAB is somewhere unusual, override either root, forward slashes only:

```powershell
just oldestroot='D:/MATLAB/R2021b' matlabroot='D:/MATLAB/R2026a' package
```

Everything generated lands in `simulink/blockset/`, which is exactly what the toolbox
contains. The `.slx` files and HTML are generated rather than committed — an `.slx` is a
zipped binary, so keeping it out of version control keeps the block's definition
somewhere a diff can review it.

To use the blockset straight from a clone, without installing it:

```matlab
run('C:\sw\nominal-simulink\simulink\nominal_setup.m')
```

The version is set in `simulink/VERSION`. A Linux build of the toolbox, and a release,
come from CI (`.github/workflows/blockset.yml`), which builds the Linux MEX and tests the
installed toolbox on both platforms and both releases.
