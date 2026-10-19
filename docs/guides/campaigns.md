# Run a campaign of simulations

{.lead}
Execute a model many times into one dataset, with every execution starting at time zero, tagged
so the executions stay apart, and each one a run in Nominal.

[Simulation before hardware](site:/quickstart/quickstart/fundamentals/stories/simulation-first/)
describes this way of organizing simulation data: one dataset for the model, every execution
written from the same time zero, each execution's points tagged with what identifies it, and a
run for each execution. The block does each part.

## Set up the block

1. Set **Dataset RID** to the model's dataset.
2. Set **Timestamps** to **Directly record simulation time (t0=0)**.
3. Leave **Execution ID** at `'auto'`. Each execution then gets its own `execution-id` tag,
   made when it starts.
4. In **Tags (key and value pairs)**, add anything else that identifies an execution, such as
   `["model" "A"]`.
5. Set **Create run** to **One per execution**. Each execution then becomes a run titled with
   the model's name and its execution ID, showing only its own points.

Every execution starts at the Unix epoch, so in a workbook the runs overlay without shifting.
Open a workbook from one execution's run and add the others, as
[Simulation before hardware](site:/quickstart/quickstart/fundamentals/stories/simulation-first/#compare-executions)
describes.

For hundreds or thousands of executions, set **Create run** to **One per model** instead. Every
execution then goes into one run, and in a workbook **Split by tags** on `execution-id` plots
each execution as its own series, as
[Large campaigns](site:/quickstart/quickstart/fundamentals/organization/simulation/#large-campaigns)
shows.

## Set each execution from a script

The block's fields take MATLAB expressions, so a field can name a variable that a script sets
for each execution. Set **Execution ID** to `execId` and **Tags (key and value pairs)** to
`nmTags`, then give each execution its values with `Simulink.SimulationInput`:

```matlab
for k = 1:50
    in(k) = Simulink.SimulationInput('<model>');
    in(k) = in(k).setVariable('execId', k);
    in(k) = in(k).setVariable('nmTags', ["parameter-set" string(<parameter set of execution k>)]);
end
out = sim(in);
```

The executions get the execution IDs 1 to 50. When an execution drops rows, the warning at its
end names its execution ID, as [Timing and dropped rows](timing.md#when-sending-falls-behind)
shows.
