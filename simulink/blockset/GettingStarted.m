%% Getting Started with the Nominal Blockset
% Stream signals from a running Simulink model into a Nominal dataset.
%
% 1. In the Library Browser, open *Nominal* and drag *Nominal Stream Sink* into your
%    model.
% 2. Wire the signals you want recorded into its input, through a Mux or a Bus Creator.
% 3. Double-click the block and click *Detect from connected signal*.
% 4. Enter the Dataset RID of an existing Nominal dataset.
% 5. Set your access token, then run the model:
%
%      setenv('NOMINAL_TOKEN', '<your token>')
%
% Run this file to open the documentation.

nominal_help
