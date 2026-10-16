# Nominal for Simulink

{.lead}
Stream signals from a running Simulink model into a Nominal dataset.

```{toctree}
:hidden:
:caption: Guides

Overview <self>
quickstart
install
guides/channels
guides/timing
guides/proxy
```

```{toctree}
:hidden:
:caption: Reference

ref/index
```

The **Nominal Stream Sink** block writes the signals wired into it to a dataset while the model
runs. Each signal becomes a channel in the dataset. While the model runs, the block never waits
for the network: if sending falls behind, it drops rows rather than slowing the model, and says
how many at the end of the run.

The block's **Mode** parameter chooses how data leaves the model. **Direct to Nominal** sends it
over HTTPS from the computer running the model, using an API key on that computer.
**Via proxy** sends it over UDP to `nmproxy`, a relay you run on another computer or the same
one, and the relay writes it to Nominal. The API key then lives on the relay. Use proxy mode
when the computer running the model shouldn't hold an API key, or has no processing time to
spare for sending data.

::::{grid} 1 2 2 2
:gutter: 2

:::{grid-item-card} Quickstart
:link: quickstart
:link-type: doc

Stream three signals from a new model into a dataset.
:::

:::{grid-item-card} Stream through a proxy
:link: guides/proxy
:link-type: doc

Run `nmproxy` so the computer running the model needs no API key.
:::

:::{grid-item-card} Timing and dropped rows
:link: guides/timing
:link-type: doc

How often the block writes, the time on each point, and what to do when rows are dropped.
:::

:::{grid-item-card} Reference
:link: ref/index
:link-type: doc

Every parameter, message, relay setting and the frame protocol.
:::
::::
