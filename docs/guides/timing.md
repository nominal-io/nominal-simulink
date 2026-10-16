# Timing and dropped rows

{.lead}
How often the block writes, what time each point gets, and what happens when sending falls
behind.

## How often the block writes

**Sample time (s) — how often to write, not the solver step** sets how often the block takes a
row from its input. With a 1 ms fixed step and a sample time of `0.01`, the block writes on every
tenth step. The default, `0.001`, writes 1000 rows per simulated second.

The block always uses its own sample time. A value of `-1` doesn't inherit one: any value of 0
or less is replaced with `0.001`.

## The time on each point

**Timestamps** has two options.

**Simulation time from start**, the default, stamps each row with the time the run started plus
the row's simulation time. Points keep the model's time axis however fast the simulation runs,
so a 60-second simulation that finishes in one second still covers 60 seconds in Nominal. Use it
for simulations that run faster or slower than real time.

**Host wall clock** stamps each row with the computer's clock at the moment the block writes it.
Use it when the model runs in real time alongside hardware, so its points line up with data
recorded by other systems. A simulation that runs faster than real time puts all its points
into the short time the run actually took.

## When sending falls behind

While the model runs, the block never waits for the network. Each row goes into a buffer, and
the rows are sent from there in the background. If the buffer is full when a new row arrives,
the block drops that row and the model carries on. Rows are also dropped when sending them
fails.

When a run has dropped rows, MATLAB shows a warning as the run ends:

```
Nominal Stream Sink: <dropped> of <total> rows did not reach Nominal (<cause>). Last error: <reason>
```

`<total>` is every row the model produced in the run. `<cause>` says why rows were lost. When it
reads `the buffer filled faster than the network drained it`, the fixes below help. Any other
cause means sending itself went wrong, and the `Last error` says why.
[Errors and warnings](../ref/messages.md#when-a-run-ends) covers each cause.

To watch drops while the model runs, turn on **Add an output port with the discarded-row count**.
The block gets an output carrying the number of rows dropped so far in the run, which you can
wire to a **Display** or a **Scope**.

When the buffer is what fills, any of these helps:

- Raise **Buffer capacity (rows)**, so the buffer rides out longer pauses in sending. In a model
  running in real time at 1000 rows a second, 60000 rows holds a minute. A larger buffer uses
  more memory.
- Write less often, with a larger sample time.
- For a simulation that runs faster than real time, make the buffer hold every row of the run,
  or turn on Simulink's simulation pacing so the simulation keeps to real time.
- Switch to [proxy mode](proxy.md), which moves the work of sending to another computer.

When the run stops, the block sends what is still in its buffer before the simulation ends.
Stopping waits up to 15 seconds for that, and rows still unsent then count as dropped.
