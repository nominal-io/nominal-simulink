# Stream through a proxy

{.lead}
Send the model's data to `nmproxy`, a relay that writes it to Nominal, so the computer running
the model holds no API key and does no HTTPS work.

In proxy mode the block sends its rows as UDP packets to `nmproxy`, which can run on another
computer or on the same one. `nmproxy` holds the API key and writes the rows to Nominal. The
computer running the model needs only the dataset's RID and the relay's address. Use proxy mode
when that computer shouldn't hold an API key, such as a shared test bench, or when it has no
processing time to spare for sending data.

```
computer running the model           relay                         Nominal
  Nominal Stream Sink    ── UDP ──►   nmproxy    ── HTTPS ──►
  (no API key)                        (API key)
```

## 1. Get nmproxy

Download `nmproxy` from the latest release on the
[releases page](https://github.com/nominal-io/nominal-simulink/releases). Each release has an
archive per platform, named `nmproxy-<version>-<os>-<arch>`: a `.zip` for Windows on x86-64,
and a `.tar.gz` for Linux on x86-64 or ARM64 and for macOS on Apple silicon. Each archive holds
the `nmproxy` program and `nmproxy.example.yaml`, an example configuration to start from.
`SHA256SUMS`, beside the archives, has their checksums.

To build it from source instead, run `go build ./cmd/nmproxy` from the `nmproxy` folder of the
[nominal-simulink repository](https://github.com/nominal-io/nominal-simulink), with Go 1.25 or
later.

## 2. Configure it

Create `nmproxy.yaml`:

```yaml
nominal:
  base_url: "<API URL>"
  token: "<API key>"

allowed_datasets:
  - "<dataset RID>"
```

The API URL and API key come from your organization's **API keys** page, as in
[API keys setup](site:/quickstart/quickstart/api-keys-setup/).

List every dataset the relay should write to under `allowed_datasets`. `nmproxy` doesn't
authenticate senders, so without the list anything that can reach its port can write to any
dataset your API key can. Data for a dataset that isn't listed is dropped, with the warning
`refusing a dataset that is not in allowed_datasets`.

The file holds your API key in plain text, so keep it out of version control and shared folders.
On Linux and macOS, `nmproxy` warns at startup unless only the file's owner has access to it
(`chmod 600`).

`nmproxy` listens on UDP port 15400 on every network interface. To accept packets on one
interface only, set `listen: "<interface address>:15400"`. [nmproxy](../ref/nmproxy.md) lists
every setting.

## 3. Start it

```shell
./nmproxy -config nmproxy.yaml
```

`nmproxy` logs a `listening` line with its address, and keeps running until you press Ctrl+C.

## 4. Point the block at it

In the block's dialog, set **Mode** to **Via proxy**. Set **Proxy host** to the relay computer's
name or IP address, in single quotes (`'127.0.0.1'` when it runs on the same computer), and
**Proxy port** to the port `nmproxy` listens on (`15400` unless you changed it). **Dataset RID**
and the channel names stay as they were. The block reads no profile in proxy mode, and the dialog
greys out **Profile** and **Profile file**.

Run the model. `nmproxy` logs `configuration registered` with the dataset and the number of
channels, then `session started`.

## 5. Read the stats line

Every 30 seconds, `nmproxy` logs a summary:

```
time=<time> level=INFO msg=stats configs=1 sessions=1 rows=30000 gaps=0 reorders=0 unknown_config=0 bad_frames=0 wrong_version=0 write_errors=0
```

The counts after `sessions` are since the previous summary. `rows` above 0 means data is reaching
the relay. `gaps` counts packets that went missing between the model and the relay, and
`write_errors` counts writes to Nominal that failed. [nmproxy](../ref/nmproxy.md#stats)
describes every field.

The block's own end-of-run warning covers only rows it couldn't hand to the network. Anything
lost after that shows up only in the relay's log.

## Check the connection without an API key

A configuration file containing only `dry_run: true` makes `nmproxy` decode and log what
arrives, without writing anything to Nominal. A dry run needs no API key and no
`allowed_datasets`. When the model starts sending, the log shows
`stream opened (dry run: nothing will be written)` with the dataset and the number of channels.
Adding `log_level: debug` logs every row.

## When data doesn't reach Nominal

When `rows` stays at 0, packets aren't reaching the relay. `nmproxy` with `listen` set to
`127.0.0.1` accepts packets only from its own computer, so when the model runs elsewhere, use
the relay's network address or leave `listen` unset. A firewall on the relay computer also has
to allow UDP on the port.

`wrong_version` above 0 means the blockset and `nmproxy` come from different releases.
`nmproxy -version` prints the frame protocol version the relay uses.

`unknown_config` rising while nothing is written means the relay never registered the block's
channel list. Either the dataset isn't in `allowed_datasets`, and the relay logs a
`refusing a dataset that is not in allowed_datasets` warning that names it, or the relay
couldn't start writing to the dataset, and it logs `could not open a stream for a new configuration`
with the error. A malformed RID causes the second.

When `rows` climbs but `write_errors` does too, the relay's `write failed` lines give Nominal's
reason. Check `base_url`, the API key and the dataset RID against the **API keys** page and the
dataset.
