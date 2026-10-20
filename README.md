# nominal-simulink

The Nominal Blockset adds a Simulink block that streams the signals wired into it to a Nominal
dataset while the model runs. It sends to Nominal directly over HTTPS, or through `nmproxy`, a
relay that holds the API key so the computer running the model doesn't need one.

## Install

You need MATLAB R2021b or later with Simulink, on 64-bit Windows, or on 64-bit Linux with glibc
2.28 or later. On a Mac with Apple silicon, you need R2023b or later.

Download `NominalBlockset-<version>.mltbx` from the latest release on the
[releases page](https://github.com/nominal-io/nominal-simulink/releases), and open it in MATLAB
to install it.

To see the block working, open the example model that comes with the toolbox and run it. It
needs no API key and no relay:

```matlab
open_system(fullfile(nominal_root, 'examples', 'nominal_stream_example.slx'))
```

## Next steps

- [Quickstart](docs/quickstart.md): stream three signals from a new model into a dataset.
- [The docs](docs/): the relay, timing, campaigns of simulations, and every parameter and
  message.
- [DESIGN.md](DESIGN.md): how the block works, and why it is built the way it is.
- [PACKAGING.md](PACKAGING.md): building, testing and packaging the blockset from source.
