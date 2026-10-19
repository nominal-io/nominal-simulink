# nmproxy

{.lead}
The command line, configuration file and log output of the relay that proxy mode sends to.

## Command line

```shell
nmproxy -config <path to nmproxy.yaml>
nmproxy -version
```

`-config` is required to run the relay. `-version` prints the relay's release and the frame
protocol version it uses, such as `nmproxy 0.1.0, frame protocol version 1`, and exits. A
relay built with a plain `go build` reports its release as `dev`.

`nmproxy` runs until it gets Ctrl+C or `SIGTERM`. It then sends the rows it still holds to
Nominal, logs a `shutting down` line with totals for the whole run, and exits.

## Configuration file

The file is YAML. A key the relay doesn't know stops it at startup. Each release's archive has
`nmproxy.example.yaml`, which describes every key.

`listen`
: Default `"0.0.0.0:15400"`. The UDP address and port to receive packets on, as `host:port`.
  `0.0.0.0` accepts packets on every network interface, and `127.0.0.1` only from the same
  computer.

`nominal.base_url`
: Default `"https://api.gov.nominal.io/api"`, the standard cloud instance. The REST API base URL
  of your Nominal instance: the **API URL** on your organization's **API keys** page.

`nominal.token`
: An API key. Required unless `dry_run` is `true`.

`allowed_datasets`
: Default empty. The RIDs of the datasets the relay may write to. Data for any other dataset is
  dropped. When the list is empty the relay writes to any dataset the API key can, and warns at
  startup.

`idle_timeout`
: Default `5m`. How long a block can go quiet, as it does once its run ends, before the relay
  closes that block's stream. A Go duration such as `30s` or `10m`, greater than zero. Leave the
  key out to use the default.

