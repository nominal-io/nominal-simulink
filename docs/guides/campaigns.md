# Run a campaign of simulations

{.lead}
Run a model many times into one dataset, with every run starting at time zero and tagged so the
runs stay apart.

[Simulation before hardware](site:/quickstart/quickstart/fundamentals/stories/simulation-first/)
describes this way of organizing simulation data: one dataset for the model, every run written
from the same time zero, and each run's points tagged with what identifies it. The block does
each part.

## Set up the block

1. Set **Dataset RID** to the model's dataset.
2. Set **Timestamps** to **Directly record simulation time (t0=0)**.
3. Leave **Execution ID** at `'auto'`. Each run then gets its own `execution-id` tag, made when
   the run starts.
4. In **Tags (key and value pairs)**, add anything else that identifies a run, such as
   `["model" "A"]`.

Every run then starts at the Unix epoch, so in a workbook the runs overlay without shifting, and
the `execution-id` tag picks out one run's points. In a workbook, **Split by tags** on
`execution-id` plots each run as its own series, as
[Large campaigns](site:/quickstart/quickstart/fundamentals/organization/simulation/#large-campaigns)
shows.

## Set each run from a script

The block's fields take MATLAB expressions, so a field can name a variable that a script sets
for each run. Set **Execution ID** to `execId` and **Tags (key and value pairs)** to `nmTags`,
then give each run its values with `Simulink.SimulationInput`:

```matlab
for k = 1:50
    in(k) = Simulink.SimulationInput('<model>');
    in(k) = in(k).setVariable('execId', k);
    in(k) = in(k).setVariable('nmTags', ["parameter-set" string(<parameter set of run k>)]);
end
out = sim(in);
```

The runs get the execution IDs 1 to 50. When a run drops rows, the warning at its end names its
execution ID, as [Timing and dropped rows](timing.md#when-sending-falls-behind) shows.
