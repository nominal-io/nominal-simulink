%% Getting Started with the Nominal Blockset
% Stream signals from a running Simulink model into a Nominal dataset.
%
% You need an existing dataset in Nominal and its RID, an API key, and your Nominal
% instance's API URL. The API key and the API URL are both on your organization's
% *API keys* page in Nominal.
%
% 1. In the Library Browser, open *Nominal* and drag *Nominal Stream Sink* into your
%    model.
% 2. Wire the signals you want to record into a Mux, and the Mux into the block.
% 3. Double-click the block and click *Detect from connected signal*.
% 4. Enter the dataset's RID in *Dataset RID*, and your instance's API URL in
%    *API base URL*, both in quotes.
% 5. Set your API key, then run the model:
%
%      setenv('NOMINAL_TOKEN', '<API key>')
%
% Run this file to open the documentation.

nominal_help
