# Choose and name channels

{.lead}
Each element of the block's input becomes one channel in the dataset, named from the block's
channel list.

The block has one input, a vector of doubles. The first element is written to the first name in
**Channel names (comma-separated)**, the second element to the second name, and so on, so the
input's width has to equal the number of names. To record several signals, wire them into a
**Mux** or a **Bus Creator**, and that into the block. Signals of other types, such as `single`,
integers or Booleans, need a **Data Type Conversion** block set to `double`. Without one, the
model fails to compile with a data type mismatch.

## Detect the names from the model

Wire the signals up first, then click **Detect from connected signal** in the block's dialog.
The button reads names from the diagram and fills in the channel list, which keeps the names and
the input in the same order. Where the names come from depends on what is wired into the block:

| Wired into the block | Channel names come from |
|---|---|
| **Mux** | Each input line's name, or the name of the block the line comes from if the line has no name. An input fed by a **Bus Creator**, **Bus Selector** or another **Mux** gives the names the other rows describe. An input with nothing connected is named by position: `in1`, `in2` and so on. |
| **Bus Creator** | The bus element names. |
| **Bus Selector** | With **Output as virtual bus** on, every selected signal. With it off, the signal on the output port wired to the block. |
| Any other block | The line's name, or the name of the block it comes from. |

Naming the lines gives the clearest channel names. Without line names, block names such as
`Band-Limited White Noise` become channel names as they are, spaces included.

When two signals have the same name, the second gets `_2` added, the third `_3`, and so on,
skipping any name another signal already has, so each signal gets its own channel. Rename them
in the model and detect again if you want other names.

## Vector signals

**Detect from connected signal** doesn't compile the model, so it can't see how wide a signal
is. A vector signal of width 3 that carries one name gives one channel name, and Simulink then
reports a port width mismatch when the model runs. Split the vector into named lines going into
a **Mux**, or type the three names into the channel list yourself.

## Type the names

The channel list is a MATLAB expression, so keep the quotes when you edit it:

```matlab
'<name 1>, <name 2>, <name 3>'
```

Changing the number of names changes the width of the block's input. Spaces around each name
are ignored.