`stats_interval`
: Default `30s`. How often to log a [stats line](#stats). `0` turns it off.

`log_level`
: Default `info`. One of `debug`, `info`, `warn` or `error`. In a dry run, `debug` logs every
  row.

`dry_run`
: Default `false`. When `true`, the relay decodes and logs what arrives without writing anything
  to Nominal, and needs no `nominal.token` or `allowed_datasets`.

A complete file:

```yaml
listen: "0.0.0.0:15400"

nominal:
  base_url: "<API URL>"
  token: "<API key>"

allowed_datasets:
  - "<dataset RID>"

idle_timeout: 5m
stats_interval: 30s
log_level: "info"
dry_run: false
```

## Errors that stop the relay

Each error is printed as `nmproxy: <message>`. All but the two `receive failed` errors happen at
startup.

| Message | Cause |
|---|---|
| `reading <path>: …` | The file named by `-config` doesn't exist or can't be read. |
| `parsing <path>: …` | The file isn't valid YAML, or has a key the relay doesn't know. |
| `<path>: listen is required` | `listen` is set to an empty string. |
| `<path>: nominal.token is not set. Set it to a Nominal API key, or set dry_run to true` | No API key, and `dry_run` isn't `true`. |
| `<path>: nominal.base_url is required` | `nominal.base_url` is set to an empty string, and `dry_run` isn't `true`. |
| `<path>: idle_timeout cannot be negative` | `idle_timeout` is below zero. |
| `<path>: idle_timeout must be greater than zero. Remove it to use the default of 5 minutes` | `idle_timeout` is zero. |
| `<path>: log_level "<level>" is not one of debug, info, warn, error` | `log_level` has another value. |
| `resolving "<listen>": …` | `listen` isn't a valid address. |
| `listening on "<listen>": …` | The address can't be used, for example because another program has the port. |
| `receive failed 100 times in a row: …` | The relay's UDP socket failed every read while it ran. |
| `receive failed: …` | The relay's UDP socket closed while it ran. |

## Startup warnings

`allowed_datasets is empty, so any sender that can reach the listening port can write to any dataset the API key can. List your dataset RIDs under allowed_datasets.`
: The message says what to do.

`<path> holds an API key and other accounts have access to it (mode <mode>). Run chmod 600 on it.`
: Linux and macOS only.

`could not enlarge the receive buffer, so the operating system may drop bursts of frames`
: Packets arriving in bursts may be lost before the relay reads them, and show up as `gaps`.

## Log

The relay logs one line per event to standard error, as `key=value` pairs:

```
time=<time> level=INFO msg=<message> <fields>
```

### Stats

Every `stats_interval`, the relay logs a `stats` line:

```
time=<time> level=INFO msg=stats configs=1 sessions=1 rows=30000 gaps=0 reorders=0 unknown_config=0 bad_frames=0 wrong_version=0 write_errors=0
```

`configs` and `sessions` are current counts. The rest count what happened since the previous
`stats` line.

| Field | Counts |
|---|---|
| `configs` | Channel lists the relay has registered, one per dataset, list of channel names and set of tags. |
| `sessions` | Model runs the relay is tracking. |
| `rows` | Rows received. |
| `gaps` | Packets that were missing when a later packet arrived. |
| `reorders` | Packets that arrived after a later one, or twice. A late packet was already counted in `gaps` when the later one arrived. |
| `unknown_config` | Packets of data for a channel list the relay hasn't registered. Expected for up to a second after the relay starts, until the block repeats its channel list. They also come from a block whose dataset the relay refused. |
| `bad_frames` | Packets that aren't valid frames or couldn't be read, channel lists whose `config_id` doesn't match their contents, and data packets whose number of channels doesn't match their channel list. |
| `wrong_version` | Packets from a sender using another version of the frame protocol. |
| `write_errors` | Writes to Nominal that failed, and channel lists the relay couldn't open a stream for. A failure in the last batch a stream sends as it closes isn't counted. |

The `shutting down` line reports `rows`, `gaps`, `reorders`, `bad_frames`, `wrong_version`,
`refused` and `write_errors` as totals for the whole run. `refused` counts channel lists for
datasets that aren't in `allowed_datasets`.

### Other messages

| Message | Level | Meaning |
|---|---|---|
| `listening` | INFO | The relay started, with its address. |
| `writing to Nominal` | INFO | The relay will write to `base_url`. |
| `dry run: frames will be decoded and reported, nothing written` | INFO | `dry_run` is `true`. |
| `configuration registered` | INFO | A block started sending a new channel list, with its dataset and number of channels. |
| `stream opened` | INFO | The relay opened a stream to the dataset. |
| `stream opened (dry run: nothing will be written)` | INFO | As above, in a dry run. |
| `session started` | INFO | A model run started sending data. |
| `dropping data for a configuration with no accepted HELLO` | INFO | Data arrived for a channel list the relay hasn't registered. Logged once. |
| `refusing a dataset that is not in allowed_datasets. Add its RID to allowed_datasets if nmproxy should write to it.` | WARN | A block is sending to a dataset that isn't listed. Logged once for each refused dataset, for up to 100 datasets. |
| `rejecting frames that use another frame protocol version. Use an nmproxy release that speaks the sender's version (see nmproxy -version).` | WARN | The blockset and the relay come from different releases. Logged on the first such packet and every 1000th after. |
| `HELLO config_id does not match its contents — the sender's hash disagrees with ours` | WARN | A sender computes `config_id` differently. See {ref}`config_id <config-id>`. |
| `data width does not match the registered configuration` | WARN | A data packet has a different number of channels than its channel list. |
| `a datagram could not be read` | WARN | Reading a packet failed. Logged on the first of a run of failures, which count as `bad_frames`. |
| `could not open a stream for a new configuration` | ERROR | The relay couldn't start writing to the dataset, for example because its RID is malformed. The error says why. |
| `write failed` | ERROR | Writing to Nominal failed, with the dataset and the error. Logged on a stream's first failure and every 100th after. |
| `stream closed with errors` | WARN | A stream closed after some of its writes failed, with the number of failures and the last error. |
| `stream closed` | INFO | A stream closed with no write failures reported. |
| `session idle, forgetting it` | INFO | A run sent nothing for `idle_timeout`. |
| `configuration idle, closing its stream` | INFO | A channel list went unheard for `idle_timeout`. |
| `closing an idle stream failed` | ERROR | Sending the last rows of an idle stream failed. |
| `closing a stream failed` | ERROR | At shutdown, sending a stream's last rows failed. |
| `shutting down` | INFO | The relay is exiting, with totals. |
