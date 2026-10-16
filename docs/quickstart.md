# Quickstart

{.lead}
Stream three signals from a new Simulink model into a Nominal dataset.

## 1. Install

Install the Nominal blockset as described in [Install](install.md). The **Nominal Stream Sink**
block then appears in the Simulink Library Browser under **Nominal**.

## 2. Get an API key and your instance's URL

Your API key and your instance's URL are both on your organization's **API keys** page.
[API keys setup](site:/quickstart/quickstart/api-keys-setup/) shows where that page is, how to
create a key, and where to copy the **API URL**.

The block reads the API key from the `NOMINAL_TOKEN` environment variable, so the key is never
saved into the model file. Set it in MATLAB before you run the model:

```matlab
setenv('NOMINAL_TOKEN', '<API key>')
```

`setenv` lasts until MATLAB closes. To keep the key across restarts, set `NOMINAL_TOKEN` in your
operating system's environment instead.

## 3. Create a dataset

The block writes to a dataset that already exists. In the Nominal app, open the **Data sources**
page (in the sidebar, click **Data**, then **Data sources**), click **NEW DATASET**, enter a name,
and click **CREATE DATASET**. On the new dataset's page, hover over **RID** in the **Details**
section and click the copy icon.
[Finding a dataset's RID](site:/quickstart/quickstart/quick/quick-dataset-rid/) has other ways
to get the RID.

## 4. Build the model

Open a new model and drag **Nominal Stream Sink** into it from the Library Browser, or type
`Nominal` on the canvas and pick it from Quick Insert. For a first
run, three source blocks are enough: **Band-Limited White Noise**, **Chirp Signal** and
**Sine Wave**. Wire them into a **Mux** with three inputs, and wire the Mux into the block. Name
the three lines going into the Mux `noise`, `chirp` and `sine`, since the line names become the
channel names in Nominal.

% SCREENSHOT: the model, three named lines into a Mux, the Mux into Nominal Stream Sink (direct)

## 5. Configure the block

Double-click the block, then:

1. Click **Detect from connected signal**. **Channel names (comma-separated)** fills in with
   `'noise, chirp, sine'`.
2. Paste the dataset's RID into **Dataset RID**, inside single quotes: `'<dataset RID>'`. Every
   text field in this dialog takes a MATLAB expression, so text needs the quotes.
3. Set **API base URL** to the API URL from step 2, also in single quotes. The default,
   `'https://api.gov.nominal.io/api'`, is the standard cloud instance.
4. Set **Sample time (s) — how often to write, not the solver step** to `0.01`. The run then
   makes 1001 rows, which fit in the block's buffer however fast the simulation runs.
5. Click **OK**.

Leave **Mode** at **Direct to Nominal** and the other fields at their defaults.

% SCREENSHOT: the block's dialog with the channel names, a placeholder RID, the API base URL and the sample time filled in

## 6. Run the model

Click **Run**. The points cover the run's 10 simulated seconds, starting from the moment the run
started.

Open the dataset in Nominal. The dataset now has the channels `noise`, `chirp` and `sine`.

## If nothing arrives

`Nominal Stream Sink: NOMINAL_TOKEN is not set.` means MATLAB has no API key. Run the `setenv`
line from step 2 in the same MATLAB session, then run the model again.

When Simulink reports that it can't evaluate **Dataset RID** or **API base URL**, the value is
missing its quotes.

A warning when the run ends, `Nominal Stream Sink: <dropped> of <total> rows did not reach Nominal`,
means some rows didn't arrive. When requests failed, the `Last error` at the end of the warning
gives the reason. Check that **API base URL** is the API URL of the instance the dataset belongs
to, and that **Dataset RID** is that dataset's RID.

[Errors and warnings](ref/messages.md) lists every message the block shows.

## Next

- [Choose and name channels](guides/channels.md): how the block maps signals to channels.
- [Timing and dropped rows](guides/timing.md): sample time, timestamps and the buffer.
- [Stream through a proxy](guides/proxy.md): keep the API key off the computer running the model.
