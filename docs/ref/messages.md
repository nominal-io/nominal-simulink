# Errors and warnings

{.lead}
Every message the block and its dialog show, what causes it, and what to fix.

When Simulink itself reports that it can't evaluate one of the block's parameters, check that
text values are in quotes. Every text field in the block's dialog takes a MATLAB expression.

## When you apply the dialog or start a run

These errors stop the model from running. Each one starts with `Nominal Stream Sink:`, left off
below. The channel-list errors appear as soon as you click **OK** or **Apply** in the block's
dialog. The rest appear when the run starts.

`the channel list is empty. Enter channel names in quotes, separated by commas, or click Detect from connected signal.`
: **Channel names (comma-separated)** holds no names.

`<field> must be text in quotes, for example <example>.`
: The field holds something other than text, such as a number. Put the value in single or
  double quotes. The block checks **Channel names (comma-separated)** when you apply the dialog,
  and **Dataset RID**, **Proxy host**, **Profile** and **Profile file** when the run starts.

`Dataset RID is empty. Enter the RID of the dataset to write to, in quotes.`
: [Finding a dataset's RID](site:/quickstart/quickstart/quick/quick-dataset-rid/) shows where
  to copy it.

`Profile is empty. Enter the name of a profile in your Nominal config file, such as 'default'.`
: Direct mode reads the API key and URL from a profile, and **Profile** names it.

`could not read the Nominal config file <path>. Create it with a profile holding your API key and base URL, or set Profile file to where yours is.`
: There is no config file at `<path>`, or it can't be opened.
  [API keys setup](site:/quickstart/quickstart/api-keys-setup/) shows how to create it.

`the Nominal config file <path> has no profile named '<profile>'. Check Profile, or add the profile to the file.`
: The file has profiles, but not that one. Profile names are case-sensitive.

`profile '<profile>' in <path> has no token. Add your API key to it as token.`
: The profile has no `token`, or it is `null`.

`profile '<profile>' in <path> has no base_url. Add your instance's API URL to it as base_url.`
: The profile has no `base_url`. It is the **API URL** on your organization's **API keys**
  page.

`profile '<profile>' in <path> has a value too long to read.`
: A `base_url` is over 511 characters, a `token` over 4095, or a `workspace_rid` over 255.

`Ref name is empty. Enter the name the dataset goes under in the run, such as 'default'.`
: **Create run** is on, and the run needs a name for the dataset.

`in proxy mode, Proxy host can't be empty and Proxy port must be from 1 to 65535.`
: Set both to the address `nmproxy` listens on.

`in proxy mode, the dataset RID, channel names and tags have to fit in one 1400-byte packet to the relay, and these don't. Use fewer or shorter channel names or tags.`
: The block sends its whole channel list and its tags to the relay in one packet, and they
  don't fit: there are too many channels or tags, or the names are too long.
  [Limits in proxy mode](block.md#limits-in-proxy-mode) gives the exact rule. Splitting the
  signals across two blocks also works, as does direct mode.

`could not set up sending to Proxy host '<host>'. Check that the name or address is right.`
: In proxy mode, **Proxy host** didn't resolve to an address.

`could not open the stream (could not reserve memory for the buffer).`
: There isn't enough memory for **Buffer capacity (rows)**. Lower it.

`could not open the stream (could not set up the HTTPS client or the socket to the relay).`
: In direct mode, the block couldn't start its HTTPS client.

`could not open the stream (could not start the thread that sends rows).`
: The operating system didn't let the block start the thread that sends its rows.

`out of memory.`
: MATLAB couldn't allocate the block's working memory.

### Tags and Execution ID

These come from reading **Tags (key and value pairs)** and **Execution ID** when the run starts.
Simulink shows each one beneath its own errors about the block's `nominal_sink_tags(tags)` or
`nominal_sink_execid(executionId, tags)` parameter.

`Tags must be pairs of key and value, such as ["model" "A"; "execution-id" "7"], or a dictionary.`
: The field holds something else, such as a single text value or an array with other than two
  columns. [Parameters](block.md#parameters) lists the forms it takes.

`a tag key is empty. Every tag needs a key.`
: A row's first column is empty text.

`the tag key "<key>" appears more than once.`
: Two rows have the same key. Keep one of them.

`the tag text "<text>" contains a line break or another control character.`
: A key or value has a line break in it. Remove it.

`Execution ID must be one text value or number, such as 'run-7' or 7.`
: The field holds an array or a cell array. Give each run one ID.

`Execution ID contains a line break or another control character.`
: Remove the line break.

`Tags has an execution-id key, which Execution ID also sets. Remove one of them.`
: Set the ID in one place: in **Execution ID**, or in **Tags (key and value pairs)** with
  **Execution ID** set to `''`.

## When a run ends

A run that dropped rows ends with a warning in this form:

```
Nominal Stream Sink: execution <ID>: <dropped> of <total> rows did not reach Nominal (<cause>). Last error: <reason>
```

`<ID>` is the run's **Execution ID**, and the `execution <ID>:` part is left out when the block
sends none. `<total>` is every row the model produced in the run, and `<dropped>` is how many of
them never reached Nominal. The `Last error` part appears only when sending failed, and describes the most
recent failure. What to do depends on the cause.

`the buffer filled faster than the network drained it`
: Rows arrived faster than they were sent. See
  [Timing and dropped rows](../guides/timing.md#when-sending-falls-behind).

`<n> requests failed, the last with HTTP status <status>`
: Nominal refused the rows, and the `Last error` has its error message. Check that the
  profile's `base_url` is the API URL of the instance the dataset belongs to, that
  **Dataset RID** is right, and that the profile's `token` is a current API key for that
  instance.

`<n> requests failed, the last with no response`
: The computer couldn't reach the profile's `base_url`, and the `Last error` says why. Check the
  URL and the network. When the `Last error` ends with
  `. Set NOMINAL_CA_BUNDLE to a PEM file of the CA certificates to trust (libcurl/<version> …)`,
  the computer doesn't trust the certificate Nominal presented. See
  [Environment variable](block.md#environment-variable).

`<n> packets could not be sent to the relay`
: In proxy mode, the computer couldn't send to **Proxy host** and **Proxy port**, and the
  `Last error` reads
  `could not send a packet to the relay. Check that it is running and reachable`. Check that
  `nmproxy` is running there and that the address is right.

Rows still waiting when the run ends get up to 15 seconds to be sent. When that time runs out,
the `Last error` is one of these:

- `<n> rows were still unsent when the 15000 ms allowed at close ran out. Check that Nominal is reachable`
- `not sent: the time allowed at close ran out`
- `cut short: the time allowed at close ran out`

All three mean Nominal was slow to answer or couldn't be reached as the run ended.

In proxy mode, this warning covers only rows the block couldn't hand to the network. Anything
lost after that shows up only in [the relay's log](nmproxy.md#log).

## The run in Nominal

With **Create run** on, the block warns when Nominal doesn't take a run request, and the model
carries on. The reason in parentheses is either `HTTP status <status>:` followed by the start
of Nominal's error, or why the computer couldn't reach the profile's `base_url`.

`could not create the run '<title>' (<reason>). The rows still go to the dataset.`
: Appears as the run starts. The rows reach the dataset, and a run can be made over them in
  the Nominal app.

`could not set the end of the run '<title>' (<reason>).`
: Appears as the run ends. The run exists, with no end time, and its end can be set in the
  Nominal app.

## Detect from connected signal

The button's errors appear in a dialog titled **Nominal Stream Sink**, and leave the channel
names as they were. Each one says what to do.

`The block has no input port. Check that Channel names (comma-separated) holds at least one name.`
: The block's input exists only while it has channel names.

`Nothing is connected to the block's input. Wire up the signals first, then click Detect from connected signal.`
: Connect the signals to the block's input, then click the button.

`Could not work out any channel names from the connected signal. Name the signal lines, or type the channel names into Channel names (comma-separated).`
: None of the connected lines or blocks gave a name.
