# Install

{.lead}
Install the Nominal Blockset toolbox, which adds the **Nominal Stream Sink** block to Simulink.

You need MATLAB R2021b or later with Simulink, on 64-bit Windows, or on 64-bit Linux with glibc
2.28 or later.

Download `NominalBlockset-<version>.mltbx` from the latest release on the
[releases page](https://github.com/nominal-io/nominal-simulink/releases), and open it in MATLAB
to install it. From the command line, the same install is:

```matlab
matlab.addons.install('<folder>\NominalBlockset-<version>.mltbx')
```

The toolbox puts itself on the MATLAB path and stays installed across restarts. Installing a
newer version replaces the older one.

The block is then in the Simulink Library Browser under **Nominal**, and typing `Nominal` on a
model's canvas finds it through Quick Insert. The block's **Help** button opens its page in the
toolbox's documentation, which is also in the MATLAB help browser under
**Supplemental Software**.

To check the install, open the example model that comes with the toolbox, and run it:

```matlab
open_system(fullfile(nominal_root, 'examples', 'nominal_stream_example.slx'))
```

The example runs without an API key or a relay. It sends to a relay address on the same
computer, where nothing needs to be listening.

When Simulink reports that `S-Function 'nominal_sink_sfun' does not exist`, or MATLAB reports
`Undefined function 'nominal_sink_detect'`, the blockset isn't on the MATLAB path. Check that the
toolbox is installed for the release of MATLAB you're running, and install it again if it isn't.

The relay that [proxy mode](guides/proxy.md) sends to, `nmproxy`, is a separate download on the
same releases page.
